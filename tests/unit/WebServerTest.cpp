// tests/unit/WebServerTest.cpp: the web library's server (src/web/Server.h, Web.h), at its own level, from C++, before
// any builtin calls it: the C functions a program's builtins will call, a context's policy and its server part. The
// policy's checks, every reason checkPolicy refuses a server policy for, and the functions that classify addresses,
// ports, paths, Host and Origin headers (which is how a listen that is not loopback is tested: none is ever bound);
// handles that know their kind; the route matcher and a reply's format against C's printf. Then a server on loopback,
// driven by a "program" thread of the test's that takes and answers its requests, and reached two ways: by the
// library's own client (a second context, http_get and http_post), and by raw sockets, for everything a well-behaved
// client never sends. One test at least for each of the nine rules and each limit, each response header, HEAD,
// keep-alive, two requests in one write, a request a byte at a time, connections dropped mid-request and mid-reply,
// more connections than the limit, every slot taken, a request no one answers, one answered after its client left, a
// reply over the limit, stop and destruction with connections open and a thread in accept, a second listen, a failed
// bind, localhost on both loopback addresses, static files and the token. And the promise behind the audio thread's
// calls: taking, reading and answering a request allocates nothing (HeapCalls) and makes no system call (a child under
// seccomp's strict mode, on Linux). Nothing here binds anything but 127.0.0.1 and ::1, or connects anywhere else. In a
// build without FMINUS_WEB_SERVER nothing here is compiled, nor on Windows, whose sockets these raw clients are not
// written for.
#include "fminus/Config.h"

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)

#include "HeapCalls.h"
#include "Shapes.h"
#include "runtime/Runtime.h"
#include "web/Policy.h"
#include "web/Server.h"
#include "web/Web.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

using fminus::web::Context;
using fminus::web::Policy;
using namespace std::chrono_literals;

namespace
{
    std::unique_ptr<Context> makeContext(const Policy& policy)
    {
        std::string error;
        std::unique_ptr<Context> context = Context::create(policy, error);
        INFO(error);
        REQUIRE(context);
        return context;
    }

    Policy serving(std::string listen = "127.0.0.1:0")
    {
        Policy policy;
        policy.server.listen = std::move(listen);
        return policy;
    }

    void* listen(Context& context, const std::vector<std::string>& routes)
    {
        std::vector<const char*> texts;
        std::vector<std::int32_t> lengths;
        for (const std::string& route : routes)
        {
            texts.push_back(route.data());
            lengths.push_back(static_cast<std::int32_t>(route.size()));
        }
        return fminus_web_http_listen(context.handle(), texts.data(), lengths.data(),
                                      static_cast<std::int32_t>(routes.size()));
    }

    std::int32_t is(void* request, std::string_view route)
    {
        return fminus_web_http_is(request, route.data(), static_cast<std::int32_t>(route.size()));
    }

    std::int32_t replyText(void* request, std::int32_t status, std::string_view format,
                           const std::vector<double>& values = {}, std::string_view type = {})
    {
        return fminus_web_http_reply_text(request, status, type.data(), static_cast<std::int32_t>(type.size()),
                                          format.data(), static_cast<std::int32_t>(format.size()), values.data(),
                                          static_cast<std::int32_t>(values.size()));
    }

    std::int32_t replyBytes(void* request, std::int32_t status, std::string_view bytes, std::string_view type = {})
    {
        return fminus_web_http_reply_bytes(request, status, bytes.data(), static_cast<std::int32_t>(bytes.size()),
                                           type.data(), static_cast<std::int32_t>(type.size()));
    }

    float queryFloat(void* request, std::string_view name, float fallback)
    {
        return fminus_web_http_query_float(request, name.data(), static_cast<std::int32_t>(name.size()), fallback);
    }

    std::int32_t queryInt(void* request, std::string_view name, std::int32_t fallback)
    {
        return fminus_web_http_query_int(request, name.data(), static_cast<std::int32_t>(name.size()), fallback);
    }

    std::string bodyOf(const void* request)
    {
        const void* data = fminus_web_http_body(request);
        return data == nullptr ? std::string()
                               : std::string(static_cast<const char*>(data),
                                             static_cast<std::size_t>(fminus_web_http_size(request)));
    }

    // A thread of the test's that plays the program: takes each request (http_accept) and answers it with `handler`,
    // until it is destroyed. Catch2's assertions stay on the test's thread: the handler records, the test checks.
    class ProgramThread
    {
    public:
        ProgramThread(void* server, std::function<void(void*)> handler)
            : server_(server)
            , handler_(std::move(handler))
            , thread_([this] { run(); })
        {
        }

        ~ProgramThread()
        {
            stop_.store(true);
            thread_.join();
        }

        ProgramThread(const ProgramThread&) = delete;
        ProgramThread& operator=(const ProgramThread&) = delete;

    private:
        void run()
        {
            while (!stop_.load())
            {
                if (void* request = fminus_web_http_accept(server_, 20))
                    handler_(request);
            }
        }

        void* server_;
        std::function<void(void*)> handler_;
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };

    // The test's routes, and the program's answers to them.
    const std::vector<std::string> kRoutes = {"GET /hello",      "GET /osc/%d/detune", "PUT /gain",
                                              "POST /table/%d",  "GET /state",         "DELETE /osc/%d",
                                              "GET /big",        "GET /",              "PATCH /a/%d/b/%d"};

    void answer(void* r)
    {
        if (is(r, "GET /hello"))
            replyText(r, 200, "hello");
        else if (is(r, "GET /osc/%d/detune"))
            replyText(r, 200, "osc %d", {double(fminus_web_http_arg(r, 0))}, "text/plain");
        else if (is(r, "PUT /gain"))
            replyText(r, 200, "{\"gain\":%g}", {double(queryFloat(r, "value", -1.0f))}, "application/json");
        else if (is(r, "POST /table/%d"))
            replyText(r, 201, "table %d: %d bytes [%s]",
                      {double(fminus_web_http_arg(r, 0)), double(fminus_web_http_size(r))});
        else if (is(r, "PATCH /a/%d/b/%d"))
            replyText(r, 200, "%d,%d", {double(fminus_web_http_arg(r, 0)), double(fminus_web_http_arg(r, 1))});
        else if (is(r, "DELETE /osc/%d"))
            fminus_web_http_reply(r, 204);
        else
            fminus_web_http_reply(r, 404);
    }

    // ---- a raw client ---------------------------------------------------------------------------------------------

    int connectTo(int port, int family = 4)
    {
        const int fd = ::socket(family == 6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        timeval timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        int connected = -1;
        if (family == 6)
        {
            sockaddr_in6 in6{};
            in6.sin6_family = AF_INET6;
            in6.sin6_port = htons(static_cast<std::uint16_t>(port));
            in6.sin6_addr = in6addr_loopback;
            connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&in6), sizeof in6);
        }
        else
        {
            sockaddr_in in{};
            in.sin_family = AF_INET;
            in.sin_port = htons(static_cast<std::uint16_t>(port));
            in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            connected = ::connect(fd, reinterpret_cast<const sockaddr*>(&in), sizeof in);
        }
        REQUIRE(connected == 0);
        return fd;
    }

#if defined(MSG_NOSIGNAL)
    constexpr int kNoSignal = MSG_NOSIGNAL;
#else
    constexpr int kNoSignal = 0;   // SO_NOSIGPIPE, set in connectTo
#endif

    bool sendAll(int fd, std::string_view bytes)
    {
        while (!bytes.empty())
        {
            const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), kNoSignal);
            if (sent <= 0)
                return false;
            bytes.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    }

    // Whether the server closed the connection (EOF, or a reset) within `wait`.
    bool closedWithin(int fd, std::chrono::milliseconds wait)
    {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(wait.count())) <= 0)
            return false;
        char byte = 0;
        return ::recv(fd, &byte, 1, MSG_DONTWAIT) <= 0;
    }

    struct Response
    {
        int status = 0;
        std::string head;
        std::map<std::string, std::string> headers;   // names in lower case
        std::string body;

        bool has(const std::string& name) const
        {
            return headers.contains(name);
        }

        std::string header(const std::string& name) const
        {
            const auto found = headers.find(name);
            return found == headers.end() ? std::string() : found->second;
        }
    };

    // Reads responses from a connection, one at a time (a response to a HEAD has no body, whatever it says).
    class Reader
    {
    public:
        explicit Reader(int fd)
            : fd_(fd)
        {
        }

        std::optional<Response> next(bool head = false)
        {
            std::size_t end = std::string::npos;
            while ((end = buffer_.find("\r\n\r\n")) == std::string::npos)
            {
                if (!fill())
                    return std::nullopt;
            }
            Response response;
            response.head = buffer_.substr(0, end + 4);
            buffer_.erase(0, end + 4);
            std::size_t line = response.head.find("\r\n");
            const std::string status = response.head.substr(0, line);
            if (status.size() < 12 || !status.starts_with("HTTP/1.1 "))
                return std::nullopt;
            response.status = std::stoi(status.substr(9, 3));
            for (std::size_t at = line + 2; at < response.head.size() - 2;)
            {
                const std::size_t next = response.head.find("\r\n", at);
                const std::string field = response.head.substr(at, next - at);
                const std::size_t colon = field.find(':');
                std::string name = field.substr(0, colon);
                for (char& c : name)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string value = field.substr(colon + 1);
                while (!value.empty() && value.front() == ' ')
                    value.erase(0, 1);
                response.headers[name] = value;
                at = next + 2;
            }
            std::size_t length = 0;
            if (response.has("content-length") && !head)
                length = std::stoul(response.header("content-length"));
            while (buffer_.size() < length)
            {
                if (!fill())
                    return std::nullopt;
            }
            response.body = buffer_.substr(0, length);
            buffer_.erase(0, length);
            return response;
        }

    private:
        bool fill()
        {
            char chunk[65536];
            const ssize_t got = ::recv(fd_, chunk, sizeof chunk, 0);
            if (got <= 0)
                return false;
            buffer_.append(chunk, static_cast<std::size_t>(got));
            return true;
        }

        int fd_;
        std::string buffer_;
    };

    // One request on a connection of its own, and its response (status 0 when none came).
    Response roundTrip(int port, const std::string& request, bool head = false)
    {
        const int fd = connectTo(port);
        Response response;
        sendAll(fd, request);   // a server that refuses a request early may close before all of it is sent
        Reader reader(fd);
        if (std::optional<Response> got = reader.next(head))
            response = *got;
        ::close(fd);
        return response;
    }

    std::string hostOf(int port)
    {
        return "127.0.0.1:" + std::to_string(port);
    }

    std::string get(int port, const std::string& target, const std::string& more = {})
    {
        return "GET " + target + " HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n" + more + "\r\n";
    }

    // A loopback server for `policy`, with the test's routes, and a program thread answering with `handler`.
    struct Served
    {
        std::unique_ptr<Context> context;
        void* server = nullptr;
        int port = 0;
        std::unique_ptr<ProgramThread> program;

        explicit Served(Policy policy, std::function<void(void*)> handler = answer,
                        const std::vector<std::string>& routes = kRoutes)
        {
            context = makeContext(policy);
            server = listen(*context, routes);
            REQUIRE(server != nullptr);
            port = fminus_web_http_port(server);
            REQUIRE(port > 0);
            if (handler)
                program = std::make_unique<ProgramThread>(server, std::move(handler));
        }

        ~Served()
        {
            program.reset();
        }
    };

    // The client side: a context allowed to reach the server's port on loopback.
    std::unique_ptr<Context> clientFor(int port)
    {
        Policy policy;
        policy.allow = {"http://127.0.0.1:" + std::to_string(port) + "/*"};
        return makeContext(policy);
    }

    void* clientGet(Context& context, int port, const std::string& path)
    {
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
        return fminus_web_http_get(context.handle(), url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0);
    }

    void* clientPost(Context& context, int port, const std::string& path, std::string_view body,
                     std::string_view type)
    {
        const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
        return fminus_web_http_post(context.handle(), url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0,
                                    body.data(), static_cast<std::int32_t>(body.size()), type.data(),
                                    static_cast<std::int32_t>(type.size()));
    }

    bool machineHasIpv6()
    {
        const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        sockaddr_in6 in6{};
        in6.sin6_family = AF_INET6;
        in6.sin6_addr = in6addr_loopback;
        const bool bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&in6), sizeof in6) == 0;
        ::close(fd);
        return bound;
    }
} // namespace

// ---- the policy ---------------------------------------------------------------------------------------------------

TEST_CASE("the server's policy: the default listens nowhere; the listen forms; every reason it is refused",
          "[web][server][policy]")
{
    CHECK(fminus::web::checkPolicy(Policy()).empty());
    const auto refused = [](const Policy& policy, std::string_view words)
    {
        const std::string error = fminus::web::checkPolicy(policy);
        INFO(error);
        CHECK(error.find(words) != std::string::npos);
    };
    const auto accepted = [](const Policy& policy)
    {
        const std::string error = fminus::web::checkPolicy(policy);
        INFO(error);
        CHECK(error.empty());
    };
    // The forms.
    for (const char* listen : {"127.0.0.1:0", "127.0.0.1:8080", "127.1.2.3:8080", "[::1]:8080", "localhost:0",
                               "LocalHost:8080", "localhost:65535"})
        accepted(serving(listen));
    Policy lan = serving("192.168.1.20:8080");
    refused(lan, "is not a loopback address: the policy needs a token");
    lan.server.token = "abcdefghijklmnop";
    accepted(lan);
    lan.server.token.clear();
    lan.server.open = true;
    accepted(lan);
    Policy every = serving("*:8080");
    refused(every, "is not a loopback address");
    every.server.token = "0123456789abcdef-_ABCDEF";
    accepted(every);
    Policy v6 = serving("[2001:db8::1]:8080");
    refused(v6, "is not a loopback address");
    v6.server.open = true;
    accepted(v6);
    // Mistakes in a listen.
    refused(serving("127.0.0.1"), "has no port");
    refused(serving("127.0.0.1:"), "has the port ''");
    refused(serving("127.0.0.1:65536"), "has the port '65536'");
    refused(serving("127.0.0.1:08080"), "without leading zeros");
    refused(serving("127.0.0.1:x"), "has the port 'x'");
    refused(serving("127.0.0.1:6000"), "which browsers refuse to reach (the Fetch standard's bad ports)");
    refused(serving("localhost:10080"), "bad ports");
    refused(serving("[::1]:22"), "bad ports");
    refused(serving("0.0.0.0:8080"), "is the unspecified address: write '*'");
    refused(serving("[::]:8080"), "is the unspecified address");
    refused(serving("224.0.0.1:8080"), "multicast");
    refused(serving("255.255.255.255:8080"), "broadcast");
    refused(serving("0.1.2.3:8080"), "0.0.0.0/8");
    refused(serving("[ff02::1]:8080"), "is a multicast address");
    refused(serving("[fe80::1]:8080"), "link-local");
    refused(serving("[fe80::1%eth0]:8080"), "no IPv6 address");
    refused(serving("[::ffff:127.0.0.1]:8080"), "an IPv4 address inside an IPv6 one");
    refused(serving("example.com:8080"), "a listen is an IPv4 address in dotted decimal");
    refused(serving("127.000.0.1:8080"), "a listen is an IPv4 address");
    refused(serving("127.0.0.1 :8080"), "holds a space");
    refused(serving("[::1:8080"), "a listen is");
    // The token.
    Policy token = serving();
    token.server.token = "short";
    refused(token, "server.token has 5 characters (16 to 256)");
    token.server.token = std::string(257, 'a');
    refused(token, "server.token has 257 characters");
    token.server.token = "abcdefghijklmnop!";
    refused(token, "other than A-Z, a-z, 0-9, '-' and '_'");
    token.server.token = "abcdefghijklmnop qrs";
    refused(token, "other than");
    // Hosts.
    Policy hosts = serving();
    hosts.server.hosts = {"studio.local", "Studio.Example"};
    accepted(hosts);
    for (const char* bad : {"studio.local:8080", "127.0.0.1", "localhost", "*.example.com", "-a.example", ""})
    {
        hosts.server.hosts = {bad};
        refused(hosts, "server.hosts has");
    }
    // Files.
    Policy files = serving();
    files.server.files = {{"/", "text/html", "<p>hi</p>"}, {"/app.js", "text/javascript", "1"}};
    accepted(files);
    for (const char* path : {"", "app.js", "/a/../b", "/a//b", "/a/", "/./a", "/a?b=1", "/a%2e", "/a b", "/a#b"})
    {
        files.server.files = {{path, "text/plain", "x"}};
        refused(files, "server.files has the path");
    }
    files.server.files = {{"/a", "text/plain", "x"}, {"/a", "text/plain", "y"}};
    refused(files, "twice");
    const std::string longType = std::string(256, 'a') + "/b";
    for (const char* type : {"", "texthtml", "text/html\r\nX: y", longType.c_str()})
    {
        files.server.files = {{"/a", type, "x"}};
        refused(files, "server.files has the type");
    }
    // The limits.
    const auto limit = [&](auto field, std::int32_t value, std::string_view words)
    {
        Policy policy = serving();
        policy.server.*field = value;
        refused(policy, words);
    };
    limit(&fminus::web::ServerPolicy::maxConnections, 0, "server.maxConnections is 0 (1 to 1024)");
    limit(&fminus::web::ServerPolicy::maxConnections, 1025, "server.maxConnections is 1025");
    limit(&fminus::web::ServerPolicy::maxPending, 0, "server.maxPending is 0 (1 to 1024)");
    limit(&fminus::web::ServerPolicy::maxRequestBytes, -1, "server.maxRequestBytes is -1 (0 to 1073741824)");
    limit(&fminus::web::ServerPolicy::maxReplyBytes, (1 << 24) + 1, "server.maxReplyBytes is 16777217");
    limit(&fminus::web::ServerPolicy::requestTimeoutMs, 0, "server.requestTimeoutMs is 0 (1 to 86400000)");
    limit(&fminus::web::ServerPolicy::replyTimeoutMs, -5, "server.replyTimeoutMs is -5");
    limit(&fminus::web::ServerPolicy::idleTimeoutMs, 86400001, "server.idleTimeoutMs is 86400001");
    Policy memory = serving();
    memory.server.maxPending = 1024;
    memory.server.maxReplyBytes = 1 << 20;
    refused(memory, "server.maxPending times server.maxReplyBytes");
    // The server part is checked with or without a listen, and a context is refused as checkPolicy says.
    Policy quiet;
    quiet.server.maxPending = 0;
    refused(quiet, "server.maxPending");
    std::string error;
    CHECK(Context::create(serving("0.0.0.0:80"), error) == nullptr);
    CHECK(error.find("unspecified") != std::string::npos);
}

