// src/web/Server.cpp: the web library's server (Server.h has its design, Web.h what it does and its C functions):
// routes, slots, connections and the nine rules, on llhttp and the I/O thread of Web.cpp. A build without
// FMINUS_WEB_SERVER compiles nothing here. fast_float is included in this one file of the library, hidden, as the
// runtime's Numbers.cpp includes it (after the standard headers it uses, which keep their own visibility).
#include "web/Server.h"

#if FMINUS_HAS_WEB_SERVER

#include "web/Handle.h"
#include "web/Sockets.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <version>

#include <curl/curl.h>
#include <llhttp.h>

#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
#include <fast_float/fast_float.h>
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

namespace fminus::web
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::int32_t kMaxHeadBytes = 16384;   // a request's head: rule 2
        constexpr std::int32_t kMaxFields = 64;
        constexpr std::int32_t kMaxArgs = 8;            // %d's a route may have
        constexpr std::size_t kReadChunk = 16384;
        constexpr std::size_t kMaxBuffered = 32768;     // bytes held unparsed while a request is being answered
        constexpr std::size_t kKeptBody = 65536;        // a free slot keeps a body's memory up to this much
        constexpr auto kAcceptPause = std::chrono::milliseconds(100);   // after accept fails (out of descriptors)

        // ---- methods and routes -------------------------------------------------------------------------------------

        enum class Method : std::uint8_t
        {
            Get,
            Head,
            Post,
            Put,
            Patch,
            Delete,
            Options,
            None,
        };

        constexpr std::string_view kMethodNames[] = {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};

        Method methodNamed(std::string_view name)
        {
            for (std::size_t i = 0; i < std::size(kMethodNames); ++i)
            {
                if (name == kMethodNames[i])
                    return static_cast<Method>(i);
            }
            return Method::None;
        }

        Method methodOf(std::uint8_t method)
        {
            switch (method)
            {
            case HTTP_GET:
                return Method::Get;
            case HTTP_HEAD:
                return Method::Head;
            case HTTP_POST:
                return Method::Post;
            case HTTP_PUT:
                return Method::Put;
            case HTTP_PATCH:
                return Method::Patch;
            case HTTP_DELETE:
                return Method::Delete;
            case HTTP_OPTIONS:
                return Method::Options;
            default:
                return Method::None;
            }
        }

        // A route literal, "METHOD /path": its method and its path's pattern; false when it is not one (a method of
        // the seven, one space, a path that starts with "/").
        bool readRoute(std::string_view text, Method& method, std::string_view& pattern)
        {
            const std::size_t space = text.find(' ');
            if (space == std::string_view::npos)
                return false;
            method = methodNamed(text.substr(0, space));
            pattern = text.substr(space + 1);
            return method != Method::None && !pattern.empty() && pattern.front() == '/';
        }

        // Whether `method`, a route's, admits a request's: itself, and a GET route a HEAD request.
        bool admits(Method route, Method request)
        {
            return route == request || (route == Method::Get && request == Method::Head);
        }

        struct Route
        {
            Method method;
            std::string pattern;
        };

        bool isDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        char lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool equalsIgnoringCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (lower(a[i]) != lower(b[i]))
                    return false;
            }
            return true;
        }

        // ---- slots ----------------------------------------------------------------------------------------------------

        // A slot's phase, the low 8 bits of its state; the bits above are the sequence number of the request it holds.
        enum Phase : std::uint64_t
        {
            Free = 0,        // the I/O thread's
            Published = 1,   // a request waits: the I/O thread's store; a taker's compare-and-swap claims it
            Claimed = 2,     // a taker's, to read and answer
            Answered = 3,    // the reply is written: the I/O thread sends it and frees the slot
            Abandoned = 4,   // claimed, and given up on by the I/O thread (the client left, the reply timed out, the
                             // server closed): the taker's still, until its reply makes it Done
            Done = 5,        // the I/O thread frees it
            Closed = 6,      // the server is closed: never used again
        };
        constexpr std::uint64_t kPhaseMask = 0xff;

        Phase phaseOf(std::uint64_t word)
        {
            return static_cast<Phase>(word & kPhaseMask);
        }

        std::uint64_t withPhase(std::uint64_t word, Phase phase)
        {
            return (word & ~kPhaseMask) | phase;
        }

    } // namespace

    struct Slot;

    namespace
    {
        // The headers the rules read; every other one is counted and skipped.
        enum class Header : std::uint8_t
        {
            Host,
            Origin,
            FetchSite,
            Authorization,
            ContentType,
            Expect,
            Upgrade,
            TransferEncoding,
            Other,
        };
        constexpr std::size_t kHeaders = static_cast<std::size_t>(Header::Other);

        constexpr std::string_view kHeaderNames[kHeaders] = {
            "host", "origin", "sec-fetch-site", "authorization", "content-type", "expect", "upgrade", "transfer-encoding"};

        struct Connection
        {
            enum class Stage
            {
                Head,      // reading a request's head (or waiting for its first byte)
                Body,      // reading its body
                Program,   // its slot is published or claimed
                Writing,   // a response is going out
                Idle,      // kept alive, nothing on it yet
            };

            Server* server = nullptr;
            sockets::Socket socket = sockets::kNone;
            Address local;
            int localPort = 0;
            llhttp_t parser{};
            Stage stage = Stage::Head;
            Clock::time_point deadline;
            std::string in;     // received, not yet parsed
            std::string out;    // the response going out
            std::size_t sent = 0;
            bool closeAfter = false;
            bool peerClosed = false;
            bool dead = false;

            // The request being read.
            bool started = false;   // a byte of it has arrived
            std::int32_t headBytes = 0;
            std::int32_t fields = 0;
            Method method = Method::None;
            std::string target;
            std::string field;
            Header current = Header::Other;
            std::string value;
            std::array<std::string, kHeaders> headers;
            std::array<std::uint8_t, kHeaders> counts{};
            std::vector<char> body;
            std::int32_t status = 0;   // the library's answer, set by a callback that refused the request
            std::string allow;         // a 405's
            std::string extra;         // a response's other headers
            const ServerPolicy::File* file = nullptr;
            bool messageDone = false;
            bool keepAlive = false;
            Slot* slot = nullptr;
        };

    } // namespace

    struct Slot : Handle
    {
        static constexpr Kind kKind = Kind::Received;
        Slot()
            : Handle(Kind::Received)
        {
        }

        std::atomic<std::uint64_t> state{Free};

        // The request: written by the I/O thread while the slot is free, unchanged while it is published, claimed or
        // abandoned. Its buffers are made with the slot, so that a taker never reads memory that moves.
        Method method = Method::Get;
        std::unique_ptr<char[]> target;   // the path, and "?" and the query: kMaxHeadBytes
        std::int32_t pathLength = 0;
        std::int32_t targetLength = 0;
        std::vector<char> body;           // with a 0 byte after its last
        std::int32_t bodySize = 0;

        // The taker's.
        std::int32_t args[kMaxArgs] = {};
        std::int32_t argCount = 0;

        // The reply: the taker's to write while it holds the slot, the I/O thread's once it is answered.
        std::int32_t replyStatus = 0;
        std::int32_t replyBytes = 0;
        std::int32_t typeLength = 0;
        char type[255] = {};
        std::unique_ptr<char[]> buffer;   // maxReplyBytes, from the start
        std::int32_t capacity = 0;

        // The I/O thread's.
        Connection* connection = nullptr;
        Clock::time_point deadline;
    };

    struct Server : Handle
    {
        static constexpr Kind kKind = Kind::Server;
        Server()
            : Handle(Kind::Server)
        {
        }

        Context::State* context = nullptr;
        const ServerSetup* setup = nullptr;
        const ServerPolicy* limits = nullptr;
        std::vector<Route> routes;
        std::vector<std::unique_ptr<Slot>> slots;   // never changes once made

        std::atomic<std::int32_t> port{FMINUS_WEB_REFUSED};

        // fminus_web_http_accept's.
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<std::int32_t> waiters{0};
        bool closed = false;   // under the mutex
        bool dying = false;    // under the mutex

        // The maker's until it is attached, then the I/O thread's.
        std::vector<sockets::Socket> listeners;
        std::vector<Clock::time_point> acceptAgain;   // a listener's pause after accept failed
        std::vector<std::unique_ptr<Connection>> connections;
        std::uint64_t sequence = 0;
    };

    namespace
    {
        // ---- connections ----------------------------------------------------------------------------------------------

        Connection& connectionOf(llhttp_t* parser)
        {
            return *static_cast<Connection*>(parser->data);
        }

        // A callback's refusal: the status the library answers with.
        int refuse(Connection& c, std::int32_t status)
        {
            c.status = status;
            return -1;
        }

        int refuseData(Connection& c, std::int32_t status)
        {
            c.status = status;
            return HPE_USER;
        }

        const ServerPolicy::File* fileFor(const Server& s, std::string_view path)
        {
            for (const ServerPolicy::File& file : s.limits->files)
            {
                if (file.path == path)
                    return &file;
            }
            return nullptr;
        }

        std::string_view pathOf(const Connection& c)
        {
            return std::string_view(c.target).substr(0, c.target.find('?'));
        }

        // Rule 7: "Bearer " (the scheme's letter case aside) and the token, compared in time that depends on the
        // token's length alone.
        bool tokenMatches(std::string_view presented, const std::string& token)
        {
            constexpr std::string_view scheme = "Bearer ";
            if (presented.size() < scheme.size() || !equalsIgnoringCase(presented.substr(0, scheme.size()), scheme))
                return false;
            const std::string_view given = presented.substr(scheme.size());
            std::size_t difference = given.size() ^ token.size();
            for (std::size_t i = 0; i < token.size(); ++i)
            {
                const unsigned char g = i < given.size() ? static_cast<unsigned char>(given[i]) : 0;
                difference |= static_cast<std::size_t>(g ^ static_cast<unsigned char>(token[i]));
            }
            return difference == 0;
        }

        bool formType(std::string_view type)
        {
            std::string_view essence = type.substr(0, type.find(';'));
            while (!essence.empty() && (essence.back() == ' ' || essence.back() == '\t'))
                essence.remove_suffix(1);
            return equalsIgnoringCase(essence, "application/x-www-form-urlencoded") ||
                   equalsIgnoringCase(essence, "multipart/form-data") || equalsIgnoringCase(essence, "text/plain");
        }

        // ---- llhttp's callbacks: rule 2 as the head arrives, rules 3 to 8 when it is complete ------------------------

        int onMethodComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            c.method = methodOf(llhttp_get_method(parser));
            return c.method == Method::None ? refuse(c, 501) : 0;
        }

        int onUrl(llhttp_t* parser, const char* at, std::size_t length)
        {
            Connection& c = connectionOf(parser);
            c.target.append(at, length);   // within the head's bytes, which the caller bounds
            return 0;
        }

        // The target: origin form, a path rule 2 allows, a query of printable ASCII without '#'.
        int onUrlComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            const std::size_t question = c.target.find('?');
            if (!servablePath(std::string_view(c.target).substr(0, question)))
                return refuse(c, 400);
            if (question != std::string::npos)
            {
                for (std::size_t i = question + 1; i < c.target.size(); ++i)
                {
                    const auto byte = static_cast<unsigned char>(c.target[i]);
                    if (byte <= 0x20 || byte >= 0x7f || byte == '#')
                        return refuse(c, 400);
                }
            }
            return 0;
        }

        int onVersionComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            return parser->http_major == 1 && parser->http_minor <= 1 ? 0 : refuse(c, 505);
        }

        int onHeaderField(llhttp_t* parser, const char* at, std::size_t length)
        {
            Connection& c = connectionOf(parser);
            if (c.field.size() < 32)   // no header the rules read has a longer name
                c.field.append(at, std::min<std::size_t>(length, 32 - c.field.size() + 1));
            return 0;
        }

        int onHeaderFieldComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            if (++c.fields > kMaxFields)
                return refuse(c, 431);
            c.current = Header::Other;
            for (std::size_t i = 0; i < kHeaders; ++i)
            {
                if (equalsIgnoringCase(c.field, kHeaderNames[i]))
                    c.current = static_cast<Header>(i);
            }
            c.field.clear();
            c.value.clear();
            return 0;
        }

        int onHeaderValue(llhttp_t* parser, const char* at, std::size_t length)
        {
            Connection& c = connectionOf(parser);
            if (c.current != Header::Other)
                c.value.append(at, length);
            return 0;
        }

        int onHeaderValueComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            if (c.current == Header::Other)
                return 0;
            while (!c.value.empty() && (c.value.back() == ' ' || c.value.back() == '\t'))
                c.value.pop_back();
            const auto i = static_cast<std::size_t>(c.current);
            if (c.counts[i] < 255)
                ++c.counts[i];
            c.headers[i] = std::move(c.value);
            c.value.clear();
            return 0;
        }

        std::size_t countOf(const Connection& c, Header header)
        {
            return c.counts[static_cast<std::size_t>(header)];
        }

        const std::string& headerOf(const Connection& c, Header header)
        {
            return c.headers[static_cast<std::size_t>(header)];
        }

        int onHeadersComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            Server& s = *c.server;
            const ServerPolicy& limits = *s.limits;
            // Rule 2's rest.
            if (parser->http_major != 1 || parser->http_minor > 1)
                return refuse(c, 505);
            if ((parser->flags & F_TRANSFER_ENCODING) != 0 || countOf(c, Header::TransferEncoding) != 0)
                return refuse(c, 501);
            if (parser->upgrade != 0 || (parser->flags & (F_UPGRADE | F_CONNECTION_UPGRADE)) != 0 ||
                countOf(c, Header::Upgrade) != 0)
                return refuse(c, 400);
            if (countOf(c, Header::Expect) != 0)
                return refuse(c, 417);
            const bool hasBody = (parser->flags & F_CONTENT_LENGTH) != 0 && parser->content_length > 0;
            // Rule 3.
            const std::string& host = headerOf(c, Header::Host);
            if (countOf(c, Header::Host) != 1 || !hostAllowed(*s.setup, host, c.local, c.localPort))
                return refuse(c, 403);
            // Rule 4.
            const std::string_view path = pathOf(c);
            if (c.method == Method::Get || c.method == Method::Head)
            {
                if (const ServerPolicy::File* file = fileFor(s, path))
                {
                    c.file = file;
                    if (hasBody)
                        return refuse(c, 200);   // answered with the file, and the connection closed
                    c.keepAlive = parser->http_minor == 1 && llhttp_should_keep_alive(parser) != 0;
                    return 0;
                }
            }
            // Rule 5.
            if (countOf(c, Header::FetchSite) > 1 || countOf(c, Header::Origin) > 1)
                return refuse(c, 403);
            if (countOf(c, Header::FetchSite) == 1)
            {
                const std::string& site = headerOf(c, Header::FetchSite);
                if (site != "same-origin" && site != "none")
                    return refuse(c, 403);
            }
            if (countOf(c, Header::Origin) == 1 && !originAllowed(headerOf(c, Header::Origin), host))
                return refuse(c, 403);
            if (countOf(c, Header::ContentType) > 1 ||
                (countOf(c, Header::ContentType) == 1 && formType(headerOf(c, Header::ContentType))))
                return refuse(c, 415);
            // Rule 6.
            bool pathKnown = false;
            bool methodKnown = false;
            std::uint32_t allowed = 0;
            std::int32_t args[kMaxArgs];
            std::int32_t count = 0;
            for (const Route& route : s.routes)
            {
                if (!matchPath(route.pattern, path, args, count))
                    continue;
                pathKnown = true;
                allowed |= 1u << static_cast<unsigned>(route.method);
                if (route.method == Method::Get)
                    allowed |= 1u << static_cast<unsigned>(Method::Head);
                methodKnown = methodKnown || admits(route.method, c.method);
            }
            if (fileFor(s, path) != nullptr)
            {
                pathKnown = true;
                allowed |= (1u << static_cast<unsigned>(Method::Get)) | (1u << static_cast<unsigned>(Method::Head));
            }
            if (!pathKnown)
                return refuse(c, 404);
            if (!methodKnown)
            {
                c.allow.clear();
                for (std::size_t i = 0; i < std::size(kMethodNames); ++i)
                {
                    if ((allowed & (1u << i)) == 0)
                        continue;
                    if (!c.allow.empty())
                        c.allow += ", ";
                    c.allow += kMethodNames[i];
                }
                return refuse(c, 405);
            }
            // Rule 7.
            if (!limits.token.empty() &&
                (countOf(c, Header::Authorization) != 1 ||
                 !tokenMatches(headerOf(c, Header::Authorization), limits.token)))
                return refuse(c, 401);
            // Rule 8: the body's length, before a byte of it is read.
            if (hasBody && parser->content_length > static_cast<std::uint64_t>(limits.maxRequestBytes))
                return refuse(c, 413);
            c.keepAlive = parser->http_minor == 1 && llhttp_should_keep_alive(parser) != 0;
            if (hasBody)
                c.body.reserve(static_cast<std::size_t>(parser->content_length) + 1);
            c.stage = Connection::Stage::Body;
            return 0;
        }

        int onBody(llhttp_t* parser, const char* at, std::size_t length)
        {
            Connection& c = connectionOf(parser);
            if (c.file != nullptr)
                return refuseData(c, 400);   // a file's request has no body (rule 4 refused one already)
            c.body.insert(c.body.end(), at, at + length);   // llhttp stops at Content-Length, checked above
            return 0;
        }

        // The request is whole: llhttp stops here (a pause), so that bytes after it wait until it is answered.
        int onMessageComplete(llhttp_t* parser)
        {
            Connection& c = connectionOf(parser);
            c.messageDone = true;
            return HPE_PAUSED;
        }

        const llhttp_settings_t& settings()
        {
            static const llhttp_settings_t instance = []
            {
                llhttp_settings_t s;
                llhttp_settings_init(&s);
                s.on_method_complete = onMethodComplete;
                s.on_url = onUrl;
                s.on_url_complete = onUrlComplete;
                s.on_version_complete = onVersionComplete;
                s.on_header_field = onHeaderField;
                s.on_header_field_complete = onHeaderFieldComplete;
                s.on_header_value = onHeaderValue;
                s.on_header_value_complete = onHeaderValueComplete;
                s.on_headers_complete = onHeadersComplete;
                s.on_body = onBody;
                s.on_message_complete = onMessageComplete;
                return s;
            }();
            return instance;
        }

        // ---- responses ----------------------------------------------------------------------------------------------

        std::string_view reason(std::int32_t status)
        {
            switch (status)
            {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 301: return "Moved Permanently";
            case 302: return "Found";
            case 303: return "See Other";
            case 304: return "Not Modified";
            case 307: return "Temporary Redirect";
            case 308: return "Permanent Redirect";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 409: return "Conflict";
            case 410: return "Gone";
            case 413: return "Content Too Large";
            case 415: return "Unsupported Media Type";
            case 417: return "Expectation Failed";
            case 422: return "Unprocessable Content";
            case 429: return "Too Many Requests";
            case 431: return "Request Header Fields Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 503: return "Service Unavailable";
            case 505: return "HTTP Version Not Supported";
            default: return "";
            }
        }

        enum class Made
        {
            Library,   // the library's own answer, or the program's: Cache-Control: no-store
            File,      // a static file: the headers that keep other sites from framing it
        };

        // Puts a response in the connection's output and makes it the connection's stage. `body` is sent unless the
        // request was a HEAD (its Content-Length is the body's all the same). Throws std::bad_alloc.
        void respond(Connection& c, std::int32_t status, std::string_view type, std::string_view body, Made made,
                     bool keepAlive, Clock::time_point now)
        {
            std::string& out = c.out;
            out.clear();
            c.sent = 0;
            char number[16];
            out += "HTTP/1.1 ";
            out.append(number, std::to_chars(number, number + sizeof number, status).ptr);
            out += ' ';
            out += reason(status);
            out += "\r\n";
            if (!type.empty())
            {
                out += "Content-Type: ";
                out += type;
                out += "\r\n";
            }
            if (status != 204 && status != 304)
            {
                out += "Content-Length: ";
                out.append(number, std::to_chars(number, number + sizeof number, body.size()).ptr);
                out += "\r\n";
            }
            out += "X-Content-Type-Options: nosniff\r\nCross-Origin-Resource-Policy: same-origin\r\n";
            if (made == Made::File)
                out += "X-Frame-Options: DENY\r\nContent-Security-Policy: frame-ancestors 'none'\r\n"
                       "Referrer-Policy: same-origin\r\n";
            else
                out += "Cache-Control: no-store\r\n";
            out += c.extra;
            if (!keepAlive)
                out += "Connection: close\r\n";
            out += "\r\n";
            if (c.method != Method::Head && status != 204 && status != 304)
                out += body;
            c.extra.clear();
            c.stage = Connection::Stage::Writing;
            c.closeAfter = !keepAlive;
            c.deadline = now + std::chrono::milliseconds(c.server->limits->requestTimeoutMs);
        }

        // The library's answer, for a request the rules refused: no body, and the connection closed after it.
        void refuseRequest(Connection& c, std::int32_t status, Clock::time_point now)
        {
            if (status == 405)
                c.extra = "Allow: " + c.allow + "\r\n";
            else if (status == 401)
                c.extra = "WWW-Authenticate: Bearer\r\n";
            respond(c, status, {}, {}, Made::Library, false, now);
        }

        void serveFile(Connection& c, bool keepAlive, Clock::time_point now)
        {
            respond(c, 200, c.file->type, c.file->bytes, Made::File, keepAlive, now);
        }

        // The state of a new request on the connection.
        void resetRequest(Connection& c)
        {
            c.started = !c.in.empty();
            c.headBytes = 0;
            c.fields = 0;
            c.method = Method::None;
            c.target.clear();
            c.field.clear();
            c.current = Header::Other;
            c.value.clear();
            for (std::string& header : c.headers)
                header.clear();
            c.counts.fill(0);
            c.body.clear();
            if (c.body.capacity() > kKeptBody)
                c.body.shrink_to_fit();
            c.status = 0;
            c.allow.clear();
            c.extra.clear();
            c.file = nullptr;
            c.messageDone = false;
            c.keepAlive = false;
        }

        // ---- slots, the I/O thread's side ---------------------------------------------------------------------------

        // A slot back to free: its request's memory kept (a body's up to kKeptBody), its links cut.
        void freeSlot(Slot& slot, std::uint64_t word)
        {
            slot.body.clear();
            if (slot.body.capacity() > kKeptBody)
                slot.body.shrink_to_fit();
            slot.connection = nullptr;
            slot.state.store(withPhase(word, Free), std::memory_order_release);
        }

        // The I/O thread gives up on the slot's request: a published one is free again, a claimed one abandoned (its
        // taker's still). False when it was answered meanwhile (or was neither).
        bool giveUp(Slot& slot)
        {
            for (;;)
            {
                std::uint64_t word = slot.state.load(std::memory_order_acquire);
                const Phase phase = phaseOf(word);
                if (phase == Published)
                {
                    if (slot.state.compare_exchange_weak(word, withPhase(word, Free), std::memory_order_acq_rel))
                    {
                        freeSlot(slot, word);
                        return true;
                    }
                }
                else if (phase == Claimed)
                {
                    if (slot.state.compare_exchange_weak(word, withPhase(word, Abandoned), std::memory_order_acq_rel))
                    {
                        slot.connection = nullptr;
                        return true;
                    }
                }
                else
                    return false;
            }
        }

        void wakeAcceptors(Server& s)
        {
            if (s.waiters.load(std::memory_order_seq_cst) > 0)
            {
                {
                    std::lock_guard lock(s.mutex);
                }
                s.changed.notify_all();
            }
        }

        // Rule 9: a free slot for the connection's whole request, published; or 503.
        void publish(Server& s, Connection& c, Clock::time_point now)
        {
            Slot* slot = nullptr;
            for (const std::unique_ptr<Slot>& candidate : s.slots)
            {
                if (phaseOf(candidate->state.load(std::memory_order_acquire)) == Free)
                {
                    slot = candidate.get();
                    break;
                }
            }
            if (slot == nullptr)
            {
                c.extra = "Retry-After: 1\r\n";
                respond(c, 503, {}, {}, Made::Library, c.keepAlive, now);
                return;
            }
            c.body.push_back('\0');   // may throw: the slot is untouched yet
            slot->method = c.method;
            std::memcpy(slot->target.get(), c.target.data(), c.target.size());   // within the head: kMaxHeadBytes
            slot->targetLength = static_cast<std::int32_t>(c.target.size());
            slot->pathLength = static_cast<std::int32_t>(pathOf(c).size());
            slot->body.swap(c.body);
            slot->bodySize = static_cast<std::int32_t>(slot->body.size()) - 1;
            slot->argCount = 0;
            slot->replyStatus = 0;
            slot->replyBytes = 0;
            slot->typeLength = 0;
            slot->connection = &c;
            slot->deadline = now + std::chrono::milliseconds(s.limits->replyTimeoutMs);
            c.slot = slot;
            c.stage = Connection::Stage::Program;
            slot->state.store((++s.sequence << 8) | Published, std::memory_order_seq_cst);
            wakeAcceptors(s);
        }

        // The program's reply, answered into the slot, as the connection's response.
        void sendReply(Connection& c, const Slot& slot, Clock::time_point now)
        {
            const std::string_view type(slot.type, static_cast<std::size_t>(slot.typeLength));
            const std::string_view body(slot.buffer.get(), static_cast<std::size_t>(slot.replyBytes));
            c.slot = nullptr;
            respond(c, slot.replyStatus, type, body, Made::Library, c.keepAlive, now);
        }

        // ---- a connection's turn --------------------------------------------------------------------------------------

        void closeConnection(Connection& c)
        {
            if (c.dead)
                return;
            if (Slot* slot = c.slot)
            {
                c.slot = nullptr;
                if (!giveUp(*slot))
                {
                    // Answered meanwhile, with no one to send it to.
                    const std::uint64_t word = slot->state.load(std::memory_order_acquire);
                    if (phaseOf(word) == Answered)
                        freeSlot(*slot, word);
                }
            }
            sockets::closeSocket(c.socket);
            c.socket = sockets::kNone;
            c.dead = true;
        }

        // Parses what the connection has buffered while it is reading a request, and acts on what that finds.
        void parse(Server& s, Connection& c, Clock::time_point now)
        {
            while ((c.stage == Connection::Stage::Head || c.stage == Connection::Stage::Body) && !c.in.empty())
            {
                std::size_t offer = c.in.size();
                if (c.stage == Connection::Stage::Head)
                {
                    offer = std::min<std::size_t>(offer, static_cast<std::size_t>(kMaxHeadBytes - c.headBytes));
                    if (offer == 0)
                    {
                        refuseRequest(c, 431, now);
                        return;
                    }
                }
                const bool inHead = c.stage == Connection::Stage::Head;
                const llhttp_errno_t result = llhttp_execute(&c.parser, c.in.data(), offer);
                std::size_t used = offer;
                if (result == HPE_PAUSED)
                {
                    used = static_cast<std::size_t>(llhttp_get_error_pos(&c.parser) - c.in.data());
                    llhttp_resume(&c.parser);
                }
                else if (result != HPE_OK)
                {
                    if (c.file != nullptr && c.status == 200)
                        serveFile(c, false, now);
                    else
                        refuseRequest(c, c.status != 0 ? c.status : result == HPE_INVALID_METHOD ? 501 : 400, now);
                    return;
                }
                if (inHead && c.stage == Connection::Stage::Head)
                    c.headBytes += static_cast<std::int32_t>(used);
                c.in.erase(0, used);
                if (c.messageDone)
                {
                    c.messageDone = false;
                    if (c.file != nullptr)
                        serveFile(c, c.keepAlive, now);
                    else
                        publish(s, c, now);
                    return;
                }
            }
            if (c.stage == Connection::Stage::Head && c.headBytes >= kMaxHeadBytes)
                refuseRequest(c, 431, now);
        }

        // Reads what the connection has for it, up to what it buffers, and parses it.
        void readFrom(Server& s, Connection& c, Clock::time_point now)
        {
            char chunk[kReadChunk];
            while (!c.peerClosed && !c.dead && c.in.size() < kMaxBuffered)
            {
                const long got = sockets::receive(c.socket, chunk, std::min(sizeof chunk, kMaxBuffered - c.in.size()));
                if (got == -1)
                    return;
                if (got == -2)
                {
                    closeConnection(c);
                    return;
                }
                if (got == 0)
                {
                    c.peerClosed = true;
                    return;
                }
                if (c.stage == Connection::Stage::Idle)
                {
                    c.stage = Connection::Stage::Head;
                    c.deadline = now + std::chrono::milliseconds(s.limits->requestTimeoutMs);
                }
                c.in.append(chunk, static_cast<std::size_t>(got));
                if (c.stage == Connection::Stage::Head || c.stage == Connection::Stage::Body)
                {
                    c.started = true;
                    parse(s, c, now);
                }
            }
        }

        // Sends what the connection has to send; then the next request, or the close.
        void writeTo(Server& s, Connection& c, Clock::time_point now)
        {
            while (c.sent < c.out.size())
            {
                const long sent = sockets::transmit(c.socket, c.out.data() + c.sent, c.out.size() - c.sent);
                if (sent == -1)
                    return;
                if (sent == -2)
                {
                    closeConnection(c);
                    return;
                }
                c.sent += static_cast<std::size_t>(sent);
            }
            c.out.clear();
            c.sent = 0;
            if (c.closeAfter || c.peerClosed)
            {
                closeConnection(c);
                return;
            }
            // Kept alive: the next request, which may be buffered already.
            resetRequest(c);
            if (c.in.empty())
            {
                c.stage = Connection::Stage::Idle;
                c.deadline = now + std::chrono::milliseconds(s.limits->idleTimeoutMs);
            }
            else
            {
                c.stage = Connection::Stage::Head;
                c.deadline = now + std::chrono::milliseconds(s.limits->requestTimeoutMs);
                parse(s, c, now);
            }
        }

        bool wantsBytes(const Connection& c)
        {
            return !c.dead && !c.peerClosed && c.in.size() < kMaxBuffered;
        }

        void turnConnection(Server& s, Connection& c, Clock::time_point now)
        {
            if (wantsBytes(c))
                readFrom(s, c, now);
            if (c.dead)
                return;
            if (c.peerClosed)
            {
                // The client is gone (a client that only shut down its sending side is taken for gone too): a request
                // it was sending, or one the program has, is given up; a response going out is finished.
                if (c.stage != Connection::Stage::Writing)
                {
                    closeConnection(c);
                    return;
                }
            }
            if (c.stage == Connection::Stage::Writing)
                writeTo(s, c, now);
            if (c.dead)
                return;
            if (c.stage != Connection::Stage::Program && now >= c.deadline)
            {
                if (c.stage == Connection::Stage::Writing || c.stage == Connection::Stage::Idle || !c.started)
                    closeConnection(c);   // a response no one takes, a connection kept with nothing on it
                else
                {
                    refuseRequest(c, 408, now);   // a request not sent in time
                    writeTo(s, c, now);
                }
            }
        }

        // The slots' turn: replies to send, given-up slots to free, replies timed out.
        void turnSlots(Server& s, Clock::time_point now)
        {
            for (const std::unique_ptr<Slot>& owned : s.slots)
            {
                Slot& slot = *owned;
                const std::uint64_t word = slot.state.load(std::memory_order_acquire);
                switch (phaseOf(word))
                {
                case Answered:
                    if (Connection* c = slot.connection)
                    {
                        try
                        {
                            sendReply(*c, slot, now);
                        }
                        catch (const std::bad_alloc&)
                        {
                            c->slot = nullptr;
                            closeConnection(*c);
                        }
                    }
                    freeSlot(slot, word);
                    break;
                case Done:
                    freeSlot(slot, word);
                    break;
                case Published:
                case Claimed:
                    if (now >= slot.deadline)
                    {
                        Connection* c = slot.connection;
                        if (giveUp(slot) && c != nullptr)
                        {
                            c->slot = nullptr;
                            try
                            {
                                c->extra = "Retry-After: 1\r\n";
                                respond(*c, 503, {}, {}, Made::Library, c->keepAlive, now);
                            }
                            catch (const std::bad_alloc&)
                            {
                                closeConnection(*c);
                            }
                        }
                    }
                    break;
                case Free:
                case Abandoned:
                case Closed:
                    break;
                }
            }
        }

        void acceptConnections(Server& s, Clock::time_point now)
        {
            for (std::size_t i = 0; i < s.listeners.size(); ++i)
            {
                if (now < s.acceptAgain[i])
                    continue;
                for (;;)
                {
                    bool again = false;
                    const sockets::Socket socket = sockets::acceptOn(s.listeners[i], again);
                    if (socket == sockets::kNone)
                    {
                        if (!again)
                            s.acceptAgain[i] = now + kAcceptPause;
                        break;
                    }
                    // Rule 1: beyond the limit, closed at once.
                    if (s.connections.size() >= static_cast<std::size_t>(s.limits->maxConnections))
                    {
                        sockets::closeSocket(socket);
                        continue;
                    }
                    try
                    {
                        auto c = std::make_unique<Connection>();
                        c->server = &s;
                        c->socket = socket;
                        if (!sockets::localAddress(socket, c->local))
                        {
                            sockets::closeSocket(socket);
                            continue;
                        }
                        c->localPort = sockets::localPort(socket);
                        llhttp_init(&c->parser, HTTP_REQUEST, &settings());
                        c->parser.data = c.get();
                        c->stage = Connection::Stage::Head;
                        c->deadline = now + std::chrono::milliseconds(s.limits->requestTimeoutMs);
                        c->target.reserve(256);
                        s.connections.push_back(std::move(c));
                    }
                    catch (const std::bad_alloc&)
                    {
                        sockets::closeSocket(socket);
                    }
                }
            }
        }

        // ---- the program's side -------------------------------------------------------------------------------------

        // The request the caller holds (claimed, or abandoned and not yet answered), or null.
        Slot* held(void* handle)
        {
            Slot* slot = as<Slot>(handle);
            if (slot == nullptr)
                return nullptr;
            const Phase phase = phaseOf(slot->state.load(std::memory_order_acquire));
            return phase == Claimed || phase == Abandoned ? slot : nullptr;
        }

        const Slot* held(const void* handle)
        {
            return held(const_cast<void*>(handle));
        }

        std::string_view queryOf(const Slot& slot)
        {
            if (slot.pathLength >= slot.targetLength)
                return {};
            return std::string_view(slot.target.get() + slot.pathLength + 1,
                                    static_cast<std::size_t>(slot.targetLength - slot.pathLength - 1));
        }

        // The value of the query's first field named `name`; false when there is none.
        bool queryValue(const Slot& slot, const char* name, std::int32_t length, std::string_view& value)
        {
            if (name == nullptr || length <= 0)
                return false;
            const std::string_view wanted(name, static_cast<std::size_t>(length));
            std::string_view query = queryOf(slot);
            while (!query.empty())
            {
                const std::size_t amp = query.find('&');
                const std::string_view field = query.substr(0, amp);
                query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
                const std::size_t equals = field.find('=');
                if (field.substr(0, equals) != wanted)
                    continue;
                value = equals == std::string_view::npos ? std::string_view() : field.substr(equals + 1);
                return true;
            }
            return false;
        }

        bool validType(const char* type, std::int32_t length)
        {
            if (length < 0 || length > 255 || (type == nullptr && length > 0))
                return false;
            for (std::int32_t i = 0; i < length; ++i)
            {
                const auto byte = static_cast<unsigned char>(type[i]);
                if (byte < 0x20 || byte >= 0x7f)
                    return false;
            }
            return true;
        }

        bool validStatus(std::int32_t status, bool body)
        {
            if (status < 200 || status > 599)
                return false;
            return !(body && (status == 204 || status == 304));
        }

        void setType(Slot& slot, const char* type, std::int32_t length, std::string_view fallback)
        {
            if (length > 0)
            {
                std::memcpy(slot.type, type, static_cast<std::size_t>(length));
                slot.typeLength = length;
            }
            else
            {
                std::memcpy(slot.type, fallback.data(), fallback.size());
                slot.typeLength = static_cast<std::int32_t>(fallback.size());
            }
        }

        // Publishes the reply written into the slot: 0, or FMINUS_WEB_CANCELLED when the request was given up on
        // meanwhile (the slot is then done), or FMINUS_WEB_INVALID when it was not the caller's to answer.
        std::int32_t finishReply(Slot& slot)
        {
            for (;;)
            {
                std::uint64_t word = slot.state.load(std::memory_order_acquire);
                const Phase phase = phaseOf(word);
                if (phase == Claimed)
                {
                    if (slot.state.compare_exchange_weak(word, withPhase(word, Answered), std::memory_order_acq_rel))
                        return 0;
                }
                else if (phase == Abandoned)
                {
                    if (slot.state.compare_exchange_weak(word, withPhase(word, Done), std::memory_order_acq_rel))
                        return FMINUS_WEB_CANCELLED;
                }
                else
                    return FMINUS_WEB_INVALID;
            }
        }

        // Writes text into a bounded buffer: `over` once it would go past its end.
        struct Writer
        {
            char* at;
            char* end;
            bool over = false;

            void put(char c)
            {
                if (at == end)
                    over = true;
                else
                    *at++ = c;
            }

            void put(const char* first, const char* last)
            {
                const auto n = static_cast<std::size_t>(last - first);
                if (static_cast<std::size_t>(end - at) < n)
                {
                    over = true;
                    return;
                }
                std::memcpy(at, first, n);
                at += n;
            }
        };

        // %d: C's conversion of the value to an int, saturated (NaN is 0).
        void putInt(Writer& out, double value)
        {
            std::int32_t n = 0;
            if (value >= 2147483647.0)
                n = std::numeric_limits<std::int32_t>::max();
            else if (value <= -2147483648.0)
                n = std::numeric_limits<std::int32_t>::min();
            else if (value == value)
                n = static_cast<std::int32_t>(value);
            char digits[16];
            out.put(digits, std::to_chars(digits, digits + sizeof digits, n).ptr);
        }

        // %g and %.Ng: as printf writes them (std::to_chars's general format with a precision is printf's %g).
        void putFloat(Writer& out, double value, int precision)
        {
            char digits[160];   // a precision of at most 99: 99 digits, a sign, a point and an exponent
            const std::to_chars_result result =
                std::to_chars(digits, digits + sizeof digits, value, std::chars_format::general, precision);
            if (result.ec == std::errc())
                out.put(digits, result.ptr);
        }

        void format(Writer& out, const char* text, std::int32_t length, const double* values, std::int32_t count)
        {
            if (text == nullptr || length < 0)
                return;
            if (values == nullptr || count < 0)
                count = 0;
            std::int32_t next = 0;
            for (std::int32_t i = 0; i < length && !out.over; ++i)
            {
                const char c = text[i];
                if (c != '%' || i + 1 == length)
                {
                    out.put(c);
                    continue;
                }
                const char d = text[i + 1];
                if (d == '%')
                {
                    out.put('%');
                    ++i;
                }
                else if (d == 'd' || d == 'g')
                {
                    ++i;
                    if (next < count)
                    {
                        if (d == 'd')
                            putInt(out, values[next]);
                        else
                            putFloat(out, values[next], 6);
                        ++next;
                    }
                }
                else if (d == '.')
                {
                    // %.Ng: one or two digits, then g.
                    std::int32_t j = i + 2;
                    int precision = 0;
                    while (j < length && j < i + 4 && isDigit(text[j]))
                        precision = precision * 10 + (text[j++] - '0');
                    if (j > i + 2 && j < length && text[j] == 'g')
                    {
                        i = j;
                        if (next < count)
                            putFloat(out, values[next++], precision == 0 ? 1 : precision);
                    }
                    else
                        out.put(c);
                }
                else
                    out.put(c);
            }
        }
    } // namespace

    // ---- matching ---------------------------------------------------------------------------------------------------

    bool matchPath(std::string_view pattern, std::string_view path, std::int32_t* args, std::int32_t& count) noexcept
    {
        count = 0;
        std::size_t j = 0;
        for (std::size_t i = 0; i < pattern.size();)
        {
            if (pattern[i] == '%' && i + 1 < pattern.size() && pattern[i + 1] == 'd')
            {
                const std::size_t start = j;
                std::int64_t value = 0;
                while (j < path.size() && isDigit(path[j]) && j - start <= 10)
                    value = value * 10 + (path[j++] - '0');
                const std::size_t digits = j - start;
                if (digits == 0 || digits > 10 || value > std::numeric_limits<std::int32_t>::max() ||
                    count == kMaxArgs)
                    return false;
                args[count++] = static_cast<std::int32_t>(value);
                i += 2;
                continue;
            }
            char c = pattern[i];
            if (c == '%' && i + 1 < pattern.size() && pattern[i + 1] == '%')
                ++i;   // a percent sign, which no path has
            ++i;
            if (j == path.size() || path[j] != c)
                return false;
            ++j;
        }
        return j == path.size();
    }

    // ---- the server's life --------------------------------------------------------------------------------------------

    Server* refusedServer() noexcept
    {
        static Server server;   // its port is FMINUS_WEB_REFUSED, it has no slots, and it is never closed
        return &server;
    }

    Server* makeServer(Context::State* context, const CompiledPolicy& policy, const char* const* routes,
                       const std::int32_t* lengths, std::int32_t count, bool stopped)
    {
        auto server = std::make_unique<Server>();
        server->context = context;
        server->setup = &policy.server;
        server->limits = &policy.policy.server;
        const ServerPolicy& limits = policy.policy.server;
        if (routes != nullptr && lengths != nullptr)
        {
            for (std::int32_t i = 0; i < count; ++i)
            {
                if (routes[i] == nullptr || lengths[i] <= 0)
                    continue;
                Method method = Method::None;
                std::string_view pattern;
                if (readRoute(std::string_view(routes[i], static_cast<std::size_t>(lengths[i])), method, pattern))
                    server->routes.push_back({method, std::string(pattern)});
            }
        }
        server->slots.reserve(static_cast<std::size_t>(limits.maxPending));
        for (std::int32_t i = 0; i < limits.maxPending; ++i)
        {
            auto slot = std::make_unique<Slot>();
            slot->target = std::make_unique<char[]>(kMaxHeadBytes);
            slot->capacity = limits.maxReplyBytes;
            slot->buffer = std::make_unique<char[]>(static_cast<std::size_t>(std::max(limits.maxReplyBytes, 1)));
            server->slots.push_back(std::move(slot));
        }
        if (stopped)
        {
            server->closed = true;
            server->port.store(FMINUS_WEB_CANCELLED, std::memory_order_release);
            return server.release();
        }

        // Binds every address of the listen, on one port. With port 0 and two addresses, the second may find the
        // port the first was given taken: then both are tried again, on another.
        const ServerSetup& setup = policy.server;
        std::vector<sockets::Socket> made;
        made.reserve(setup.addresses.size());
        int port = 0;
        bool listening = false;
        const int attempts = setup.port == 0 && setup.addresses.size() > 1 ? 16 : 1;
        for (int attempt = 0; attempt < attempts && !listening; ++attempt)
        {
            for (sockets::Socket socket : made)
                sockets::closeSocket(socket);
            made.clear();
            int chosen = setup.port;
            bool retry = false;
            listening = true;
            for (std::size_t i = 0; i < setup.addresses.size(); ++i)
            {
                const Address& address = setup.addresses[i];
                sockets::Bound outcome = sockets::Bound::Failed;
                const sockets::Socket socket = sockets::listenOn(address, chosen, outcome);
                if (socket == sockets::kNone)
                {
                    // localhost's ::1 and *'s ::, on a machine without IPv6: the other address alone.
                    if (i > 0 && address.family == 6 && (setup.localhost || setup.any) &&
                        outcome == sockets::Bound::Unavailable)
                        continue;
                    retry = i > 0 && setup.port == 0 && outcome == sockets::Bound::InUse;
                    listening = false;
                    break;
                }
                made.push_back(socket);
                if (chosen == 0)
                {
                    chosen = sockets::localPort(socket);
                    if (chosen == 0)
                    {
                        listening = false;
                        break;
                    }
                }
            }
            port = chosen;
            if (!listening && !retry)
                break;
        }
        if (!listening)
        {
            for (sockets::Socket socket : made)
                sockets::closeSocket(socket);
            server->port.store(FMINUS_WEB_TRANSPORT, std::memory_order_release);
            return server.release();
        }
        server->listeners = std::move(made);
        server->acceptAgain.assign(server->listeners.size(), Clock::time_point());
        server->port.store(port, std::memory_order_release);
        return server.release();
    }

    bool serverListens(const Server& server) noexcept
    {
        return !server.listeners.empty();
    }

    Context::State* serverContext(const Server& server) noexcept
    {
        return server.context;
    }

    void failServer(Server& server) noexcept
    {
        for (sockets::Socket socket : server.listeners)
            sockets::closeSocket(socket);
        server.listeners.clear();
        server.port.store(FMINUS_WEB_FAILED, std::memory_order_release);
    }

    void closeServer(Server& server) noexcept
    {
        if (&server == refusedServer())
            return;
        {
            std::lock_guard lock(server.mutex);
            server.closed = true;
            server.port.store(FMINUS_WEB_CANCELLED, std::memory_order_release);
        }
        server.changed.notify_all();
    }

    void freeServer(Server* server) noexcept
    {
        if (server == nullptr || server == refusedServer())
            return;
        {
            std::unique_lock lock(server->mutex);
            server->closed = true;
            server->dying = true;
            server->changed.notify_all();
            server->changed.wait(lock, [&] { return server->waiters.load() == 0; });
        }
        for (sockets::Socket socket : server->listeners)
            sockets::closeSocket(socket);
        delete server;
    }

    // ---- the I/O thread's servers ---------------------------------------------------------------------------------------

    void Servers::attach(Server* server) noexcept
    {
        if (std::find(servers_.begin(), servers_.end(), server) != servers_.end())
            return;
        try
        {
            servers_.push_back(server);
        }
        catch (const std::bad_alloc&)
        {
            failServer(*server);
        }
    }

    void Servers::detach(Server* server) noexcept
    {
        const auto found = std::find(servers_.begin(), servers_.end(), server);
        if (found == servers_.end())
            return;
        servers_.erase(found);
        for (const std::unique_ptr<Connection>& c : server->connections)
            closeConnection(*c);
        server->connections.clear();
        for (sockets::Socket socket : server->listeners)
            sockets::closeSocket(socket);
        server->listeners.clear();
        server->acceptAgain.clear();
        // Every slot closed, but those a taker holds: abandoned, so that its reply says FMINUS_WEB_CANCELLED.
        for (const std::unique_ptr<Slot>& slot : server->slots)
        {
            for (;;)
            {
                std::uint64_t word = slot->state.load(std::memory_order_acquire);
                const Phase phase = phaseOf(word);
                if (phase == Abandoned || phase == Closed)
                    break;
                const Phase next = phase == Claimed ? Abandoned : Closed;
                if (slot->state.compare_exchange_weak(word, withPhase(word, next), std::memory_order_acq_rel))
                    break;
            }
            slot->connection = nullptr;
        }
    }

    void Servers::turn() noexcept
    {
        const Clock::time_point now = Clock::now();
        for (Server* server : servers_)
        {
            Server& s = *server;
            turnSlots(s, now);
            acceptConnections(s, now);
            for (const std::unique_ptr<Connection>& owned : s.connections)
            {
                try
                {
                    turnConnection(s, *owned, now);
                }
                catch (const std::bad_alloc&)
                {
                    closeConnection(*owned);
                }
            }
            std::erase_if(s.connections, [](const std::unique_ptr<Connection>& c) { return c->dead; });
        }
    }

    void Servers::descriptors(std::vector<curl_waitfd>& out) noexcept
    {
        out.clear();
        const Clock::time_point now = Clock::now();
        try
        {
            for (Server* server : servers_)
            {
                for (std::size_t i = 0; i < server->listeners.size(); ++i)
                {
                    if (now >= server->acceptAgain[i])
                        out.push_back({static_cast<curl_socket_t>(server->listeners[i]), CURL_WAIT_POLLIN, 0});
                }
                for (const std::unique_ptr<Connection>& c : server->connections)
                {
                    short events = 0;
                    if (wantsBytes(*c))
                        events |= CURL_WAIT_POLLIN;
                    if (c->sent < c->out.size())
                        events |= CURL_WAIT_POLLOUT;
                    if (events != 0)
                        out.push_back({static_cast<curl_socket_t>(c->socket), events, 0});
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            // Some descriptors left out of this wait: the next turn (within a second) tries every socket anyway.
        }
    }

    int Servers::timeoutMs(int cap) const noexcept
    {
        const Clock::time_point now = Clock::now();
        Clock::time_point next = now + std::chrono::milliseconds(cap);
        for (const Server* server : servers_)
        {
            for (const std::unique_ptr<Slot>& slot : server->slots)
            {
                const Phase phase = phaseOf(slot->state.load(std::memory_order_acquire));
                if (phase == Published || phase == Claimed || phase == Answered || phase == Done)
                    return std::min(cap, 1);
            }
            for (const Clock::time_point again : server->acceptAgain)
            {
                if (again > now)
                    next = std::min(next, again);
            }
            for (const std::unique_ptr<Connection>& c : server->connections)
            {
                if (c->stage != Connection::Stage::Program)
                    next = std::min(next, c->deadline);
            }
        }
        if (next <= now)
            return 0;
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count() + 1;
        return static_cast<int>(std::min<std::int64_t>(wait, cap));
    }

    // ---- handles ---------------------------------------------------------------------------------------------------------

    bool isServer(const void* handle) noexcept
    {
        return as<Server>(handle) != nullptr;
    }

    bool isReceived(const void* handle) noexcept
    {
        return as<Slot>(handle) != nullptr;
    }

    Server* asServer(void* handle) noexcept
    {
        return as<Server>(handle);
    }

    void* serverHandle(Server* server) noexcept
    {
        return toHandle(server);
    }

    std::int32_t receivedSize(const void* handle) noexcept
    {
        const Slot* slot = held(handle);
        return slot == nullptr ? 0 : slot->bodySize;
    }

    const void* receivedBody(const void* handle) noexcept
    {
        const Slot* slot = held(handle);
        return slot == nullptr ? nullptr : slot->body.data();
    }
} // namespace fminus::web

using fminus::web::Server;
using fminus::web::Slot;

extern "C"
{
    std::int32_t fminus_web_http_port(const void* handle) noexcept
    {
        const Server* server = fminus::web::as<Server>(handle);
        return server == nullptr ? FMINUS_WEB_INVALID : server->port.load(std::memory_order_acquire);
    }

    void* fminus_web_http_next(void* handle) noexcept
    {
        Server* server = fminus::web::as<Server>(handle);
        if (server == nullptr)
            return nullptr;
        const std::size_t slots = server->slots.size();
        for (std::size_t attempt = 0; attempt < 2 * slots; ++attempt)
        {
            Slot* oldest = nullptr;
            std::uint64_t oldestWord = 0;
            for (const std::unique_ptr<Slot>& slot : server->slots)
            {
                const std::uint64_t word = slot->state.load(std::memory_order_acquire);
                if (fminus::web::phaseOf(word) == fminus::web::Published &&
                    (oldest == nullptr || (word >> 8) < (oldestWord >> 8)))
                {
                    oldest = slot.get();
                    oldestWord = word;
                }
            }
            if (oldest == nullptr)
                return nullptr;
            if (oldest->state.compare_exchange_strong(oldestWord,
                                                      fminus::web::withPhase(oldestWord, fminus::web::Claimed),
                                                      std::memory_order_acq_rel))
                return fminus::web::toHandle(oldest);
        }
        return nullptr;
    }

    // The thread counts itself in first, and out last, under the server's mutex, whatever path it takes: freeServer
    // waits for the count to be 0, so a thread in here is never left touching a server being freed.
    void* fminus_web_http_accept(void* handle, std::int32_t timeoutMs) noexcept
    {
        Server* server = fminus::web::as<Server>(handle);
        if (server == nullptr)
            return nullptr;
        server->waiters.fetch_add(1, std::memory_order_seq_cst);
        void* request = nullptr;
        std::unique_lock lock(server->mutex);
        if (!server->closed && !server->dying && server->port.load(std::memory_order_acquire) > 0)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                request = fminus_web_http_next(handle);
                if (request != nullptr || server->closed || server->dying || timeoutMs == 0)
                    break;
                if (timeoutMs < 0)
                    server->changed.wait(lock);
                else if (server->changed.wait_until(lock, deadline) == std::cv_status::timeout)
                {
                    request = fminus_web_http_next(handle);
                    break;
                }
            }
        }
        if (server->waiters.fetch_sub(1, std::memory_order_seq_cst) == 1 && server->dying)
            server->changed.notify_all();
        return request;
    }

    std::int32_t fminus_web_http_is(void* handle, const char* route, std::int32_t length) noexcept
    {
        Slot* slot = fminus::web::held(handle);
        if (slot == nullptr || route == nullptr || length <= 0)
            return 0;
        fminus::web::Method method = fminus::web::Method::None;
        std::string_view pattern;
        if (!fminus::web::readRoute(std::string_view(route, static_cast<std::size_t>(length)), method, pattern) ||
            !fminus::web::admits(method, slot->method))
            return 0;
        std::int32_t args[fminus::web::kMaxArgs];
        std::int32_t count = 0;
        if (!fminus::web::matchPath(pattern,
                                    std::string_view(slot->target.get(), static_cast<std::size_t>(slot->pathLength)),
                                    args, count))
            return 0;
        std::copy_n(args, count, slot->args);
        slot->argCount = count;
        return 1;
    }

    std::int32_t fminus_web_http_arg(const void* handle, std::int32_t index) noexcept
    {
        const Slot* slot = fminus::web::held(handle);
        if (slot == nullptr || index < 0 || index >= slot->argCount)
            return 0;
        return slot->args[index];
    }

    float fminus_web_http_query_float(const void* handle, const char* name, std::int32_t length,
                                      float fallback) noexcept
    {
        const Slot* slot = fminus::web::held(handle);
        std::string_view value;
        if (slot == nullptr || !fminus::web::queryValue(*slot, name, length, value) || value.empty())
            return fallback;
        float read = 0.0f;
        const fast_float::from_chars_result result =
            fast_float::from_chars(value.data(), value.data() + value.size(), read, fast_float::chars_format::general);
        if ((result.ec != std::errc() && result.ec != std::errc::result_out_of_range) ||
            result.ptr != value.data() + value.size() || !std::isfinite(read))
            return fallback;
        return read;
    }

    std::int32_t fminus_web_http_query_int(const void* handle, const char* name, std::int32_t length,
                                           std::int32_t fallback) noexcept
    {
        const Slot* slot = fminus::web::held(handle);
        std::string_view value;
        if (slot == nullptr || !fminus::web::queryValue(*slot, name, length, value) || value.empty())
            return fallback;
        std::int32_t read = 0;
        const std::from_chars_result result = std::from_chars(value.data(), value.data() + value.size(), read, 10);
        if (result.ec != std::errc() || result.ptr != value.data() + value.size())
            return fallback;
        return read;
    }

    std::int32_t fminus_web_http_reply(void* handle, std::int32_t status) noexcept
    {
        Slot* slot = fminus::web::held(handle);
        if (slot == nullptr || !fminus::web::validStatus(status, false))
            return FMINUS_WEB_INVALID;
        slot->replyStatus = status;
        slot->replyBytes = 0;
        slot->typeLength = 0;
        return fminus::web::finishReply(*slot);
    }

    std::int32_t fminus_web_http_reply_bytes(void* handle, std::int32_t status, const void* data, std::int32_t bytes,
                                             const char* type, std::int32_t typeLength) noexcept
    {
        Slot* slot = fminus::web::held(handle);
        if (slot == nullptr || bytes < 0 || (data == nullptr && bytes > 0) ||
            !fminus::web::validStatus(status, bytes > 0) || !fminus::web::validType(type, typeLength))
            return FMINUS_WEB_INVALID;
        if (bytes > slot->capacity)
            return FMINUS_WEB_LIMIT;
        if (bytes > 0)
            std::memcpy(slot->buffer.get(), data, static_cast<std::size_t>(bytes));
        slot->replyStatus = status;
        slot->replyBytes = bytes;
        fminus::web::setType(*slot, type, typeLength, "application/octet-stream");
        return fminus::web::finishReply(*slot);
    }

    std::int32_t fminus_web_http_reply_text(void* handle, std::int32_t status, const char* type,
                                            std::int32_t typeLength, const char* format, std::int32_t formatLength,
                                            const double* values, std::int32_t count) noexcept
    {
        Slot* slot = fminus::web::held(handle);
        if (slot == nullptr || !fminus::web::validType(type, typeLength) || formatLength < 0 ||
            (format == nullptr && formatLength > 0) || status < 200 || status > 599)
            return FMINUS_WEB_INVALID;
        fminus::web::Writer out{slot->buffer.get(), slot->buffer.get() + slot->capacity};
        fminus::web::format(out, format, formatLength, values, count);
        if (out.over)
            return FMINUS_WEB_LIMIT;
        const auto bytes = static_cast<std::int32_t>(out.at - slot->buffer.get());
        if (!fminus::web::validStatus(status, bytes > 0))
            return FMINUS_WEB_INVALID;
        slot->replyStatus = status;
        slot->replyBytes = bytes;
        fminus::web::setType(*slot, type, typeLength, "text/plain; charset=utf-8");
        return fminus::web::finishReply(*slot);
    }
}

#endif