TEST_CASE("the classifiers behind the rules: loopback, bad ports, paths, Host and Origin", "[web][server][policy]")
{
    const auto address = [](std::string_view text)
    {
        fminus::web::Address out;
        if (!fminus::web::parseAddress(text, 4, out))
            REQUIRE(fminus::web::parseAddress(text, 6, out));
        return out;
    };
    CHECK(fminus::web::loopbackAddress(address("127.0.0.1")));
    CHECK(fminus::web::loopbackAddress(address("127.255.0.9")));
    CHECK(fminus::web::loopbackAddress(address("::1")));
    CHECK_FALSE(fminus::web::loopbackAddress(address("::ffff:127.0.0.1")));
    CHECK_FALSE(fminus::web::loopbackAddress(address("192.168.1.20")));
    CHECK_FALSE(fminus::web::loopbackAddress(address("0.0.0.0")));
    CHECK_FALSE(fminus::web::loopbackAddress(address("::")));
    for (int port : {0, 1, 22, 25, 6000, 6665, 6669, 6697, 10080})
        CHECK(fminus::web::badPort(port));
    for (int port : {80, 443, 8080, 8000, 3000, 49152, 65535})
        CHECK_FALSE(fminus::web::badPort(port));
    for (const char* path : {"/", "/a", "/a/b", "/a-b_c.d~e", "/A/9", "/..a", "/a.."})
        CHECK(fminus::web::servablePath(path));
    for (const char* path : {"", "a", "//", "/a/", "/a//b", "/.", "/..", "/a/./b", "/a/../b", "/%2e", "/a b", "/a?b",
                             "/a#b", "/a;b", "/a:b", "/\xc3\xa9"})
        CHECK_FALSE(fminus::web::servablePath(path));

    // Host on a LAN address: never bound by a test, only judged.
    fminus::web::ServerSetup setup;
    setup.hosts = {"studio.local"};
    const fminus::web::Address lan = address("192.168.1.20");
    CHECK(fminus::web::hostAllowed(setup, "192.168.1.20:8080", lan, 8080));
    CHECK(fminus::web::hostAllowed(setup, "studio.local:8080", lan, 8080));
    CHECK(fminus::web::hostAllowed(setup, "Studio.Local:8080", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "studio.local", lan, 8080));        // the port, left out
    CHECK_FALSE(fminus::web::hostAllowed(setup, "192.168.1.20:8081", lan, 8080));   // another port
    CHECK_FALSE(fminus::web::hostAllowed(setup, "192.168.1.20:08080", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "localhost:8080", lan, 8080));      // not a loopback connection
    CHECK_FALSE(fminus::web::hostAllowed(setup, "127.0.0.1:8080", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "0.0.0.0:8080", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "evil.example:8080", lan, 8080));   // a rebound name
    CHECK_FALSE(fminus::web::hostAllowed(setup, "studio.local.:8080", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, ":8080", lan, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "192.168.1.20:8080:8080", lan, 8080));
    CHECK(fminus::web::hostAllowed(setup, "192.168.1.20", lan, 80));                // port 80 may be left out
    CHECK(fminus::web::hostAllowed(setup, "192.168.1.20:80", lan, 80));
    const fminus::web::Address v6 = address("2001:db8::7");
    CHECK(fminus::web::hostAllowed(setup, "[2001:db8::7]:8080", v6, 8080));
    CHECK(fminus::web::hostAllowed(setup, "[2001:DB8:0::7]:8080", v6, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "[2001:db8::8]:8080", v6, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "[2001:db8::7]8080", v6, 8080));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "[2001:db8::7", v6, 8080));
    // On loopback.
    const fminus::web::Address loop = address("127.0.0.1");
    CHECK(fminus::web::hostAllowed(setup, "localhost:9000", loop, 9000));
    CHECK(fminus::web::hostAllowed(setup, "LOCALHOST:9000", loop, 9000));
    CHECK(fminus::web::hostAllowed(setup, "127.0.0.1:9000", loop, 9000));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "127.0.0.2:9000", loop, 9000));
    CHECK_FALSE(fminus::web::hostAllowed(setup, "[::1]:9000", loop, 9000));
    CHECK(fminus::web::hostAllowed(setup, "[::1]:9000", address("::1"), 9000));
    CHECK(fminus::web::hostAllowed(setup, "localhost:9000", address("::1"), 9000));
    // Origin.
    CHECK(fminus::web::originAllowed("http://127.0.0.1:9000", "127.0.0.1:9000"));
    CHECK(fminus::web::originAllowed("HTTP://LOCALHOST:9000", "localhost:9000"));
    CHECK_FALSE(fminus::web::originAllowed("null", "127.0.0.1:9000"));
    CHECK_FALSE(fminus::web::originAllowed("https://127.0.0.1:9000", "127.0.0.1:9000"));
    CHECK_FALSE(fminus::web::originAllowed("http://127.0.0.1:9001", "127.0.0.1:9000"));
    CHECK_FALSE(fminus::web::originAllowed("http://evil.example", "127.0.0.1:9000"));
    CHECK_FALSE(fminus::web::originAllowed("http://127.0.0.1:9000/", "127.0.0.1:9000"));
}

// ---- handles, routes, formats -----------------------------------------------------------------------------------

TEST_CASE("no server by default: a null context, a policy without a listen, the shared refused server",
          "[web][server]")
{
    const std::string route = "GET /a";
    const char* texts[] = {route.data()};
    const std::int32_t lengths[] = {static_cast<std::int32_t>(route.size())};
    void* none = fminus_web_http_listen(nullptr, texts, lengths, 1);
    REQUIRE(none != nullptr);
    CHECK(fminus_web_http_port(none) == FMINUS_WEB_REFUSED);
    auto context = makeContext(Policy());
    void* refused = listen(*context, {"GET /a"});
    CHECK(refused == none);   // shared
    CHECK(fminus_web_http_port(refused) == FMINUS_WEB_REFUSED);
    CHECK(fminus_web_http_next(refused) == nullptr);
    CHECK(fminus_web_http_accept(refused, -1) == nullptr);   // at once: it never listens
    fminus_web_http_close(refused);
    fminus_web_http_close(refused);
    CHECK(fminus_web_http_port(refused) == FMINUS_WEB_REFUSED);
    CHECK_FALSE(fminus::web::ioThreadRunning());
}

TEST_CASE("handles know their kind: each function does nothing for another's", "[web][server]")
{
    Served served(serving(), nullptr);
    void* server = served.server;
    auto client = clientFor(served.port);
    void* made = clientGet(*client, served.port, "/hello");
    REQUIRE(made != nullptr);
    void* received = fminus_web_http_accept(server, 5000);
    REQUIRE(received != nullptr);
    // A server and a received request are no request made.
    for (void* handle : {server, received})
    {
        CHECK(fminus_web_http_status(handle) == FMINUS_WEB_INVALID);
        CHECK(fminus_web_http_wait(handle, 0) == FMINUS_WEB_INVALID);
        CHECK(fminus_web_http_progress(handle) == 0);
        fminus_web_http_cancel(handle);
    }
    CHECK(fminus_web_http_size(server) == 0);
    CHECK(fminus_web_http_body(server) == nullptr);
    fminus_web_http_close(received);   // nothing: a received request is answered, not closed
    // A request made, and a received one, are no server.
    for (void* handle : {made, received})
    {
        CHECK(fminus_web_http_port(handle) == FMINUS_WEB_INVALID);
        CHECK(fminus_web_http_next(handle) == nullptr);
        CHECK(fminus_web_http_accept(handle, 10) == nullptr);
    }
    // A request made, and a server, are no received request.
    for (void* handle : {made, server})
    {
        CHECK(is(handle, "GET /hello") == 0);
        CHECK(fminus_web_http_arg(handle, 0) == 0);
        CHECK(queryFloat(handle, "a", 2.5f) == 2.5f);
        CHECK(queryInt(handle, "a", 7) == 7);
        CHECK(fminus_web_http_reply(handle, 200) == FMINUS_WEB_INVALID);
        CHECK(replyBytes(handle, 200, "x") == FMINUS_WEB_INVALID);
        CHECK(replyText(handle, 200, "x") == FMINUS_WEB_INVALID);
    }
    // The shared refused request (a null context's) is a request made: no server, no received one.
    const std::string url = "http://127.0.0.1/";
    void* refusedRequest =
        fminus_web_http_get(nullptr, url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0);
    CHECK(fminus_web_http_status(refusedRequest) == FMINUS_WEB_REFUSED);
    CHECK(fminus_web_http_port(refusedRequest) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply(refusedRequest, 200) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_next(refusedRequest) == nullptr);
    // And the refused server is no request.
    void* refusedServer = fminus_web_http_listen(nullptr, nullptr, nullptr, 0);
    CHECK(fminus_web_http_status(refusedServer) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply(refusedServer, 200) == FMINUS_WEB_INVALID);
    // Null.
    CHECK(fminus_web_http_port(nullptr) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_next(nullptr) == nullptr);
    CHECK(fminus_web_http_accept(nullptr, 0) == nullptr);
    CHECK(is(nullptr, "GET /") == 0);
    CHECK(fminus_web_http_reply(nullptr, 200) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_size(nullptr) == 0);
    // The real ones still work.
    CHECK(fminus_web_http_size(received) == 0);
    CHECK(is(received, "GET /hello") == 1);
    CHECK(replyText(received, 200, "hi") == 0);
    CHECK(fminus_web_http_wait(made, 5000) == 200);
    CHECK(bodyOf(made) == "hi");
    CHECK(fminus_web_http_reply(received, 200) == FMINUS_WEB_INVALID);   // answered: the handle is dead
    fminus_web_http_close(made);
}

TEST_CASE("a route's path: each %d a decimal number of 1 to 10 digits that fits an int", "[web][server]")
{
    std::int32_t args[8];
    std::int32_t count = -1;
    CHECK(fminus::web::matchPath("/osc/%d/detune", "/osc/3/detune", args, count));
    CHECK(count == 1);
    CHECK(args[0] == 3);
    CHECK(fminus::web::matchPath("/osc/%d", "/osc/0042", args, count));
    CHECK(args[0] == 42);
    CHECK(fminus::web::matchPath("/osc/%d", "/osc/2147483647", args, count));
    CHECK(args[0] == 2147483647);
    CHECK_FALSE(fminus::web::matchPath("/osc/%d", "/osc/2147483648", args, count));
    CHECK_FALSE(fminus::web::matchPath("/osc/%d", "/osc/00000000001", args, count));   // 11 digits
    CHECK_FALSE(fminus::web::matchPath("/osc/%d", "/osc/", args, count));
    CHECK_FALSE(fminus::web::matchPath("/osc/%d", "/osc/-1", args, count));
    CHECK_FALSE(fminus::web::matchPath("/osc/%d", "/osc/1x", args, count));
    CHECK_FALSE(fminus::web::matchPath("/osc/%d/detune", "/osc/1/detune/", args, count));
    CHECK_FALSE(fminus::web::matchPath("/osc", "/osc/1", args, count));
    CHECK_FALSE(fminus::web::matchPath("/osc/%d", "/OSC/1", args, count));
    CHECK(fminus::web::matchPath("/%d-%d", "/12-34", args, count));
    CHECK(count == 2);
    CHECK(args[1] == 34);
    CHECK_FALSE(fminus::web::matchPath("/%d5", "/45", args, count));   // all the digits there are
    CHECK(fminus::web::matchPath("/%d/%d/%d/%d/%d/%d/%d/%d", "/1/2/3/4/5/6/7/8", args, count));
    CHECK(count == 8);
    CHECK_FALSE(fminus::web::matchPath("/%d/%d/%d/%d/%d/%d/%d/%d/%d", "/1/2/3/4/5/6/7/8/9", args, count));
    CHECK(fminus::web::matchPath("/100%%", "/100%", args, count));   // a percent sign, which rule 2 lets no path have
    CHECK(fminus::web::matchPath("/", "/", args, count));
}

TEST_CASE("a reply's text is what C's printf writes for %d, %g, %.Ng and %%", "[web][server]")
{
    Policy policy = serving();
    policy.server.maxReplyBytes = 4096;
    std::mutex mutex;
    std::vector<std::pair<std::string, std::vector<double>>> cases;
    for (double v : {0.0, -0.0, 1.0, -1.5, 0.1, 1.0 / 3.0, 123456.0, 1234567.0, 1e-5, 1e-4, 1e21, 6.02214076e23,
                     3.4028234663852886e38, 1.401298464324817e-45, 0.75, static_cast<double>(0.1f), 2147483647.0,
                     -2147483648.0, 1e300, 5e-324})
    {
        cases.push_back({"%g", {v}});
        cases.push_back({"%.9g", {v}});
        cases.push_back({"%.17g", {v}});
        cases.push_back({"%.1g|%.0g|%.3g", {v, v, v}});
    }
    for (double v : {0.0, 7.0, -7.0, 2147483647.0, -2147483648.0, 123.0})
        cases.push_back({"[%d]", {v}});
    cases.push_back({"{\"gain\":%g,\"level\":%g,\"n\":%d} 100%%", {0.5, double(0.25f), 3.0}});
    std::vector<std::string> got;
    Served served(policy, [&](void* r) {
        std::lock_guard lock(mutex);
        const auto& [format, values] = cases[got.size()];
        replyText(r, 200, format, values);
        got.push_back("");
    });
    auto client = clientFor(served.port);
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        void* request = clientGet(*client, served.port, "/hello");
        REQUIRE(fminus_web_http_wait(request, 5000) == 200);
        const auto& [format, values] = cases[i];
        char expected[512];
        if (format == "[%d]")
            std::snprintf(expected, sizeof expected, "[%d]", static_cast<int>(values[0]));
        else if (format == "%g")
            std::snprintf(expected, sizeof expected, "%g", values[0]);
        else if (format == "%.9g")
            std::snprintf(expected, sizeof expected, "%.9g", values[0]);
        else if (format == "%.17g")
            std::snprintf(expected, sizeof expected, "%.17g", values[0]);
        else if (format == "%.1g|%.0g|%.3g")
            std::snprintf(expected, sizeof expected, "%.1g|%.0g|%.3g", values[0], values[1], values[2]);
        else
            std::snprintf(expected, sizeof expected, "{\"gain\":%g,\"level\":%g,\"n\":%d} 100%%", values[0],
                          values[1], static_cast<int>(values[2]));
        INFO(format << " of " << values[0]);
        CHECK(bodyOf(request) == expected);
        fminus_web_http_close(request);
    }
}

TEST_CASE("a format the library trusts no byte of: stray percents, missing values, %d of a non-int",
          "[web][server]")
{
    std::mutex mutex;
    // %r, %R and %b were a JSON literal's, which the JSON writer writes now: each is itself, as %s is here (only
    // fminus_web_http_reply_text_s fills a %s).
    std::vector<std::string> formats = {"%", "a%", "%x%q", "%d %g %.5g", "%.g", "%.123g", "%.5", "%%%d", "%r%R%b%s %d"};
    std::vector<std::vector<double>> values = {{}, {}, {}, {1.0}, {2.0}, {3.0}, {}, {std::nan(""), 1.0}, {4.0}};
    std::size_t next = 0;
    Served served(serving(), [&](void* r) {
        std::lock_guard lock(mutex);
        replyText(r, 200, formats[next], values[next]);
        ++next;
    });
    auto client = clientFor(served.port);
    const std::vector<std::string> expected = {"%", "a%", "%x%q", "1  ", "%.g", "%.123g", "%.5", "%0", "%r%R%b%s 4"};
    for (std::size_t i = 0; i < formats.size(); ++i)
    {
        void* request = clientGet(*client, served.port, "/hello");
        REQUIRE(fminus_web_http_wait(request, 5000) == 200);
        INFO(formats[i]);
        CHECK(bodyOf(request) == expected[i]);
        fminus_web_http_close(request);
    }
}

TEST_CASE("a reply's text with strings: each %s the next string as it is, the values in the other places",
          "[web][server]")
{
    struct Case
    {
        std::string format;
        std::vector<double> values;
        std::vector<std::string> texts;
        std::string expected;
    };
    const std::vector<Case> cases = {
        {"%s", {}, {"warm"}, "warm"},
        {"%s=%g, %s=%d %%", {0.5, 7.0}, {"gain", "n"}, "gain=0.5, n=7 %"},
        {"[%s][%s]", {}, {"", std::string("a\0b\n\"\xff", 6)}, std::string("[][a\0b\n\"\xff]", 10)},
        {"%s %s %s", {}, {"one"}, "one  "},   // a %s past the last string is nothing
        {"no places", {}, {"unused"}, "no places"},
        {"%d%s%g", {1.0, 2.0}, {"-"}, "1-2"},
    };
    std::mutex mutex;
    std::size_t next = 0;
    Served served(serving(), [&](void* r) {
        std::lock_guard lock(mutex);
        const Case& c = cases[next++];
        std::vector<const char*> data;
        std::vector<std::int32_t> lengths;
        for (const std::string& text : c.texts)
        {
            data.push_back(text.data());
            lengths.push_back(static_cast<std::int32_t>(text.size()));
        }
        fminus_web_http_reply_text_s(r, 200, "", 0, c.format.data(), static_cast<std::int32_t>(c.format.size()),
                                     c.values.data(), static_cast<std::int32_t>(c.values.size()), data.data(),
                                     lengths.data(), static_cast<std::int32_t>(data.size()));
    });
    auto client = clientFor(served.port);
    for (const Case& c : cases)
    {
        void* request = clientGet(*client, served.port, "/hello");
        REQUIRE(fminus_web_http_wait(request, 5000) == 200);
        INFO(c.format);
        CHECK(bodyOf(request) == c.expected);
        fminus_web_http_close(request);
    }
    // A string that is not one, a negative length or a null address with bytes, is FMINUS_WEB_INVALID and sends
    // nothing; null arrays, or none, leave every %s empty.
    Served held(serving(), nullptr);
    auto heldClient = clientFor(held.port);
    void* asked = clientGet(*heldClient, held.port, "/hello");
    void* received = fminus_web_http_accept(held.server, 5000);
    REQUIRE(received != nullptr);
    const char* texts[] = {"x", nullptr};
    const std::int32_t negative[] = {1, -1};
    const std::int32_t nullWithBytes[] = {1, 3};
    CHECK(fminus_web_http_reply_text_s(received, 200, "", 0, "%s%s", 4, nullptr, 0, texts, negative, 2)
          == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply_text_s(received, 200, "", 0, "%s%s", 4, nullptr, 0, texts, nullWithBytes, 2)
          == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply_text_s(nullptr, 200, "", 0, "%s", 2, nullptr, 0, texts, negative, 1)
          == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply_text_s(received, 200, "", 0, "<%s>", 4, nullptr, 0, nullptr, nullptr, 2) == 0);
    REQUIRE(fminus_web_http_wait(asked, 5000) == 200);
    CHECK(bodyOf(asked) == "<>");
    fminus_web_http_close(asked);
}

// ---- the library's own client against the server ------------------------------------------------------------------

TEST_CASE("the library's client against its server: routes, numbers in a path, a query, a body", "[web][server]")
{
    Served served(serving());
    auto client = clientFor(served.port);
    const auto fetch = [&](void* request) {
        const std::int32_t status = fminus_web_http_wait(request, 5000);
        std::string body = bodyOf(request);
        fminus_web_http_close(request);
        return std::pair(status, body);
    };
    CHECK(fetch(clientGet(*client, served.port, "/hello")) == std::pair(200, std::string("hello")));
    CHECK(fetch(clientGet(*client, served.port, "/osc/17/detune")) == std::pair(200, std::string("osc 17")));
    CHECK(fetch(clientGet(*client, served.port, "/osc/x/detune")).first == 404);
    CHECK(fetch(clientGet(*client, served.port, "/nothing")).first == 404);
    CHECK(fetch(clientPost(*client, served.port, "/table/3", std::string(1000, 'a'), "application/octet-stream")) ==
          std::pair(201, std::string("table 3: 1000 bytes [%s]")));
    CHECK(fetch(clientPost(*client, served.port, "/hello", "x", "application/json")).first == 405);
    // A form's content type, which any page may send without asking.
    CHECK(fetch(clientPost(*client, served.port, "/table/3", "x", "text/plain")).first == 415);
    CHECK(fetch(clientPost(*client, served.port, "/table/3", "x", "application/x-www-form-urlencoded")).first == 415);
    CHECK(fetch(clientPost(*client, served.port, "/table/3", "x", "multipart/form-data; boundary=a")).first == 415);
}

TEST_CASE("a query's numbers: the first field of the name, read whole, finite, or the fallback", "[web][server]")
{
    std::mutex mutex;
    std::vector<std::pair<float, std::int32_t>> seen;
    Served served(serving(), [&](void* r) {
        std::lock_guard lock(mutex);
        seen.push_back({queryFloat(r, "v", -9.0f), queryInt(r, "n", -9)});
        fminus_web_http_reply(r, 204);
    });
    const std::vector<std::pair<std::string, std::pair<float, std::int32_t>>> cases = {
        {"/hello?v=0.5&n=3", {0.5f, 3}},
        {"/hello?n=-12&v=1e3", {1000.0f, -12}},
        {"/hello?v=0.1", {0.1f, -9}},
        {"/hello?v=1&v=2", {1.0f, -9}},
        {"/hello?v=nan&n=1.5", {-9.0f, -9}},
        {"/hello?v=inf&n=2147483648", {-9.0f, -9}},
        {"/hello?v=-infinity&n=+1", {-9.0f, -9}},
        {"/hello?v=1e39&n=007", {-9.0f, 7}},
        {"/hello?v=1e-50&n=-0", {0.0f, 0}},
        {"/hello?v=0x10&n=0x10", {-9.0f, -9}},
        {"/hello?v=%31&n=%31", {-9.0f, -9}},   // never decoded
        {"/hello?v=+1&n=", {-9.0f, -9}},
        {"/hello?v&n", {-9.0f, -9}},
        {"/hello?vv=1&nn=1", {-9.0f, -9}},
        {"/hello?v=1.5x&n=2x", {-9.0f, -9}},
        {"/hello?x=1&v=2.25&n=-2147483648", {2.25f, -2147483647 - 1}},
        {"/hello", {-9.0f, -9}},
        {"/hello?", {-9.0f, -9}},
    };
    for (const auto& [target, expected] : cases)
    {
        const Response response = roundTrip(served.port, get(served.port, target));
        INFO(target);
        CHECK(response.status == 204);
        std::lock_guard lock(mutex);
        REQUIRE(!seen.empty());
        CHECK(seen.back() == expected);
    }
}

TEST_CASE("http_is: a method and a path, a GET route for HEAD, the numbers kept from the last match",
          "[web][server]")
{
    std::mutex mutex;
    std::vector<std::string> notes;
    Served served(serving(), [&](void* r) {
        std::string note;
        note += std::to_string(is(r, "PATCH /a/%d/b/%d"));
        note += std::to_string(fminus_web_http_arg(r, 0)) + "," + std::to_string(fminus_web_http_arg(r, 1));
        note += std::to_string(is(r, "PATCH /a/%d/c/%d"));   // no match: the numbers stay
        note += std::to_string(fminus_web_http_arg(r, 0)) + "," + std::to_string(fminus_web_http_arg(r, 1));
        note += std::to_string(fminus_web_http_arg(r, 2)) + std::to_string(fminus_web_http_arg(r, -1));
        note += std::to_string(is(r, "GET /a/%d/b/%d")) + std::to_string(is(r, "patch /a/%d/b/%d"));
        note += std::to_string(is(r, "PATCH  /a/%d/b/%d")) + std::to_string(is(r, "PATCH"));
        {
            std::lock_guard lock(mutex);
            notes.push_back(note);
        }
        fminus_web_http_reply(r, 204);
    });
    CHECK(roundTrip(served.port, "PATCH /a/5/b/0007 HTTP/1.1\r\nHost: " + hostOf(served.port) +
                                    "\r\nContent-Length: 0\r\n\r\n")
              .status == 204);
    {
        std::lock_guard lock(mutex);
        REQUIRE(notes.size() == 1);
        CHECK(notes[0] == "15,705,7000000");
    }
    // HEAD on a GET route: the program sees HEAD, a GET literal matches it, and the reply's body is dropped.
    Served heads(serving(), [&](void* r) {
        {
            std::lock_guard lock(mutex);
            notes.push_back(std::to_string(is(r, "GET /hello")) + std::to_string(is(r, "HEAD /hello")));
        }
        replyText(r, 200, "twelve bytes");
    });
    const Response head = roundTrip(heads.port, "HEAD /hello HTTP/1.1\r\nHost: " + hostOf(heads.port) + "\r\n\r\n", true);
    CHECK(head.status == 200);
    CHECK(head.header("content-length") == "12");
    CHECK(head.body.empty());
    std::lock_guard lock(mutex);
    CHECK(notes.back() == "11");
}

// ---- a request taken by route: its route, a text place, its text ---------------------------------------------------

namespace
{
    std::string textOf(const char* bytes, std::int32_t length)
    {
        return bytes == nullptr ? std::string("<null>") : std::string(bytes, static_cast<std::size_t>(length));
    }

    std::string placeText(const void* request, std::int32_t index)
    {
        std::int32_t length = -1;
        const char* bytes = fminus_web_http_text(request, index, &length);
        return textOf(bytes, length);
    }

    std::string pathText(const void* request)
    {
        std::int32_t length = -1;
        const char* bytes = fminus_web_http_path(request, &length);
        return textOf(bytes, length);
    }

    std::string headerText(const void* request, std::string_view name)
    {
        std::int32_t length = -1;
        const char* bytes =
            fminus_web_http_header(request, name.data(), static_cast<std::int32_t>(name.size()), &length);
        return textOf(bytes, length);
    }

    std::string queryText(const void* request, std::string_view name)
    {
        std::int32_t length = -1;
        const char* bytes =
            fminus_web_http_query_text(request, name.data(), static_cast<std::int32_t>(name.size()), &length);
        return textOf(bytes, length);
    }

    // A route set of the table's indexes, as a program's constant would be.
    std::vector<std::uint64_t> routeSet(std::initializer_list<int> indexes, std::size_t words = 1)
    {
        std::vector<std::uint64_t> set(words, 0);
        for (int index : indexes)
            set[static_cast<std::size_t>(index) / 64] |= std::uint64_t(1) << (index % 64);
        return set;
    }

    void* nextIn(void* server, const std::vector<std::uint64_t>& set)
    {
        return fminus_web_http_next_in(server, set.data(), static_cast<std::int32_t>(set.size()));
    }

    void* acceptIn(void* server, const std::vector<std::uint64_t>& set, std::int32_t timeoutMs)
    {
        return fminus_web_http_accept_in(server, set.data(), static_cast<std::int32_t>(set.size()), timeoutMs);
    }

    std::string request(int port, const std::string& method, const std::string& target, const std::string& more = {})
    {
        return method + " " + target + " HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n" + more +
               "Content-Length: 0\r\n\r\n";
    }

    // Sends each request on a connection of its own, in order, each published before the next is sent (the
    // connections are returned, open, for the caller to read and close).
    std::vector<int> sendEach(void* server, int port, const std::vector<std::string>& requests)
    {
        std::vector<int> fds;
        for (const std::string& text : requests)
        {
            const int fd = connectTo(port);
            REQUIRE(sendAll(fd, text));
            fds.push_back(fd);
            std::this_thread::sleep_for(30ms);   // published, in this order (the I/O thread turns every millisecond)
        }
        (void)server;
        return fds;
    }
} // namespace

TEST_CASE("a route's %s: one whole segment as it is, a segment by itself, and its place's bytes", "[web][server]")
{
    fminus::web::Place places[fminus::web::kMaxPlaces];
    std::int32_t count = -1;
    const auto text = [&](std::string_view path, std::int32_t i)
    {
        return std::string(path.substr(places[i].offset, places[i].length));
    };
    std::string_view path = "/presets/My-preset_1.v2~x";
    CHECK(fminus::web::matchRoute("/presets/%s", path, places, count));
    CHECK(count == 1);
    CHECK(places[0].text);
    CHECK(places[0].value == 0);
    CHECK(text(path, 0) == "My-preset_1.v2~x");
    path = "/u/ann/n/0042/k";
    CHECK(fminus::web::matchRoute("/u/%s/n/%d/%s", path, places, count));
    CHECK(count == 3);
    CHECK(text(path, 0) == "ann");
    CHECK_FALSE(places[1].text);
    CHECK(places[1].value == 42);
    CHECK(text(path, 1) == "0042");   // a number's place: its digits as the path has them
    CHECK(text(path, 2) == "k");
    CHECK(fminus::web::matchRoute("/%s", "/x", places, count));
    CHECK(fminus::web::matchRoute("/%s/%s/%s/%s/%s/%s/%s/%d", "/a/b/c/d/e/f/g/8", places, count));
    CHECK(count == 8);
    CHECK_FALSE(fminus::web::matchRoute("/%s/%s/%s/%s/%s/%s/%s/%s/%s", "/a/b/c/d/e/f/g/h/i", places, count));
    // One segment, whole: never empty, never two, never "." or "..", only the bytes rule 2 allows.
    CHECK_FALSE(fminus::web::matchRoute("/presets/%s", "/presets/", places, count));
    CHECK_FALSE(fminus::web::matchRoute("/presets/%s", "/presets", places, count));
    CHECK_FALSE(fminus::web::matchRoute("/presets/%s", "/presets/a/b", places, count));
    CHECK_FALSE(fminus::web::matchRoute("/presets/%s", "/presets/.", places, count));
    CHECK_FALSE(fminus::web::matchRoute("/presets/%s", "/presets/..", places, count));
    CHECK(fminus::web::matchRoute("/presets/%s", "/presets/...", places, count));
    for (const char* bad : {"/presets/a b", "/presets/a%2F", "/presets/a?b", "/presets/a\\b", "/presets/\xc3\xa9"})
    {
        INFO(bad);
        CHECK_FALSE(fminus::web::matchRoute("/presets/%s", bad, places, count));
    }
    // A %s that is part of a segment never matches: not even the bytes "%s".
    for (const char* pattern : {"/a%s", "/%sx", "/presets/%s.json", "%s", "/a/%s%d", "/a/%d%s"})
    {
        INFO(pattern);
        CHECK_FALSE(fminus::web::matchRoute(pattern, "/a", places, count));
        CHECK_FALSE(fminus::web::matchRoute(pattern, "/ax", places, count));
        CHECK_FALSE(fminus::web::matchRoute(pattern, "/presets/a.json", places, count));
        CHECK_FALSE(fminus::web::matchRoute(pattern, "/a/1x", places, count));
        CHECK_FALSE(fminus::web::matchRoute(pattern, "/a/x1", places, count));
    }
    CHECK(fminus::web::matchRoute("/a/%%s", "/a/%s", places, count));   // "%%" is a percent sign, as before
    CHECK(count == 0);
    // http_arg's numbers through the old matcher: a text's place is 0.
    std::int32_t args[8] = {-1, -1, -1};
    CHECK(fminus::web::matchPath("/%s/%d", "/x/7", args, count));
    CHECK(count == 2);
    CHECK(args[0] == 0);
    CHECK(args[1] == 7);
}

TEST_CASE("which route is more specific: segment by segment, a literal before a %d before a %s", "[web][server]")
{
    using fminus::web::compareSpecificity;
    CHECK(compareSpecificity("/a/b", "/a/%d") < 0);
    CHECK(compareSpecificity("/a/%d", "/a/%s") < 0);
    CHECK(compareSpecificity("/a/b", "/a/%s") < 0);
    CHECK(compareSpecificity("/a/%s", "/a/b") > 0);
    CHECK(compareSpecificity("/v%d", "/%d-%d") < 0);      // fewer numbers in the segment
    CHECK(compareSpecificity("/x/%d/%s", "/x/%s/y") < 0);  // the first segment that differs decides
    CHECK(compareSpecificity("/a/%s", "/%d/b") < 0);
    CHECK(compareSpecificity("/a/%d", "/b/%d") == 0);
    CHECK(compareSpecificity("/a/%d", "/a/%d") == 0);
    CHECK(compareSpecificity("/a/v%d", "/a/%dx") == 0);    // the same rank: the table's order decides
    CHECK(compareSpecificity("/", "/") == 0);
}

TEST_CASE("each request is published for the most specific route that matches it; http_is is as it was",
          "[web][server]")
{
    // What the library did before a request carried its route: it chose none. Rule 6 asked only whether some route
    // matched (404) and admitted the method (405), and every literal that matches a request gives http_is 1, so a
    // program's chain of http_is took the first that matched in its own order. http_is still does exactly that.
    const std::vector<std::string> routes = {
        "not a route",      // 0: left out of the table, but the indexes after it keep their places
        "GET /a/%s",        // 1
        "GET /a/%d",        // 2
        "GET /a/b",         // 3
        "POST /a/%s",       // 4
        "GET /x/%d/%s",     // 5
        "GET /x/%s/y",      // 6
        "GET /p/v%d",       // 7
        "GET /p/%dx",       // 8: ranks as 7 does
        "GET /a/b",         // 9: the same text again: 3 is first
        "PUT /presets/%s",  // 10
    };
    std::mutex mutex;
    std::vector<std::string> notes;
    Served served(serving(), [&](void* r) {
        std::string note = std::to_string(fminus_web_http_route(r));
        for (std::int32_t i = 0; i < 3; ++i)
            note += "|" + placeText(r, i) + ":" + std::to_string(fminus_web_http_arg(r, i));
        note += "|is";
        for (const std::string& route : routes)
            note += std::to_string(is(r, route));
        {
            std::lock_guard lock(mutex);
            notes.push_back(note);
        }
        fminus_web_http_reply(r, 204);
    }, routes);
    const auto ask = [&](const std::string& method, const std::string& target)
    {
        const Response response = roundTrip(served.port, request(served.port, method, target), method == "HEAD");
        std::lock_guard lock(mutex);
        if (response.status != 204 || notes.empty())
            return "status " + std::to_string(response.status);
        std::string note = notes.back();
        notes.clear();
        return note;
    };
    CHECK(ask("GET", "/a/b") == "3|<null>:0|<null>:0|<null>:0|is01010000010");
    CHECK(ask("HEAD", "/a/b") == "3|<null>:0|<null>:0|<null>:0|is01010000010");
    CHECK(ask("GET", "/a/0042") == "2|0042:42|<null>:0|<null>:0|is01100000000");
    CHECK(ask("GET", "/a/zz") == "1|zz:0|<null>:0|<null>:0|is01000000000");
    CHECK(ask("POST", "/a/b") == "4|b:0|<null>:0|<null>:0|is00001000000");
    CHECK(ask("GET", "/x/1/y") == "5|1:1|y:0|<null>:0|is00000110000");
    CHECK(ask("GET", "/x/q/y") == "6|q:0|<null>:0|<null>:0|is00000010000");
    CHECK(ask("GET", "/p/v12") == "7|12:12|<null>:0|<null>:0|is00000001000");
    CHECK(ask("PUT", "/presets/warm.pad") == "10|warm.pad:0|<null>:0|<null>:0|is00000000001");
    CHECK(ask("GET", "/presets/warm.pad") == "status 405");
    CHECK(ask("GET", "/a") == "status 404");
    // http_route makes the published route's places the request's again, after an http_is of another.
    Served again(serving(), [&](void* r) {
        // One call a statement: the operands of + are unsequenced, and the calls write the places they read.
        std::string note = std::to_string(is(r, "GET /a/%s"));
        note += placeText(r, 0);
        note += std::to_string(fminus_web_http_route(r));
        note += placeText(r, 0);
        note += std::to_string(fminus_web_http_arg(r, 0));
        note += std::to_string(is(r, "GET /a/%s"));
        note += placeText(r, 0);
        {
            std::lock_guard lock(mutex);
            notes.push_back(note);
        }
        fminus_web_http_reply(r, 204);
    }, routes);
    CHECK(roundTrip(again.port, request(again.port, "GET", "/a/7")).status == 204);
    std::lock_guard lock(mutex);
    REQUIRE(!notes.empty());
    CHECK(notes.back() == "1727717");
}

TEST_CASE("taken by route set: each taker takes its own routes' requests, oldest first, and no other",
          "[web][server]")
{
    const std::vector<std::string> routes = {"GET /audio/%d", "PUT /preset/%s", "GET /other"};
    Policy policy = serving();
    policy.server.replyTimeoutMs = 400;
    Served served(policy, nullptr, routes);
    const int port = served.port;
    const auto audio = routeSet({0});
    const auto loader = routeSet({1});
    // The set's representation: words of 64 bits, a route past the words in none, null or no words the empty set.
    CHECK(nextIn(served.server, {}) == nullptr);
    CHECK(fminus_web_http_next_in(served.server, nullptr, 1) == nullptr);
    std::vector<int> fds = sendEach(served.server, port,
                                    {request(port, "PUT", "/preset/a"), request(port, "GET", "/audio/1"),
                                     request(port, "PUT", "/preset/b"), request(port, "GET", "/audio/2")});
    CHECK(fminus_web_http_next_in(served.server, audio.data(), 0) == nullptr);
    CHECK(fminus_web_http_next_in(served.server, nullptr, 0) == nullptr);
    CHECK(nextIn(served.server, routeSet({2})) == nullptr);    // none waits for that route
    CHECK(nextIn(served.server, routeSet({}, 4)) == nullptr);
    // The audio thread's set skips the older PUT, and takes its two in their order.
    void* first = nextIn(served.server, audio);
    REQUIRE(first != nullptr);
    CHECK(pathText(first) == "/audio/1");
    CHECK(fminus_web_http_route(first) == 0);
    CHECK(fminus_web_http_arg(first, 0) == 1);
    void* second = nextIn(served.server, audio);
    REQUIRE(second != nullptr);
    CHECK(pathText(second) == "/audio/2");
    CHECK(nextIn(served.server, audio) == nullptr);
    // The loader's, oldest first, through a set of several words (route 1 in the first).
    const auto wide = routeSet({1, 100, 127}, 2);
    void* third = nextIn(served.server, wide);
    REQUIRE(third != nullptr);
    CHECK(pathText(third) == "/preset/a");
    CHECK(fminus_web_http_route(third) == 1);
    CHECK(placeText(third, 0) == "a");
    void* fourth = acceptIn(served.server, loader, 1000);
    REQUIRE(fourth != nullptr);
    CHECK(pathText(fourth) == "/preset/b");
    for (void* r : {first, second, third, fourth})
        CHECK(fminus_web_http_reply(r, 204) == 0);
    for (int fd : fds)
    {
        Reader reader(fd);
        CHECK(reader.next()->status == 204);
        ::close(fd);
    }
    // Every route in the set: the same as http_next.
    fds = sendEach(served.server, port, {request(port, "GET", "/other")});
    void* other = nextIn(served.server, routeSet({0, 1, 2}));
    REQUIRE(other != nullptr);
    CHECK(fminus_web_http_route(other) == 2);
    CHECK(fminus_web_http_reply(other, 204) == 0);
    ::close(fds[0]);
    // A request whose route is in no thread's set waits, untaken, and is answered 503 at replyTimeoutMs; a blocking
    // take for another set times out, and does not consume it.
    const int lonely = connectTo(port);
    REQUIRE(sendAll(lonely, request(port, "GET", "/other")));
    const auto started = std::chrono::steady_clock::now();
    CHECK(acceptIn(served.server, audio, 150) == nullptr);
    CHECK(std::chrono::steady_clock::now() - started >= 140ms);
    CHECK(nextIn(served.server, loader) == nullptr);
    Reader reader(lonely);
    const std::optional<Response> timedOut = reader.next();
    REQUIRE(timedOut);
    CHECK(timedOut->status == 503);
    CHECK(fminus_web_http_next(served.server) == nullptr);   // given up on: its slot free again
    ::close(lonely);
}

TEST_CASE("taken by route set: a blocked taker is woken by its own route, never starved by another's",
          "[web][server]")
{
    const std::vector<std::string> routes = {"GET /audio/%d", "GET /preset/%s"};
    Policy policy = serving();
    policy.server.maxConnections = 64;
    policy.server.maxPending = 8;
    policy.server.replyTimeoutMs = 10000;
    Served served(policy, nullptr, routes);
    const int port = served.port;
    // One thread blocked for the loader's route while the audio route's requests pile up untaken: it wakes for its
    // own the moment it comes.
    {
        const int a = connectTo(port);
        const int b = connectTo(port);
        const int c = connectTo(port);
        std::atomic<bool> returned{false};
        void* got = nullptr;
        std::thread loader([&] {
            got = acceptIn(served.server, routeSet({1}), -1);
            returned.store(true);
        });
        REQUIRE(sendAll(a, request(port, "GET", "/audio/1")));
        REQUIRE(sendAll(b, request(port, "GET", "/audio/2")));
        std::this_thread::sleep_for(150ms);
        CHECK_FALSE(returned.load());
        REQUIRE(sendAll(c, request(port, "GET", "/preset/p")));
        loader.join();
        REQUIRE(got != nullptr);
        CHECK(pathText(got) == "/preset/p");
        CHECK(fminus_web_http_reply(got, 204) == 0);
        // The audio route's requests are still there, for the audio set.
        void* one = nextIn(served.server, routeSet({0}));
        void* two = nextIn(served.server, routeSet({0}));
        REQUIRE(one != nullptr);
        REQUIRE(two != nullptr);
        CHECK(pathText(one) == "/audio/1");
        CHECK(pathText(two) == "/audio/2");
        fminus_web_http_reply(one, 204);
        fminus_web_http_reply(two, 204);
        for (int fd : {a, b, c})
            ::close(fd);
    }
    // Two takers, one per route, and a stream of both kinds from two clients: each takes only its own, all are
    // answered by the right one.
    std::atomic<bool> stop{false};
    std::atomic<int> wrong{0};
    std::atomic<int> answered{0};
    std::vector<std::thread> takers;
    for (int t = 0; t < 2; ++t)
        takers.emplace_back([&, t] {
            const auto mine = routeSet({t});
            while (!stop.load())
            {
                void* r = t == 0 ? nextIn(served.server, mine) : acceptIn(served.server, mine, 5);
                if (r == nullptr)
                {
                    if (t == 0)
                        std::this_thread::sleep_for(1ms);
                    continue;
                }
                if (fminus_web_http_route(r) != t)
                    wrong.fetch_add(1);
                replyText(r, 200, "%d", {double(t)});
                answered.fetch_add(1);
            }
        });
    auto client = clientFor(port);
    std::vector<std::pair<int, void*>> requests;
    int ok = 0;
    for (int i = 0; i < 96; ++i)
    {
        const int kind = (i * 7) % 3 == 0 ? 1 : 0;
        requests.push_back({kind, clientGet(*client, port, (kind == 0 ? "/audio/" : "/preset/p") + std::to_string(i))});
        if (requests.size() < 16)
            continue;
        for (const auto& [expected, r] : requests)
        {
            const std::int32_t status = fminus_web_http_wait(r, 20000);
            if (status == 200)
            {
                CHECK(bodyOf(r) == std::to_string(expected));
                ++ok;
            }
            else
                CHECK(status == 503);   // every slot taken at that moment
            fminus_web_http_close(r);
        }
        requests.clear();
    }
    stop.store(true);
    for (std::thread& t : takers)
        t.join();
    CHECK(wrong.load() == 0);
    CHECK(ok > 0);
    CHECK(answered.load() == ok);
}

TEST_CASE("a request's text: its path, a header by name, a query's field as text", "[web][server]")
{
    std::mutex mutex;
    std::vector<std::string> notes;
    const std::vector<std::string> routes = {"GET /hello", "PUT /presets/%s"};
    const auto handler = [&](void* r) {
        std::string note = pathText(r);
        for (const char* name : {"x-preset", "X-PRESET", "x-empty", "x-none", "host", "x preset", "", "authorization",
                                 "x-long"})
        {
            const std::string value = headerText(r, name);
            note += "|" + (std::string_view(name) == "x-long" ? std::to_string(value.size()) : value);
        }
        for (const char* name : {"a", "b", "c", "d", "", "%61"})
            note += "|" + queryText(r, name);
        {
            std::lock_guard lock(mutex);
            notes.push_back(note);
        }
        fminus_web_http_reply(r, 204);
    };
    const std::string headers = "X-Preset: \t warm pad \t\r\nx-preset: second\r\nX-Empty:\r\n"
                                "Authorization: Bearer Tok-en_0123456789abcdef\r\nX-Long: " +
                                std::string(10000, 'v') + "\r\n";
    const auto noteOf = [&](int port, const std::string& target) {
        const Response response = roundTrip(port, request(port, "PUT", target, headers));
        std::lock_guard lock(mutex);
        if (response.status != 204 || notes.empty())
            return "status " + std::to_string(response.status);
        return notes.back();
    };
    {
        Served served(serving(), handler, routes);
        const std::string host = hostOf(served.port);
        CHECK(noteOf(served.port, "/presets/warm?a=1&b=&c&a=2&%61=x&d=%20") ==
              "/presets/warm|warm pad|warm pad|||" + host + "|||Bearer Tok-en_0123456789abcdef|10000|1|||%20|<null>|x");
        CHECK(noteOf(served.port, "/presets/warm") ==
              "/presets/warm|warm pad|warm pad|||" + host +
                  "|||Bearer Tok-en_0123456789abcdef|10000|<null>|<null>|<null>|<null>|<null>|<null>");
    }
    {
        // With a token, the Authorization field is the host's secret: never the program's.
        Policy policy = serving();
        policy.server.token = "Tok-en_0123456789abcdef";
        Served served(policy, handler, routes);
        const std::string host = hostOf(served.port);
        CHECK(noteOf(served.port, "/presets/x?a=") ==
              "/presets/x|warm pad|warm pad|||" + host + "||||10000||<null>|<null>|<null>|<null>|<null>");
    }
    // 64 fields, the most a request may have, each found; the bytes of the request a slot held before are not.
    {
        std::vector<std::string> got;
        Served served(serving(), [&](void* r) {
            std::string note;
            for (int i = 0; i < 62; ++i)
                note += headerText(r, "f" + std::to_string(i)) + ",";
            note += headerText(r, "x-preset") + "," + headerText(r, "host").substr(0, 9);
            {
                std::lock_guard lock(mutex);
                got.push_back(note);
            }
            fminus_web_http_reply(r, 204);
        }, routes);
        std::string many;
        std::string expected;
        for (int i = 0; i < 62; ++i)
        {
            many += "F" + std::to_string(i) + ": " + std::to_string(i * 3) + "\r\n";
            expected += std::to_string(i * 3) + ",";
        }
        expected += ",127.0.0.1";
        // Host and Content-Length are the other two: 64.
        CHECK(roundTrip(served.port, request(served.port, "PUT", "/presets/a", many)).status == 204);
        CHECK(roundTrip(served.port, request(served.port, "PUT", "/presets/a", many + "X-Extra: 1\r\n")).status == 431);
        std::lock_guard lock(mutex);
        REQUIRE(got.size() == 1);
        CHECK(got[0] == expected);
    }
}

TEST_CASE("the new functions do nothing for a handle of another kind, nor for one not held", "[web][server]")
{
    Served served(serving(), nullptr);
    auto client = clientFor(served.port);
    void* made = clientGet(*client, served.port, "/hello");
    const auto all = routeSet({0, 1, 2, 3, 4, 5, 6, 7, 8});
    void* received = acceptIn(served.server, all, 5000);
    REQUIRE(received != nullptr);
    void* refusedServer = fminus_web_http_listen(nullptr, nullptr, nullptr, 0);
    for (void* handle : {made, served.server, refusedServer, static_cast<void*>(nullptr)})
    {
        CHECK(fminus_web_http_route(handle) == FMINUS_WEB_INVALID);
        std::int32_t length = 99;
        CHECK(fminus_web_http_text(handle, 0, &length) == nullptr);
        CHECK(length == 0);
        length = 99;
        CHECK(fminus_web_http_path(handle, &length) == nullptr);
        CHECK(length == 0);
        CHECK(fminus_web_http_header(handle, "host", 4, &length) == nullptr);
        CHECK(fminus_web_http_query_text(handle, "a", 1, &length) == nullptr);
        CHECK(fminus_web_http_path(handle, nullptr) == nullptr);   // no length wanted
    }
    for (void* handle : {made, received, static_cast<void*>(nullptr)})
    {
        CHECK(nextIn(handle, all) == nullptr);
        CHECK(acceptIn(handle, all, 10) == nullptr);
    }
    CHECK(nextIn(refusedServer, all) == nullptr);
    CHECK(acceptIn(refusedServer, all, -1) == nullptr);   // at once: it never listens
    CHECK(fminus_web_http_route(received) == 0);
    CHECK(pathText(received) == "/hello");
    CHECK(fminus_web_http_path(received, nullptr) != nullptr);
    CHECK(headerText(received, "nope") == "");
    CHECK(fminus_web_http_header(received, nullptr, 3, nullptr) != nullptr);   // no name: empty
    CHECK(queryText(received, "") == "<null>");
    CHECK(placeText(received, 0) == "<null>");
    CHECK(placeText(received, -1) == "<null>");
    CHECK(fminus_web_http_reply(received, 200) == 0);
    // Answered: the handle is dead.
    CHECK(fminus_web_http_route(received) == FMINUS_WEB_INVALID);
    CHECK(pathText(received) == "<null>");
    CHECK(fminus_web_http_wait(made, 5000) == 200);
    fminus_web_http_close(made);
}

TEST_CASE("a deadline scope bounds the thread's blocking takes, and only that thread's", "[web][server][within]")
{
    Served served(serving(), nullptr);
    void* context = served.context->handle();
    const auto all = routeSet({0, 1, 2, 3, 4, 5, 6, 7, 8});
    const auto since = [](std::chrono::steady_clock::time_point start) { return std::chrono::steady_clock::now() - start; };
    auto start = std::chrono::steady_clock::now();
    REQUIRE(fminus_web_http_within_begin(context, 200) == 0);
    CHECK(fminus_web_http_accept(served.server, -1) == nullptr);   // what it returns on its own timeout
    CHECK(since(start) >= 190ms);
    CHECK(since(start) < 2s);
    CHECK(acceptIn(served.server, all, -1) == nullptr);           // passed: at once
    CHECK(fminus_web_http_accept(served.server, 5000) == nullptr);
    CHECK(since(start) < 2s);
    // Another thread's waits are its own.
    std::chrono::steady_clock::duration theirs{};
    std::thread([&] {
        const auto began = std::chrono::steady_clock::now();
        acceptIn(served.server, all, 300);
        theirs = std::chrono::steady_clock::now() - began;
    }).join();
    CHECK(theirs >= 290ms);
    CHECK(fminus_web_http_within_end(context) == 0);
    // A shorter timeout of the wait's own still counts; a later deadline than the wait's is no bound.
    REQUIRE(fminus_web_http_within_begin(context, 5000) == 0);
    start = std::chrono::steady_clock::now();
    CHECK(acceptIn(served.server, all, 100) == nullptr);
    CHECK(since(start) >= 90ms);
    CHECK(since(start) < 2s);
    // A request that comes before the deadline is taken.
    auto client = clientFor(served.port);
    void* made = clientGet(*client, served.port, "/hello");
    void* received = fminus_web_http_accept(served.server, -1);
    REQUIRE(received != nullptr);
    CHECK(replyText(received, 200, "in time") == 0);
    CHECK(fminus_web_http_within_end(context) == 0);
    CHECK(fminus_web_http_wait(made, 5000) == 200);
    fminus_web_http_close(made);
}

// ---- the rules, one by one ----------------------------------------------------------------------------------------

TEST_CASE("rule 1: connections beyond maxConnections are closed at once", "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.maxConnections = 2;
    Served served(policy);
    const int a = connectTo(served.port);
    const int b = connectTo(served.port);
    std::this_thread::sleep_for(100ms);
    const int c = connectTo(served.port);
    CHECK(closedWithin(c, 2000ms));
    CHECK_FALSE(closedWithin(a, 100ms));
    // The two still serve.
    REQUIRE(sendAll(a, get(served.port, "/hello")));
    Reader reader(a);
    const std::optional<Response> response = reader.next();
    REQUIRE(response);
    CHECK(response->body == "hello");
    ::close(a);
    ::close(b);
    ::close(c);
    std::this_thread::sleep_for(100ms);
    CHECK(roundTrip(served.port, get(served.port, "/hello")).status == 200);   // room again
}

TEST_CASE("rule 2: the head's size, fields, version, method, target and framing", "[web][server][rules]")
{
    Served served(serving());
    const int port = served.port;
    const std::string host = "Host: " + hostOf(port) + "\r\n";
    const auto status = [&](const std::string& request) { return roundTrip(port, request).status; };
    // Size and fields: 16384 bytes and 64 fields at most.
    const std::string base = "GET /hello HTTP/1.1\r\n" + host;
    const std::string filler = "X-Pad: ";
    const std::size_t fits = 16384 - base.size() - filler.size() - 4;
    CHECK(status(base + filler + std::string(fits, 'a') + "\r\n\r\n") == 200);
    CHECK(status(base + filler + std::string(fits + 1, 'a') + "\r\n\r\n") == 431);
    CHECK(status("GET /hello HTTP/1.1\r\n" + host + std::string(20000, 'x')) == 431);   // a head that never ends
    std::string fields;
    for (int i = 0; i < 63; ++i)
        fields += "X-" + std::to_string(i) + ": 1\r\n";
    CHECK(status(base + fields + "\r\n") == 200);   // 64 with Host
    CHECK(status(base + fields + "X-More: 1\r\n\r\n") == 431);
    // Version and method.
    CHECK(status("GET /hello HTTP/1.0\r\n" + host + "\r\n") == 200);
    CHECK(status("GET /hello HTTP/2.0\r\n" + host + "\r\n") == 505);
    CHECK(status("GET /hello HTTP/0.9\r\n" + host + "\r\n") == 505);
    CHECK(status("GET /hello HTTP/1.2\r\n" + host + "\r\n") == 400);   // no version llhttp knows
    CHECK(status("TRACE /hello HTTP/1.1\r\n" + host + "\r\n") == 501);
    CHECK(status("CONNECT 127.0.0.1:1 HTTP/1.1\r\n" + host + "\r\n") == 501);
    CHECK(status("PROPFIND /hello HTTP/1.1\r\n" + host + "\r\n") == 501);
    CHECK(status("FROB /hello HTTP/1.1\r\n" + host + "\r\n") == 501);
    CHECK(status("get /hello HTTP/1.1\r\n" + host + "\r\n") == 501);
    // The target.
    for (const char* target : {"http://127.0.0.1/hello", "*", "hello", "/hello/", "//hello", "/./hello", "/a/../hello",
                               "/%68ello", "/hel%6Co", "/hello#x", "/hello?a#b", "/hel;lo", "/hello?a=\x7f"})
    {
        INFO(target);
        CHECK(status(std::string("GET ") + target + " HTTP/1.1\r\n" + host + "\r\n") == 400);
    }
    CHECK(status("GET /hello?a=1&b=%20!$'()*+,;=:@/? HTTP/1.1\r\n" + host + "\r\n") == 200);
    // Framing.
    CHECK(status("POST /table/1 HTTP/1.1\r\n" + host + "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == 501);
    CHECK(status("POST /table/1 HTTP/1.1\r\n" + host +
                 "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == 400);
    CHECK(status("GET /hello HTTP/1.1\r\n" + host + "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n") == 400);
    CHECK(status("GET /hello HTTP/1.1\r\n" + host + "Upgrade: h2c\r\n\r\n") == 400);
    CHECK(status("POST /table/1 HTTP/1.1\r\n" + host + "Content-Length: 3\r\nExpect: 100-continue\r\n\r\n") == 417);
    CHECK(status("POST /table/1 HTTP/1.1\r\n" + host + "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx") == 400);
    CHECK(status("POST /table/1 HTTP/1.1\r\n" + host + "Content-Length: -1\r\n\r\n") == 400);
    CHECK(status("GET /hello HTTP/1.1\n" + std::string("Host: ") + hostOf(port) + "\n\n") == 400);   // bare LF
    CHECK(status("GET /hello HTTP/1.1\r\n" + host + "X: a\rb\r\n\r\n") == 400);                   // a stray CR
    CHECK(status("GET /hello HTTP/1.1\r\nHost : " + hostOf(port) + "\r\n\r\n") == 400);           // space before ':'
    CHECK(status("GET /hello HTTP/1.1\r\n" + host + " folded: x\r\n\r\n") == 400);
    CHECK(status("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03") == 501);   // a TLS ClientHello
    CHECK(status("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") == 501);              // HTTP/2's preface
    // Every one closed the connection after it.
    const int fd = connectTo(port);
    REQUIRE(sendAll(fd, "GET /hello HTTP/1.1\r\n" + host + "Upgrade: x\r\n\r\n" + get(port, "/hello")));
    Reader reader(fd);
    CHECK(reader.next()->status == 400);
    CHECK_FALSE(reader.next());
    ::close(fd);
}

TEST_CASE("rule 2: a whole request within requestTimeoutMs, or 408", "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.requestTimeoutMs = 300;
    Served served(policy);
    const std::string host = "Host: " + hostOf(served.port) + "\r\n";
    // A head that stops.
    int fd = connectTo(served.port);
    REQUIRE(sendAll(fd, "GET /hello HTTP/1.1\r\n" + host));
    Reader reader(fd);
    auto started = std::chrono::steady_clock::now();
    std::optional<Response> response = reader.next();
    REQUIRE(response);
    CHECK(response->status == 408);
    CHECK(response->header("connection") == "close");
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    ::close(fd);
    // A body that stops.
    fd = connectTo(served.port);
    REQUIRE(sendAll(fd, "POST /table/1 HTTP/1.1\r\n" + host + "Content-Length: 10\r\nContent-Type: a/b\r\n\r\nabc"));
    Reader slow(fd);
    response = slow.next();
    REQUIRE(response);
    CHECK(response->status == 408);
    ::close(fd);
    // A connection with nothing on it is closed, unanswered.
    fd = connectTo(served.port);
    CHECK(closedWithin(fd, 2000ms));
    ::close(fd);
    // A request a byte at a time, each byte well within the time: the whole of it counts.
    fd = connectTo(served.port);
    const std::string request = get(served.port, "/hello");
    for (char c : request)
    {
        REQUIRE(sendAll(fd, std::string_view(&c, 1)));
        std::this_thread::sleep_for(1ms);
    }
    Reader bytes(fd);
    response = bytes.next();
    REQUIRE(response);
    CHECK(response->status == 200);
    ::close(fd);
}

TEST_CASE("a request a byte at a time, and two requests in one write, answered in order", "[web][server]")
{
    Served served(serving());
    const std::string request = "PUT /gain?value=0.25 HTTP/1.1\r\nHost: " + hostOf(served.port) +
                                "\r\nContent-Length: 0\r\n\r\n";
    int fd = connectTo(served.port);
    for (char c : request)
        REQUIRE(sendAll(fd, std::string_view(&c, 1)));
    Reader reader(fd);
    std::optional<Response> response = reader.next();
    REQUIRE(response);
    CHECK(response->body == "{\"gain\":0.25}");
    ::close(fd);
    // Two in one write: the second waits for the first's answer, on the same connection.
    fd = connectTo(served.port);
    REQUIRE(sendAll(fd, get(served.port, "/osc/1/detune") + get(served.port, "/osc/2/detune") +
                            get(served.port, "/hello", "Connection: close\r\n")));
    Reader pipelined(fd);
    CHECK(pipelined.next()->body == "osc 1");
    CHECK(pipelined.next()->body == "osc 2");
    response = pipelined.next();
    REQUIRE(response);
    CHECK(response->body == "hello");
    CHECK(response->header("connection") == "close");
    CHECK_FALSE(pipelined.next());
    ::close(fd);
}

TEST_CASE("rule 3: Host is the address and port, localhost on loopback, or a listed name", "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.hosts = {"studio.local"};
    Served served(policy);
    const int port = served.port;
    const auto status = [&](const std::string& headers)
    { return roundTrip(port, "GET /hello HTTP/1.1\r\n" + headers + "\r\n").status; };
    const std::string p = std::to_string(port);
    CHECK(status("Host: 127.0.0.1:" + p + "\r\n") == 200);
    CHECK(status("Host: localhost:" + p + "\r\n") == 200);
    CHECK(status("Host: studio.local:" + p + "\r\n") == 200);
    CHECK(status("") == 403);                                             // none
    CHECK(status("Host: 127.0.0.1:" + p + "\r\nHost: 127.0.0.1:" + p + "\r\n") == 403);   // two
    CHECK(status("Host: evil.example:" + p + "\r\n") == 403);             // a rebound name
    CHECK(status("Host: 0.0.0.0:" + p + "\r\n") == 403);
    CHECK(status("Host: 127.0.0.1\r\n") == 403);                          // the port left out
    CHECK(status("Host: 127.0.0.1:1" + p + "\r\n") == 403);
    CHECK(status("Host: 127.0.0.2:" + p + "\r\n") == 403);                // not the address it arrived on
    CHECK(status("Host: [::1]:" + p + "\r\n") == 403);
    CHECK(status("Host: \r\n") == 403);
    CHECK(status("Host: localhost.:" + p + "\r\n") == 403);
}

TEST_CASE("rule 4: static files, GET and HEAD, from anywhere, with the token or not", "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.token = "abcdefghijklmnopqrstuvwxyz012345";
    policy.server.files = {{"/", "text/html; charset=utf-8", "<!doctype html><p>synth</p>"},
                           {"/app.js", "text/javascript", "let x = 1;"},
                           {"/hello", "text/plain", "the file, not the route"}};
    Served served(policy);
    const int port = served.port;
    Response page = roundTrip(port, get(port, "/"));
    CHECK(page.status == 200);
    CHECK(page.body == "<!doctype html><p>synth</p>");
    CHECK(page.header("content-type") == "text/html; charset=utf-8");
    CHECK(page.header("content-length") == "27");
    CHECK(page.header("x-frame-options") == "DENY");
    CHECK(page.header("content-security-policy") == "frame-ancestors 'none'");
    CHECK(page.header("referrer-policy") == "same-origin");
    CHECK(page.header("x-content-type-options") == "nosniff");
    CHECK(page.header("cross-origin-resource-policy") == "same-origin");
    CHECK_FALSE(page.has("cache-control"));
    // From another site, without the token: the page is the same for everyone.
    page = roundTrip(port, get(port, "/app.js", "Origin: http://evil.example\r\nSec-Fetch-Site: cross-site\r\n"));
    CHECK(page.status == 200);
    CHECK(page.body == "let x = 1;");
    // A file's path wins over a route's for GET and HEAD.
    CHECK(roundTrip(port, get(port, "/hello")).body == "the file, not the route");
    Response head = roundTrip(port, "HEAD /app.js HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n\r\n", true);
    CHECK(head.status == 200);
    CHECK(head.header("content-length") == "10");
    CHECK(head.body.empty());
    // Kept alive, as a page's next request wants.
    const int fd = connectTo(port);
    REQUIRE(sendAll(fd, get(port, "/") + get(port, "/app.js")));
    Reader reader(fd);
    CHECK(reader.next()->status == 200);
    CHECK(reader.next()->body == "let x = 1;");
    ::close(fd);
    // Other methods on a file's path: 405, GET and HEAD allowed; a GET with a body is answered, then closed.
    Response post = roundTrip(port, "POST /app.js HTTP/1.1\r\nHost: " + hostOf(port) + "\r\nContent-Length: 0\r\n\r\n");
    CHECK(post.status == 405);
    CHECK(post.header("allow") == "GET, HEAD");
    Response withBody = roundTrip(port, "GET /app.js HTTP/1.1\r\nHost: " + hostOf(port) + "\r\nContent-Length: 2\r\n\r\nab");
    CHECK(withBody.status == 200);
    CHECK(withBody.header("connection") == "close");
    // A route still needs the token.
    CHECK(roundTrip(port, get(port, "/osc/1/detune")).status == 401);
    // A file needs the Host rule all the same.
    CHECK(roundTrip(port, "GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n").status == 403);
}

TEST_CASE("rule 5: Sec-Fetch-Site, Origin, and a form's content type", "[web][server][rules]")
{
    Served served(serving());
    const int port = served.port;
    const auto status = [&](const std::string& headers, const std::string& method = "GET /hello")
    { return roundTrip(port, method + " HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n" + headers + "\r\n").status; };
    CHECK(status("Sec-Fetch-Site: same-origin\r\n") == 200);
    CHECK(status("Sec-Fetch-Site: none\r\n") == 200);
    CHECK(status("Sec-Fetch-Site: same-site\r\n") == 403);   // another port of the machine
    CHECK(status("Sec-Fetch-Site: cross-site\r\n") == 403);
    CHECK(status("Sec-Fetch-Site: Same-Origin\r\n") == 403);
    CHECK(status("Sec-Fetch-Site: none\r\nSec-Fetch-Site: none\r\n") == 403);
    CHECK(status("Origin: http://" + hostOf(port) + "\r\n") == 200);
    CHECK(status("Origin: http://localhost:" + std::to_string(port) + "\r\n") == 403);   // not the Host it sent
    CHECK(status("Origin: null\r\n") == 403);
    CHECK(status("Origin: http://evil.example\r\n") == 403);
    CHECK(status("Origin: https://" + hostOf(port) + "\r\n") == 403);
    const std::string post = "POST /table/1";
    CHECK(status("Content-Length: 0\r\nContent-Type: application/json\r\n", post) == 201);
    CHECK(status("Content-Length: 0\r\nContent-Type: text/plain;charset=UTF-8\r\n", post) == 415);
    CHECK(status("Content-Length: 0\r\nContent-Type: Application/X-WWW-Form-Urlencoded\r\n", post) == 415);
    CHECK(status("Content-Length: 0\r\nContent-Type: multipart/form-data; boundary=x\r\n", post) == 415);
    CHECK(status("Content-Length: 0\r\nContent-Type: a/b\r\nContent-Type: c/d\r\n", post) == 415);
    // Rule 5 comes before the route: a page elsewhere cannot even learn which paths exist.
    CHECK(status("Origin: http://evil.example\r\n", "GET /no-such-path") == 403);
}

TEST_CASE("rule 6: 404 for a path no route has, 405 with Allow for a method none has", "[web][server][rules]")
{
    Served served(serving());
    const int port = served.port;
    const auto ask = [&](const std::string& method, const std::string& path)
    {
        return roundTrip(port, method + " " + path + " HTTP/1.1\r\nHost: " + hostOf(port) +
                                  "\r\nContent-Length: 0\r\n\r\n");
    };
    CHECK(ask("GET", "/nothing").status == 404);
    CHECK(ask("GET", "/hello/more").status == 404);
    Response response = ask("POST", "/hello");
    CHECK(response.status == 405);
    CHECK(response.header("allow") == "GET, HEAD");
    response = ask("GET", "/osc/3");
    CHECK(response.status == 405);
    CHECK(response.header("allow") == "DELETE");
    response = ask("OPTIONS", "/osc/3/detune");   // a preflight gets no permission
    CHECK(response.status == 405);
    CHECK(response.header("allow") == "GET, HEAD");
    CHECK_FALSE(response.has("access-control-allow-origin"));
    CHECK(ask("DELETE", "/osc/3").status == 204);
    CHECK(roundTrip(port, "HEAD /osc/3/detune HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n\r\n", true).status == 200);
}

TEST_CASE("rule 7: the token, as a bearer, compared whole", "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.token = "Tok-en_0123456789abcdef";
    Served served(policy);
    const int port = served.port;
    const auto ask = [&](const std::string& headers) { return roundTrip(port, get(port, "/hello", headers)); };
    CHECK(ask("Authorization: Bearer Tok-en_0123456789abcdef\r\n").status == 200);
    CHECK(ask("Authorization: bearer Tok-en_0123456789abcdef\r\n").status == 200);
    Response refused = ask("");
    CHECK(refused.status == 401);
    CHECK(refused.header("www-authenticate") == "Bearer");
    for (const char* wrong : {"Bearer Tok-en_0123456789abcde", "Bearer Tok-en_0123456789abcdeff",
                              "Bearer tok-en_0123456789abcdef", "Bearer  Tok-en_0123456789abcdef", "Tok-en_0123456789abcdef",
                              "Basic Tok-en_0123456789abcdef", "Bearer", ""})
    {
        INFO(wrong);
        CHECK(ask(std::string("Authorization: ") + wrong + "\r\n").status == 401);
    }
    CHECK(ask("Authorization: Bearer Tok-en_0123456789abcdef\r\nAuthorization: Bearer Tok-en_0123456789abcdef\r\n")
              .status == 401);
    // In a query it is nothing.
    CHECK(roundTrip(port, get(port, "/hello?token=Tok-en_0123456789abcdef")).status == 401);
    // The rules before it come first: an unknown path is 404 without the token.
    CHECK(roundTrip(port, get(port, "/nothing")).status == 404);
}

TEST_CASE("rule 8: a body over maxRequestBytes is 413 before a byte of it is read", "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.maxRequestBytes = 100;
    Served served(policy);
    const std::string head = "POST /table/9 HTTP/1.1\r\nHost: " + hostOf(served.port) + "\r\nContent-Type: a/b\r\n";
    Response response = roundTrip(served.port, head + "Content-Length: 100\r\n\r\n" + std::string(100, 'z'));
    CHECK(response.status == 201);
    CHECK(response.body == "table 9: 100 bytes [%s]");
    // 101: refused on its Content-Length alone, with the body never sent.
    response = roundTrip(served.port, head + "Content-Length: 101\r\n\r\n");
    CHECK(response.status == 413);
    CHECK(response.header("connection") == "close");
    response = roundTrip(served.port, head + "Content-Length: 99999999999999999999\r\n\r\n");
    CHECK((response.status == 400 || response.status == 413));
    // A body longer than it says: the rest is the next request, which is no request.
    const int fd = connectTo(served.port);
    REQUIRE(sendAll(fd, head + "Content-Length: 2\r\n\r\nabcdef"));
    Reader reader(fd);
    CHECK(reader.next()->status == 201);
    CHECK(reader.next()->status == 501);   // "cdef" is no method
    ::close(fd);
}

TEST_CASE("rule 9: every slot taken is 503 with Retry-After; a request no one answers is 503 too",
          "[web][server][rules]")
{
    Policy policy = serving();
    policy.server.maxPending = 2;
    policy.server.replyTimeoutMs = 300;
    Served served(policy, nullptr);   // no program: nothing is taken
    const int a = connectTo(served.port);
    const int b = connectTo(served.port);
    REQUIRE(sendAll(a, get(served.port, "/hello")));
    REQUIRE(sendAll(b, get(served.port, "/hello")));
    std::this_thread::sleep_for(100ms);
    Response full = roundTrip(served.port, get(served.port, "/hello"));
    CHECK(full.status == 503);
    CHECK(full.header("retry-after") == "1");
    // The two waiting are answered 503 for the program once replyTimeoutMs has passed, and stay open.
    Reader readerA(a);
    const auto started = std::chrono::steady_clock::now();
    std::optional<Response> timedOut = readerA.next();
    REQUIRE(timedOut);
    CHECK(timedOut->status == 503);
    CHECK(timedOut->header("retry-after") == "1");
    CHECK_FALSE(timedOut->has("connection"));
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    Reader readerB(b);
    CHECK(readerB.next()->status == 503);
    // The slots are free again, and the connection carries the next request.
    REQUIRE(sendAll(a, get(served.port, "/hello")));
    void* request = fminus_web_http_accept(served.server, 2000);
    REQUIRE(request != nullptr);
    CHECK(replyText(request, 200, "late but fine") == 0);
    CHECK(readerA.next()->body == "late but fine");
    ::close(a);
    ::close(b);
}

TEST_CASE("a request taken and never answered keeps its slot: the client's 503 at replyTimeoutMs frees nothing",
          "[web][server][rules]")
{
    // The slot is the taker's until it replies, whatever the library has told the client: the program may still be
    // writing into the slot's buffer. So maxPending requests never answered leave every later request a 503 until the
    // program answers one (its reply HTTP_CANCELLED) or the server closes.
    Policy policy = serving();
    policy.server.maxPending = 2;
    policy.server.replyTimeoutMs = 300;
    Served served(policy, nullptr);
    const int a = connectTo(served.port);
    const int b = connectTo(served.port);
    REQUIRE(sendAll(a, get(served.port, "/hello")));
    REQUIRE(sendAll(b, get(served.port, "/hello")));
    void* first = fminus_web_http_accept(served.server, 2000);
    void* second = fminus_web_http_accept(served.server, 2000);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    Reader readerA(a);
    Reader readerB(b);
    CHECK(readerA.next()->status == 503);   // at replyTimeoutMs, for the program
    CHECK(readerB.next()->status == 503);
    // Long after that, both slots are still held: a new request finds none free, now and later.
    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(400ms);
        CHECK(roundTrip(served.port, get(served.port, "/hello")).status == 503);
        CHECK(fminus_web_http_next(served.server) == nullptr);
    }
    // The program's late reply gives the slot back (CANCELLED: its client was answered already), and the next request
    // is taken in it.
    CHECK(replyText(first, 200, "too late") == FMINUS_WEB_CANCELLED);
    REQUIRE(sendAll(a, get(served.port, "/hello")));
    void* third = fminus_web_http_accept(served.server, 2000);
    REQUIRE(third != nullptr);
    CHECK(replyText(third, 200, "in the freed slot") == 0);
    CHECK(readerA.next()->body == "in the freed slot");
    CHECK(replyText(second, 200, "too late") == FMINUS_WEB_CANCELLED);
    ::close(a);
    ::close(b);
}

// ---- responses, keep-alive and the connection's life ----------------------------------------------------------------

TEST_CASE("every response's headers: the program's, the library's, none of Server or Access-Control",
          "[web][server]")
{
    Served served(serving());
    const int port = served.port;
    const auto common = [](const Response& r)
    {
        CHECK(r.header("x-content-type-options") == "nosniff");
        CHECK(r.header("cross-origin-resource-policy") == "same-origin");
        CHECK_FALSE(r.has("server"));
        for (const auto& [name, value] : r.headers)
            CHECK_FALSE(name.starts_with("access-control-"));
        CHECK(r.head.starts_with("HTTP/1.1 "));
    };
    Response program = roundTrip(port, get(port, "/osc/2/detune", "Origin: http://" + hostOf(port) + "\r\n"));
    common(program);
    CHECK(program.status == 200);
    CHECK(program.head.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(program.header("cache-control") == "no-store");
    CHECK(program.header("content-type") == "text/plain");
    CHECK(program.header("content-length") == "5");
    CHECK_FALSE(program.has("x-frame-options"));
    Response noContent = roundTrip(port, "DELETE /osc/1 HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n\r\n", true);
    common(noContent);
    CHECK(noContent.status == 204);
    CHECK_FALSE(noContent.has("content-length"));
    CHECK_FALSE(noContent.has("content-type"));
    for (const std::string& request : {get(port, "/nothing"), get(port, "/hello", "Origin: null\r\n"),
                                       std::string("GET /hello HTTP/1.1\r\n\r\n")})
    {
        Response refused = roundTrip(port, request);
        common(refused);
        CHECK(refused.header("cache-control") == "no-store");
        CHECK(refused.header("content-length") == "0");
        CHECK(refused.header("connection") == "close");
        CHECK_FALSE(refused.has("content-type"));
    }
    // A JSON reply, and the default types.
    Response json = roundTrip(port, "PUT /gain?value=1.5 HTTP/1.1\r\nHost: " + hostOf(port) + "\r\nContent-Length: 0\r\n\r\n");
    CHECK(json.header("content-type") == "application/json");
    CHECK(json.body == "{\"gain\":1.5}");
    CHECK(roundTrip(port, get(port, "/hello")).header("content-type") == "text/plain; charset=utf-8");
}

TEST_CASE("keep-alive: requests one after another on a connection; Connection: close, HTTP/1.0, the idle limit",
          "[web][server]")
{
    Policy policy = serving();
    policy.server.idleTimeoutMs = 300;
    Served served(policy);
    const int port = served.port;
    int fd = connectTo(port);
    Reader reader(fd);
    for (int i = 0; i < 20; ++i)
    {
        REQUIRE(sendAll(fd, get(port, "/osc/" + std::to_string(i) + "/detune")));
        const std::optional<Response> response = reader.next();
        REQUIRE(response);
        CHECK(response->body == "osc " + std::to_string(i));
        CHECK_FALSE(response->has("connection"));
    }
    // Idle past the limit: closed.
    CHECK(closedWithin(fd, 3000ms));
    ::close(fd);
    fd = connectTo(port);
    REQUIRE(sendAll(fd, get(port, "/hello", "Connection: close\r\n")));
    Reader closing(fd);
    CHECK(closing.next()->header("connection") == "close");
    CHECK(closedWithin(fd, 1000ms));
    ::close(fd);
    fd = connectTo(port);
    REQUIRE(sendAll(fd, "GET /hello HTTP/1.0\r\nHost: " + hostOf(port) + "\r\nConnection: keep-alive\r\n\r\n"));
    Reader old(fd);
    const std::optional<Response> response = old.next();
    REQUIRE(response);
    CHECK(response->body == "hello");
    CHECK(response->header("connection") == "close");
    CHECK(closedWithin(fd, 1000ms));
    ::close(fd);
    // The library's client keeps its connection too: many requests, one connection's worth of accepts.
    auto client = clientFor(port);
    for (int i = 0; i < 10; ++i)
    {
        void* request = clientGet(*client, port, "/hello");
        CHECK(fminus_web_http_wait(request, 5000) == 200);
        fminus_web_http_close(request);
    }
}

TEST_CASE("connections dropped mid-request and mid-reply; a reply after its client left is cancelled",
          "[web][server]")
{
    Policy policy = serving();
    policy.server.maxReplyBytes = 4 << 20;
    policy.server.maxPending = 2;
    const std::string big(4 << 20, 'b');
    Served served(policy, [&](void* r) {
        if (is(r, "GET /big"))
            replyBytes(r, 200, big, "application/octet-stream");
        else
            answer(r);
    });
    const int port = served.port;
    // Mid-request: half a head, then gone; half a body, then gone.
    for (const std::string& part : {std::string("GET /hel"), "POST /table/1 HTTP/1.1\r\nHost: " + hostOf(port) +
                                                                  "\r\nContent-Length: 50\r\n\r\nabc"})
    {
        const int fd = connectTo(port);
        REQUIRE(sendAll(fd, part));
        ::close(fd);
    }
    // Mid-reply: a reply of 4 MiB the client stops reading, then resets. No SIGPIPE (this process would die of it).
    for (int i = 0; i < 3; ++i)
    {
        const int fd = connectTo(port);
        int small = 4096;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
        REQUIRE(sendAll(fd, get(port, "/big")));
        char some[1000];
        CHECK(::recv(fd, some, sizeof some, 0) > 0);
        std::this_thread::sleep_for(50ms);
        linger reset{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof reset);
        ::close(fd);
    }
    // And one read whole.
    Response whole = roundTrip(port, get(port, "/big"));
    CHECK(whole.status == 200);
    CHECK(whole.body.size() == big.size());
    CHECK(roundTrip(port, get(port, "/hello")).body == "hello");

    // A claimed request whose client leaves before the reply: the reply says FMINUS_WEB_CANCELLED.
    served.program.reset();
    const int fd = connectTo(port);
    REQUIRE(sendAll(fd, get(port, "/hello")));
    void* request = fminus_web_http_accept(served.server, 5000);
    REQUIRE(request != nullptr);
    ::close(fd);
    std::this_thread::sleep_for(200ms);
    CHECK(is(request, "GET /hello") == 1);   // still the caller's to read
    CHECK(fminus_web_http_reply(request, 200) == FMINUS_WEB_CANCELLED);
    CHECK(fminus_web_http_reply(request, 200) == FMINUS_WEB_INVALID);   // dead now
    // And a published one whose client leaves is never taken.
    const int gone = connectTo(port);
    REQUIRE(sendAll(gone, get(port, "/hello")));
    std::this_thread::sleep_for(100ms);
    ::close(gone);
    std::this_thread::sleep_for(200ms);
    CHECK(fminus_web_http_next(served.server) == nullptr);
}

TEST_CASE("a client that sends its request and closes: the reply meets EPIPE, never SIGPIPE", "[web][server]")
{
    // The client's FIN puts the server's end in CLOSE_WAIT; the first bytes of the reply then reach a closed socket,
    // whose RST makes the next send fail with EPIPE, the one error that raises SIGPIPE unless the send or the socket
    // says not to (Sockets.h). A send without that would end this process.
    Policy policy = serving();
    const std::string big(4u << 20, 'b');
    policy.server.files = {{"/big.bin", "application/octet-stream", big}};
    Served served(policy);
    for (int i = 0; i < 8; ++i)
    {
        const int fd = connectTo(served.port);
        REQUIRE(sendAll(fd, get(served.port, "/big.bin")));
        ::close(fd);
    }
    std::this_thread::sleep_for(200ms);
    Response whole = roundTrip(served.port, get(served.port, "/big.bin"));
    CHECK(whole.status == 200);
    CHECK(whole.body.size() == big.size());
    CHECK(roundTrip(served.port, get(served.port, "/hello")).body == "hello");
}

TEST_CASE("a reply over maxReplyBytes is refused, sends nothing, and the program may answer again",
          "[web][server]")
{
    Policy policy = serving();
    policy.server.maxReplyBytes = 16;
    Served served(policy, nullptr);
    auto client = clientFor(served.port);
    void* made = clientGet(*client, served.port, "/hello");
    void* request = fminus_web_http_accept(served.server, 5000);
    REQUIRE(request != nullptr);
    CHECK(replyBytes(request, 200, std::string(17, 'x')) == FMINUS_WEB_LIMIT);
    CHECK(replyText(request, 200, "%g %g %g", {1.0 / 3, 2.0 / 3, 1.0}) == FMINUS_WEB_LIMIT);
    CHECK(replyText(request, 99, "x") == FMINUS_WEB_INVALID);
    CHECK(replyText(request, 600, "x") == FMINUS_WEB_INVALID);
    CHECK(replyText(request, 204, "x") == FMINUS_WEB_INVALID);
    CHECK(replyBytes(request, 304, "x") == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply_bytes(request, 200, nullptr, 1, nullptr, 0) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_reply_bytes(request, 200, "x", -1, nullptr, 0) == FMINUS_WEB_INVALID);
    CHECK(replyBytes(request, 200, "x", "text/plain\r\nX-Injected: 1") == FMINUS_WEB_INVALID);
    CHECK(replyBytes(request, 200, "x", std::string(256, 'a')) == FMINUS_WEB_INVALID);
    CHECK(fminus_web_http_status(made) == FMINUS_WEB_PENDING);   // nothing was sent
    CHECK(replyBytes(request, 200, std::string(16, 'y'), "a/b") == 0);
    CHECK(fminus_web_http_wait(made, 5000) == 200);
    CHECK(bodyOf(made) == std::string(16, 'y'));
    fminus_web_http_close(made);
}

TEST_CASE("many requests from several clients, taken by several threads, each once", "[web][server]")
{
    Policy policy = serving();
    policy.server.maxConnections = 64;
    policy.server.maxPending = 8;
    policy.server.replyTimeoutMs = 10000;
    Served served(policy, nullptr);
    std::atomic<int> answered{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> takers;
    for (int t = 0; t < 4; ++t)
        takers.emplace_back([&, t] {
            while (!stop.load())
            {
                void* r = t % 2 == 0 ? fminus_web_http_next(served.server) : fminus_web_http_accept(served.server, 5);
                if (r == nullptr)
                    continue;
                if (is(r, "GET /osc/%d/detune"))
                {
                    replyText(r, 200, "%d", {double(fminus_web_http_arg(r, 0))});
                    answered.fetch_add(1);
                }
            }
        });
    // Two clients (contexts), each with 16 requests in flight at a time (its policy's limit), 8 slots: some are
    // answered 503 when every slot is taken, every other one by a taker, with its own number.
    constexpr int kBatches = 8;
    std::atomic<int> ok{0};
    std::atomic<int> full{0};
    std::atomic<int> wrong{0};
    std::vector<std::unique_ptr<Context>> contexts;   // made here: Catch2's macros are this thread's alone
    contexts.push_back(clientFor(served.port));
    contexts.push_back(clientFor(served.port));
    std::vector<std::thread> clients;
    for (int k = 0; k < 2; ++k)
        clients.emplace_back([&, k] {
            Context* client = contexts[static_cast<std::size_t>(k)].get();
            for (int batch = 0; batch < kBatches; ++batch)
            {
                std::vector<std::pair<int, void*>> requests;
                for (int i = 0; i < 16; ++i)
                {
                    const int n = (k * kBatches + batch) * 16 + i;
                    requests.push_back({n, clientGet(*client, served.port, "/osc/" + std::to_string(n) + "/detune")});
                }
                for (const auto& [n, request] : requests)
                {
                    const std::int32_t status = fminus_web_http_wait(request, 20000);
                    if (status == 200 && bodyOf(request) == std::to_string(n))
                        ok.fetch_add(1);
                    else if (status == 503)
                        full.fetch_add(1);
                    else
                        wrong.fetch_add(1);
                    fminus_web_http_close(request);
                }
            }
        });
    for (std::thread& t : clients)
        t.join();
    stop.store(true);
    for (std::thread& t : takers)
        t.join();
    INFO("ok " << ok.load() << ", 503 " << full.load());
    CHECK(wrong.load() == 0);
    CHECK(ok.load() + full.load() == 2 * kBatches * 16);
    CHECK(answered.load() == ok.load());
    CHECK(ok.load() > 0);
}

// ---- the server's life --------------------------------------------------------------------------------------------

TEST_CASE("a context listens once: a second listen is the same server", "[web][server]")
{
    auto context = makeContext(serving());
    void* first = listen(*context, {"GET /a"});
    void* second = listen(*context, {"GET /b"});
    CHECK(first == second);
    const int port = fminus_web_http_port(first);
    REQUIRE(port > 0);
    // The routes are the first's.
    CHECK(roundTrip(port, get(port, "/b")).status == 404);
}

TEST_CASE("a failed bind is FMINUS_WEB_TRANSPORT: the port taken by the test itself", "[web][server]")
{
    const int taken = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(taken >= 0);
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(taken, reinterpret_cast<const sockaddr*>(&in), sizeof in) == 0);
    REQUIRE(::listen(taken, 1) == 0);
    socklen_t length = sizeof in;
    REQUIRE(::getsockname(taken, reinterpret_cast<sockaddr*>(&in), &length) == 0);
    const int port = ntohs(in.sin_port);
    auto context = makeContext(serving("127.0.0.1:" + std::to_string(port)));
    void* server = listen(*context, {"GET /a"});
    CHECK(fminus_web_http_port(server) == FMINUS_WEB_TRANSPORT);
    CHECK(fminus_web_http_next(server) == nullptr);
    CHECK(fminus_web_http_accept(server, -1) == nullptr);   // at once
    CHECK(listen(*context, {}) == server);
    fminus_web_http_close(server);
    CHECK(fminus_web_http_port(server) == FMINUS_WEB_CANCELLED);
    ::close(taken);
    // localhost on a port whose IPv4 half is taken: not listening on the other half alone.
    const int again = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(::bind(again, reinterpret_cast<const sockaddr*>(&in), sizeof in) == 0);
    REQUIRE(::listen(again, 1) == 0);
    auto both = makeContext(serving("localhost:" + std::to_string(port)));
    CHECK(fminus_web_http_port(listen(*both, {})) == FMINUS_WEB_TRANSPORT);
    ::close(again);
}

TEST_CASE("localhost listens on 127.0.0.1 and ::1, one port, where the machine has IPv6", "[web][server]")
{
    Served served(serving("localhost:0"));
    const int port = served.port;
    CHECK(roundTrip(port, "GET /hello HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n").status ==
          200);
    if (!machineHasIpv6())
    {
        WARN("no IPv6 loopback here: localhost listened on 127.0.0.1 alone, and ::1 alone cannot listen");
        auto v6 = makeContext(serving("[::1]:0"));
        CHECK(fminus_web_http_port(listen(*v6, kRoutes)) == FMINUS_WEB_TRANSPORT);
        return;
    }
    for (const std::string& host : {"localhost:" + std::to_string(port), "[::1]:" + std::to_string(port),
                                    "127.0.0.1:" + std::to_string(port)})
    {
        const int fd = connectTo(port, 6);
        REQUIRE(sendAll(fd, "GET /hello HTTP/1.1\r\nHost: " + host + "\r\n\r\n"));
        Reader reader(fd);
        const std::optional<Response> response = reader.next();
        REQUIRE(response);
        INFO(host);
        CHECK(response->status == (host.starts_with("127.") ? 403 : 200));   // the address it arrived on is ::1
        ::close(fd);
    }
    // [::1] alone.
    Served v6(serving("[::1]:0"));
    const int fd = connectTo(v6.port, 6);
    REQUIRE(sendAll(fd, "GET /hello HTTP/1.1\r\nHost: [::1]:" + std::to_string(v6.port) + "\r\n\r\n"));
    Reader reader(fd);
    CHECK(reader.next()->body == "hello");
    ::close(fd);
}

TEST_CASE("close, stop and destruction with connections open, requests out and a thread in accept",
          "[web][server][lifetime]")
{
    SECTION("http_close")
    {
        auto context = makeContext(serving());
        void* server = listen(*context, kRoutes);
        const int port = fminus_web_http_port(server);
        REQUIRE(port > 0);
        const int idle = connectTo(port);
        const int waiting = connectTo(port);
        REQUIRE(sendAll(waiting, get(port, "/hello") + get(port, "/hello")));
        void* held = fminus_web_http_accept(server, 5000);
        REQUIRE(held != nullptr);
        std::atomic<bool> returned{false};
        void* got = reinterpret_cast<void*>(1);
        std::thread acceptor([&] {
            got = fminus_web_http_accept(server, -1);
            returned.store(true);
        });
        std::this_thread::sleep_for(100ms);
        CHECK_FALSE(returned.load());
        fminus_web_http_close(server);
        acceptor.join();
        CHECK(got == nullptr);
        CHECK(fminus_web_http_port(server) == FMINUS_WEB_CANCELLED);
        CHECK(closedWithin(idle, 1000ms));
        CHECK(closedWithin(waiting, 1000ms));
        CHECK(is(held, "GET /hello") == 1);
        CHECK(fminus_web_http_reply(held, 200) == FMINUS_WEB_CANCELLED);
        CHECK(fminus_web_http_next(server) == nullptr);
        CHECK(fminus_web_http_accept(server, -1) == nullptr);
        fminus_web_http_close(server);   // again: nothing
        CHECK(listen(*context, kRoutes) == server);
        ::close(idle);
        ::close(waiting);
        // The port is free again.
        Served again(serving("127.0.0.1:" + std::to_string(port)));
        CHECK(again.port == port);
    }
    SECTION("Context::stop")
    {
        auto context = makeContext(serving());
        void* server = listen(*context, kRoutes);
        const int port = fminus_web_http_port(server);
        const int open = connectTo(port);
        REQUIRE(sendAll(open, get(port, "/hello")));
        void* got = reinterpret_cast<void*>(1);
        std::thread acceptor([&] {
            std::this_thread::sleep_for(50ms);
            void* first = fminus_web_http_accept(server, -1);   // the published request
            got = fminus_web_http_accept(server, -1);           // then nothing until the stop
            if (first != nullptr)
                fminus_web_http_reply(first, 200);
        });
        std::this_thread::sleep_for(300ms);
        context->stop();
        acceptor.join();
        CHECK(got == nullptr);
        CHECK(fminus_web_http_port(server) == FMINUS_WEB_CANCELLED);
        CHECK(closedWithin(open, 1000ms));
        ::close(open);
        context->stop();
        // A context stopped before it listens never listens.
        auto stopped = makeContext(serving());
        stopped->stop();
        void* never = listen(*stopped, kRoutes);
        CHECK(fminus_web_http_port(never) == FMINUS_WEB_CANCELLED);
        CHECK(fminus_web_http_accept(never, -1) == nullptr);
    }
    SECTION("~Context")
    {
        auto context = makeContext(serving());
        void* server = listen(*context, kRoutes);
        const int port = fminus_web_http_port(server);
        const int open = connectTo(port);
        const int waiting = connectTo(port);
        REQUIRE(sendAll(waiting, get(port, "/hello") + get(port, "/osc/1/detune")));
        std::this_thread::sleep_for(100ms);
        void* held = fminus_web_http_next(server);
        REQUIRE(held != nullptr);
        std::vector<void*> got(3, reinterpret_cast<void*>(1));
        std::vector<std::thread> acceptors;
        for (int i = 0; i < 3; ++i)
            acceptors.emplace_back([&, i] {
                std::this_thread::sleep_for(20ms);
                got[static_cast<std::size_t>(i)] = fminus_web_http_accept(server, -1);
            });
        std::this_thread::sleep_for(200ms);
        context.reset();   // the threads in accept are woken, waited for, then the server is freed
        for (std::thread& t : acceptors)
            t.join();
        CHECK(std::count(got.begin(), got.end(), nullptr) == 3);
        CHECK(closedWithin(open, 1000ms));
        CHECK(closedWithin(waiting, 1000ms));
        ::close(open);
        ::close(waiting);
        CHECK_FALSE(fminus::web::ioThreadRunning());
    }
}

// ---- the audio thread's promise -----------------------------------------------------------------------------------

TEST_CASE("taking, reading and answering a request allocate nothing and make no system call", "[web][server]")
{
    Policy policy = serving();
    policy.server.replyTimeoutMs = 20000;
    Served served(policy, nullptr);
    const int port = served.port;
    const std::string post = "POST /table/12?v=0.75&n=-3 HTTP/1.1\r\nHost: " + hostOf(port) +
                             "\r\nContent-Type: a/b\r\nContent-Length: 4\r\n\r\nwxyz";
    // What the audio thread does with one request: the reads, summed, and the reply of `kind`.
    const auto handle = [](void* server, int kind, double& sum) -> bool
    {
        void* r = fminus_web_http_next(server);
        if (r == nullptr)
            return false;
        sum = fminus_web_http_port(server);
        sum += is(r, "GET /hello") + 10.0 * is(r, "POST /table/%d") + fminus_web_http_arg(r, 0);
        sum += static_cast<double>(queryFloat(r, "v", 0.0f)) + queryInt(r, "n", 0) + fminus_web_http_size(r);
        sum += static_cast<const char*>(fminus_web_http_body(r))[0];
        static const double values[] = {0.1, 2.0, -7.0};
        static const char* const texts[] = {"warm", "pad"};
        static const std::int32_t lengths[] = {4, 3};
        std::int32_t replied = 0;
        if (kind == 0)
            replied = fminus_web_http_reply(r, 204);
        else if (kind == 1)
            replied = fminus_web_http_reply_bytes(r, 200, "bytes", 5, "a/b", 3);
        else if (kind == 2)
            replied = fminus_web_http_reply_text(r, 200, "", 0, "%g %.9g %d %%", 13, values, 3);
        else
            replied = fminus_web_http_reply_text_s(r, 200, "", 0, "%s %g %s", 8, values, 1, texts, lengths, 2);
        return replied == 0;
    };
    const double expected = port + 10.0 + 12.0 + 0.75 - 3.0 + 4.0 + 'w';
    std::vector<int> clients;
    const auto send = [&]
    {
        const int fd = connectTo(port);
        REQUIRE(sendAll(fd, post));
        clients.push_back(fd);
    };

    // In this process: no allocation, counted on this thread, for each kind of reply.
    if (fminus::test::HeapCalls::counted())
    {
        for (int kind = 0; kind < 4; ++kind)
        {
            send();
            bool done = false;
            for (int attempt = 0; attempt < 500 && !done; ++attempt)
            {
                double sum = 0;
                long calls = 0;
                {
                    const fminus::test::HeapCalls counting;
                    done = handle(served.server, kind, sum);
                    calls = counting.count();
                }
                if (done)
                {
                    INFO("kind " << kind);
                    CHECK(calls == 0);
                    CHECK(sum == expected);
                }
                else
                    std::this_thread::sleep_for(2ms);
            }
            CHECK(done);
        }
        Reader replies(clients[2]);
        CHECK(replies.next()->body == "0.1 2 -7 %");
        Reader strings(clients[3]);
        CHECK(strings.next()->body == "warm 0.1 pad");
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");

    // In a child under seccomp's strict mode, where any system call but read, write and exit kills it: the same, for
    // each kind, on a request published before the fork (the child has no I/O thread). The parent's copy of the request
    // is untouched, and is answered after. Linux only.
    send();
#if defined(__linux__)
    for (int kind = 0; kind < 4; ++kind)
    {
        bool checked = false;
        for (int attempt = 0; attempt < 200 && !checked; ++attempt)
        {
            int fds[2];
            REQUIRE(::pipe(fds) == 0);
            const pid_t child = ::fork();
            REQUIRE(child >= 0);
            if (child == 0)
            {
                ::close(fds[0]);
                if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
                    ::syscall(SYS_exit, 2);
                double result[2] = {0, 0};
                result[0] = handle(served.server, kind, result[1]) ? 1 : 0;
                const ssize_t written = ::write(fds[1], result, sizeof result);
                ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
            }
            ::close(fds[1]);
            int status = 0;
            ::waitpid(child, &status, 0);
            double result[2] = {0, 0};
            const ssize_t got = ::read(fds[0], result, sizeof result);
            ::close(fds[0]);
            INFO("status " << status);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
            {
                WARN("seccomp's strict mode is not available here: the system calls are not checked");
                checked = true;
                kind = 4;
                break;
            }
            REQUIRE(WIFEXITED(status));
            REQUIRE(WEXITSTATUS(status) == 0);
            REQUIRE(got == static_cast<ssize_t>(sizeof result));
            if (result[0] == 1)
            {
                CHECK(result[1] == expected);
                checked = true;
            }
            else
                std::this_thread::sleep_for(5ms);   // not published yet
        }
        CHECK(checked);
    }
#else
    WARN("seccomp is Linux's: the system calls are not checked here");
#endif
    void* last = fminus_web_http_accept(served.server, 5000);
    REQUIRE(last != nullptr);
    CHECK(fminus_web_http_reply(last, 204) == 0);
    for (int fd : clients)
        ::close(fd);
}

TEST_CASE("taking by route set and reading a request's text allocate nothing and make no system call",
          "[web][server]")
{
    // The same proof as above, for the functions a serve statement and a string's reads will call: a take by route
    // set, the route, a place's text, the path, a header, a query's text, and a reply.
    Policy policy = serving();
    policy.server.replyTimeoutMs = 20000;
    Served served(policy, nullptr, {"GET /hello", "PUT /presets/%s/%d"});
    const int port = served.port;
    const std::string put = "PUT /presets/warm.pad/0042?name=deep&v=0.5 HTTP/1.1\r\nHost: " + hostOf(port) +
                            "\r\nX-Preset:  lead \r\nContent-Length: 0\r\n\r\n";
    const auto handle = [](void* server, double& sum) -> bool
    {
        static const std::uint64_t loader[] = {2};   // route 1
        static const std::uint64_t audio[] = {1};    // route 0
        if (fminus_web_http_next_in(server, audio, 1) != nullptr)
            return false;   // never: no request for route 0 is sent
        void* r = fminus_web_http_next_in(server, loader, 1);
        if (r == nullptr)
            return false;
        std::int32_t length = 0;
        sum = fminus_web_http_route(r) * 1000.0;
        const char* text = fminus_web_http_text(r, 0, &length);
        sum += length + text[0];
        text = fminus_web_http_text(r, 1, &length);
        sum += length + text[0] + fminus_web_http_arg(r, 1);
        text = fminus_web_http_path(r, &length);
        sum += length + text[length - 1];
        text = fminus_web_http_header(r, "x-PRESET", 8, &length);
        sum += length + text[0];
        text = fminus_web_http_header(r, "x-none", 6, &length);
        sum += length;
        text = fminus_web_http_query_text(r, "name", 4, &length);
        sum += length + text[0];
        sum += fminus_web_http_query_text(r, "none", 4, &length) == nullptr ? 1 : 100;
        return fminus_web_http_reply(r, 204) == 0;
    };
    // route 1; "warm.pad" 8 and 'w'; "0042" 4, '0' and 42; "/presets/warm.pad/0042" 22 and '2'; "lead" 4 and 'l';
    // nothing; "deep" 4 and 'd'; none.
    const double expected = 1000.0 + 8 + 'w' + 4 + '0' + 42 + 22 + '2' + 4 + 'l' + 0 + 4 + 'd' + 1;
    std::vector<int> clients;
    const auto send = [&]
    {
        const int fd = connectTo(port);
        REQUIRE(sendAll(fd, put));
        clients.push_back(fd);
    };

    if (fminus::test::HeapCalls::counted())
    {
        send();
        bool done = false;
        for (int attempt = 0; attempt < 500 && !done; ++attempt)
        {
            double sum = 0;
            long calls = 0;
            {
                const fminus::test::HeapCalls counting;
                done = handle(served.server, sum);
                calls = counting.count();
            }
            if (done)
            {
                CHECK(calls == 0);
                CHECK(sum == expected);
            }
            else
                std::this_thread::sleep_for(2ms);
        }
        CHECK(done);
        Reader replies(clients[0]);
        CHECK(replies.next()->status == 204);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");

    send();
#if defined(__linux__)
    bool checked = false;
    for (int attempt = 0; attempt < 200 && !checked; ++attempt)
    {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            ::close(fds[0]);
            if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
                ::syscall(SYS_exit, 2);
            double result[2] = {0, 0};
            result[0] = handle(served.server, result[1]) ? 1 : 0;
            const ssize_t written = ::write(fds[1], result, sizeof result);
            ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
        }
        ::close(fds[1]);
        int status = 0;
        ::waitpid(child, &status, 0);
        double result[2] = {0, 0};
        const ssize_t got = ::read(fds[0], result, sizeof result);
        ::close(fds[0]);
        INFO("status " << status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        {
            WARN("seccomp's strict mode is not available here: the system calls are not checked");
            break;
        }
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
        REQUIRE(got == static_cast<ssize_t>(sizeof result));
        if (result[0] == 1)
        {
            CHECK(result[1] == expected);
            checked = true;
        }
        else
            std::this_thread::sleep_for(5ms);   // not published yet
    }
    CHECK(checked);
#else
    WARN("seccomp is Linux's: the system calls are not checked here");
#endif
    void* last = fminus_web_http_accept_in(served.server, std::array<std::uint64_t, 1>{2}.data(), 1, 5000);
    REQUIRE(last != nullptr);
    CHECK(fminus_web_http_reply(last, 204) == 0);
    for (int fd : clients)
        ::close(fd);
}

// ---- a reply written in place, and URLs with strings ------------------------------------------------------------

namespace
{
    // The oldest request the server has published, taken, waiting up to five seconds for one.
    void* takeOne(void* server)
    {
        for (int attempt = 0; attempt < 2500; ++attempt)
        {
            if (void* r = fminus_web_http_next(server))
                return r;
            std::this_thread::sleep_for(2ms);
        }
        return nullptr;
    }

    std::int32_t replyWritten(void* request, std::int32_t status, std::int32_t bytes, std::string_view type = {})
    {
        return fminus_web_http_reply_written(request, status, bytes, type.data(),
                                             static_cast<std::int32_t>(type.size()));
    }

#if FMINUS_HAS_JSON
    // A module, its shape, and the literal {"osc": <module>, "level": <float>} a reply writes.
    struct Voice
    {
        float detune;
        std::int32_t notes[3];
        bool on;
        char name[8];
    };

    std::vector<std::int32_t> voiceLiteral()
    {
        using fminus::test::shapes::Table;
        namespace k = fminus::test::shapes;
        Table t;
        t.literal(R"({"osc":)");
        t.value(0, Table::pointer(t.module({
                       {"detune", offsetof(Voice, detune), Table::scalar(k::Float)},
                       {"notes", offsetof(Voice, notes), Table::array(3, 4, Table::scalar(k::Int))},
                       {"on", offsetof(Voice, on), Table::scalar(k::Bool)},
                       {"name", offsetof(Voice, name), Table::chars(8)},
                   })));
        t.literal(R"(,"level":)");
        t.value(1, Table::scalar(k::Float));
        t.literal("}");
        return t.layout();
    }

    constexpr std::string_view kVoiceText = R"({"osc":{"detune":-0.25,"notes":[60,64,67],"on":true,"name":"lead"},"level":0.1})";
#endif
} // namespace

TEST_CASE("a reply written in place: the request's own buffer, its capacity, and the reply published from it",
          "[web][server]")
{
    Policy policy = serving();
    policy.server.maxReplyBytes = 100;
    Served served(policy, nullptr);
    const int port = served.port;

    // What is no request the caller holds has no buffer.
    std::int32_t capacity = -1;
    CHECK(fminus_web_http_reply_buffer(nullptr, &capacity) == nullptr);
    CHECK(capacity == 0);
    capacity = -1;
    CHECK(fminus_web_http_reply_buffer(served.server, &capacity) == nullptr);
    CHECK(capacity == 0);
    CHECK(replyWritten(nullptr, 200, 0) == FMINUS_WEB_INVALID);
    CHECK(replyWritten(served.server, 200, 0) == FMINUS_WEB_INVALID);

    // A body written into the buffer, its type, and every refusal before it, the request still the caller's.
    const int fd = connectTo(port);
    REQUIRE(sendAll(fd, get(port, "/hello")));
    void* r = takeOne(served.server);
    REQUIRE(r != nullptr);
    char* buffer = fminus_web_http_reply_buffer(r, &capacity);
    REQUIRE(buffer != nullptr);
    CHECK(capacity == 100);
    CHECK(fminus_web_http_reply_buffer(r, nullptr) == buffer);   // the same buffer each time, no capacity asked
    std::memset(buffer, 'x', 100);
    std::memcpy(buffer, "in place", 8);
    CHECK(replyWritten(r, 200, 101, "text/plain") == FMINUS_WEB_LIMIT);
    CHECK(replyWritten(r, 200, -1, "text/plain") == FMINUS_WEB_INVALID);
    CHECK(replyWritten(r, 204, 8) == FMINUS_WEB_INVALID);    // 204 has no body
    CHECK(replyWritten(r, 304, 8) == FMINUS_WEB_INVALID);
    CHECK(replyWritten(r, 199, 8) == FMINUS_WEB_INVALID);
    CHECK(replyWritten(r, 600, 8) == FMINUS_WEB_INVALID);
    CHECK(replyWritten(r, 200, 8, "text/plain\r\nX-A: b") == FMINUS_WEB_INVALID);
    CHECK(replyWritten(r, 200, 8, "text/plain") == 0);
    CHECK(replyWritten(r, 200, 8, "text/plain") == FMINUS_WEB_INVALID);   // answered: the handle is dead
    CHECK(fminus_web_http_reply_buffer(r, &capacity) == nullptr);
    CHECK(capacity == 0);
    Reader reader(fd);
    std::optional<Response> response = reader.next();
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->body == "in place");
    CHECK(response->header("content-type") == "text/plain");

    // The whole capacity, the default type; then no body (204), on the same connection.
    REQUIRE(sendAll(fd, get(port, "/hello")));
    r = takeOne(served.server);
    REQUIRE(r != nullptr);
    buffer = fminus_web_http_reply_buffer(r, &capacity);
    REQUIRE(buffer != nullptr);
    for (int i = 0; i < 100; ++i)
        buffer[i] = static_cast<char>('a' + i % 26);
    CHECK(replyWritten(r, 201, 100) == 0);
    response = reader.next();
    REQUIRE(response);
    CHECK(response->status == 201);
    CHECK(response->body.size() == 100);
    CHECK(response->body.substr(0, 3) == "abc");
    CHECK(response->header("content-type") == "application/octet-stream");
    REQUIRE(sendAll(fd, get(port, "/hello")));
    r = takeOne(served.server);
    REQUIRE(r != nullptr);
    CHECK(replyWritten(r, 204, 0) == 0);
    response = reader.next();
    REQUIRE(response);
    CHECK(response->status == 204);
    CHECK(response->body.empty());

    // A HEAD keeps the Content-Length and drops the body.
    REQUIRE(sendAll(fd, "HEAD /hello HTTP/1.1\r\nHost: " + hostOf(port) + "\r\n\r\n"));
    r = takeOne(served.server);
    REQUIRE(r != nullptr);
    std::memcpy(fminus_web_http_reply_buffer(r, nullptr), "twelve bytes", 12);
    CHECK(replyWritten(r, 200, 12, "a/b") == 0);
    response = reader.next(true);
    REQUIRE(response);
    CHECK(response->header("content-length") == "12");
    ::close(fd);
}

TEST_CASE("a request whose reply buffer was got and nothing published is unanswered, then answered by any reply",
          "[web][server]")
{
    Policy policy = serving();
    policy.server.replyTimeoutMs = 300;
    Served served(policy, nullptr);
    const int port = served.port;

    // Got, nothing published: the library answers 503 at replyTimeoutMs, and the reply then is cancelled.
    {
        const int fd = connectTo(port);
        REQUIRE(sendAll(fd, get(port, "/hello")));
        void* r = takeOne(served.server);
        REQUIRE(r != nullptr);
        std::int32_t capacity = 0;
        char* buffer = fminus_web_http_reply_buffer(r, &capacity);
        REQUIRE(buffer != nullptr);
        std::memcpy(buffer, "late", 4);
        Reader reader(fd);
        std::optional<Response> response = reader.next();
        REQUIRE(response);
        CHECK(response->status == 503);
        CHECK(fminus_web_http_reply_buffer(r, nullptr) == buffer);   // still the taker's, until it answers
        CHECK(replyWritten(r, 200, 4) == FMINUS_WEB_CANCELLED);
        CHECK(fminus_web_http_reply_buffer(r, nullptr) == nullptr);
        ::close(fd);
    }
    // Got, then answered by another reply, which copies over what was written there.
    {
        const int fd = connectTo(port);
        REQUIRE(sendAll(fd, get(port, "/hello")));
        void* r = takeOne(served.server);
        REQUIRE(r != nullptr);
        std::memcpy(fminus_web_http_reply_buffer(r, nullptr), "written", 7);
        CHECK(replyBytes(r, 200, "copied") == 0);
        Reader reader(fd);
        std::optional<Response> response = reader.next();
        REQUIRE(response);
        CHECK(response->body == "copied");
        ::close(fd);
    }
    // A server with a capacity of 0: a buffer, and no byte in it.
    Policy none = serving();
    none.server.maxReplyBytes = 0;
    Served empty(none, nullptr);
    const int fd = connectTo(empty.port);
    REQUIRE(sendAll(fd, get(empty.port, "/hello")));
    void* r = takeOne(empty.server);
    REQUIRE(r != nullptr);
    std::int32_t capacity = -1;
    CHECK(fminus_web_http_reply_buffer(r, &capacity) != nullptr);
    CHECK(capacity == 0);
    CHECK(replyWritten(r, 200, 1) == FMINUS_WEB_LIMIT);
    CHECK(replyWritten(r, 200, 0) == 0);
    Reader reader(fd);
    std::optional<Response> response = reader.next();
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->header("content-length") == "0");
    ::close(fd);
}

#if FMINUS_HAS_JSON
TEST_CASE("a JSON literal written into the reply buffer and published allocates nothing and makes no system call",
          "[web][server][json]")
{
    Policy policy = serving();
    policy.server.replyTimeoutMs = 20000;
    Served served(policy, nullptr);
    const int port = served.port;
    const std::vector<std::int32_t> literal = voiceLiteral();
    // What the audio thread does: takes a request, writes the literal into its buffer, and publishes it. 1 when
    // it answered one, 0 when there was none, -1 when anything failed.
    const auto handle = [&literal](void* server) -> int
    {
        void* r = fminus_web_http_next(server);
        if (r == nullptr)
            return 0;
        Voice voice{-0.25f, {60, 64, 67}, true, {'l', 'e', 'a', 'd', 0, 0, 0, 0}};
        std::uint64_t slots[2] = {0, 0};
        const Voice* address = &voice;
        std::memcpy(&slots[0], &address, sizeof address);
        const float level = 0.1f;
        std::memcpy(&slots[1], &level, sizeof level);
        std::int32_t capacity = 0;
        char* buffer = fminus_web_http_reply_buffer(r, &capacity);
        const std::int32_t bytes = fminus_rt_json_write(buffer, capacity, literal.data(), slots);
        if (buffer == nullptr || bytes < 0 || bytes > capacity)
            return -1;
        return fminus_web_http_reply_written(r, 200, bytes, "application/json", 16) == 0 ? 1 : -1;
    };
    std::vector<int> clients;
    const auto send = [&]
    {
        const int fd = connectTo(port);
        REQUIRE(sendAll(fd, get(port, "/hello")));
        clients.push_back(fd);
    };

    if (fminus::test::HeapCalls::counted())
    {
        send();
        int done = 0;
        for (int attempt = 0; attempt < 500 && done == 0; ++attempt)
        {
            long calls = 0;
            {
                const fminus::test::HeapCalls counting;
                done = handle(served.server);
                calls = counting.count();
            }
            if (done != 0)
            {
                CHECK(done == 1);
                CHECK(calls == 0);
            }
            else
                std::this_thread::sleep_for(2ms);
        }
        CHECK(done == 1);
        Reader replies(clients.back());
        std::optional<Response> response = replies.next();
        REQUIRE(response);
        CHECK(response->body == kVoiceText);
        CHECK(response->header("content-type") == "application/json");
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");

    send();
#if defined(__linux__)
    bool checked = false;
    for (int attempt = 0; attempt < 200 && !checked; ++attempt)
    {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            ::close(fds[0]);
            if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
                ::syscall(SYS_exit, 2);
            const int result = handle(served.server);
            const ssize_t written = ::write(fds[1], &result, sizeof result);
            ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
        }
        ::close(fds[1]);
        int status = 0;
        ::waitpid(child, &status, 0);
        int result = -2;
        const ssize_t got = ::read(fds[0], &result, sizeof result);
        ::close(fds[0]);
        INFO("status " << status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        {
            WARN("seccomp's strict mode is not available here: the system calls are not checked");
            break;
        }
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
        REQUIRE(got == static_cast<ssize_t>(sizeof result));
        CHECK(result >= 0);
        if (result == 1)
            checked = true;
        else
            std::this_thread::sleep_for(5ms);   // not published yet
    }
    CHECK(checked);
#else
    WARN("seccomp is Linux's: the system calls are not checked here");
#endif
    // The parent's copy of the request, untouched by the child, answered here as well.
    CHECK(handle(served.server) == 1);
    Reader replies(clients.back());
    std::optional<Response> response = replies.next();
    REQUIRE(response);
    CHECK(response->body == kVoiceText);
    for (int fd : clients)
        ::close(fd);
}
#endif

TEST_CASE("a URL's %s is percent-encoded as it is filled in, and reaches the server as the program wrote it",
          "[web][server]")
{
    std::mutex mutex;
    std::vector<std::string> seen;   // each request's path, its first place's text, and its query's "name"
    const auto record = [&](void* r)
    {
        std::int32_t length = 0;
        const char* path = fminus_web_http_path(r, &length);
        std::string entry(path, static_cast<std::size_t>(length));
        fminus_web_http_route(r);
        const char* text = fminus_web_http_text(r, 0, &length);
        entry += " " + std::string(text == nullptr ? "" : std::string(text, static_cast<std::size_t>(length)));
        text = fminus_web_http_query_text(r, "name", 4, &length);
        entry += " " + (text == nullptr ? std::string("-") : std::string(text, static_cast<std::size_t>(length)));
        entry += " " + bodyOf(r);
        {
            const std::lock_guard lock(mutex);
            seen.push_back(entry);
        }
        replyBytes(r, 200, "ok");
    };
    Served served(serving(), record, {"GET /presets/%s", "POST /presets/%s/%d"});
    const int port = served.port;
    const std::unique_ptr<Context> client = clientFor(port);
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    const auto getS = [&](const std::string& format, const std::vector<std::string>& texts,
                          const std::vector<std::int32_t>& ints = {})
    {
        std::vector<const char*> addresses;
        std::vector<std::int32_t> lengths;
        for (const std::string& t : texts)
        {
            addresses.push_back(t.data());
            lengths.push_back(static_cast<std::int32_t>(t.size()));
        }
        void* r = fminus_web_http_get_s(client->handle(), format.data(), static_cast<std::int32_t>(format.size()),
                                        ints.data(), static_cast<std::int32_t>(ints.size()), addresses.data(),
                                        lengths.data(), static_cast<std::int32_t>(texts.size()));
        REQUIRE(r != nullptr);
        const std::int32_t status = fminus_web_http_wait(r, 10000);
        fminus_web_http_close(r);
        return status;
    };
    // Unreserved bytes as they are; every other byte %XX, in the query, UTF-8 and the delimiters among them.
    CHECK(getS(base + "/presets/%s", {"warm.pad-1_~"}) == 200);
    CHECK(getS(base + "/presets/%s?name=%s&v=%d", {"lead", "a b/c?d=\xc3\xa9&#%"}, {42}) == 200);
    CHECK(getS(base + "/presets/%%%s", {"x"}) == 400);   // a literal's percent sign: the server takes none
    const std::string body = "{}";
    const std::string format = base + "/presets/%s/%d";
    const char* texts[] = {"pad"};
    const std::int32_t lengths[] = {3};
    const std::int32_t ints[] = {7};
    void* post = fminus_web_http_post_s(client->handle(), format.data(), static_cast<std::int32_t>(format.size()), ints,
                                        1, texts, lengths, 1, body.data(), 2, "application/json", 16);
    REQUIRE(post != nullptr);
    CHECK(fminus_web_http_wait(post, 10000) == 200);
    fminus_web_http_close(post);

    served.program.reset();
    const std::lock_guard lock(mutex);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "/presets/warm.pad-1_~ warm.pad-1_~ - ");
    CHECK(seen[1] == "/presets/lead lead a%20b%2Fc%3Fd%3D%C3%A9%26%23%25 ");
    CHECK(seen[2] == "/presets/pad/7 pad - {}");
}

#endif
