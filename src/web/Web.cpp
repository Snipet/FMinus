// src/web/Web.cpp: contexts, requests, and the I/O thread that drives them (Web.h).
//
// Who touches what:
//
// - A request's status, received count, body and size are written by the I/O thread only, and the body and size before
//   the status: the status is published last, with one release store, under its context's mutex (so a thread waiting
//   on the context's condition variable cannot miss it). After that store the I/O thread never touches the request
//   again, so the program may free it.
// - Everything else of a request in flight (its easy handle, its URL handle, its buffer) belongs to the I/O thread
//   from the moment the Start command is queued until that store; before, to the thread that starts it.
// - A context's list of requests, its count in flight and its waiters are under its mutex. Its share handle (DNS
//   entries and connections) belongs to the I/O thread, which makes it at its first request and frees it when the
//   context is forgotten.
// - The I/O thread takes commands (start, cancel, forget a context) from a queue under the engine's mutex, and is
//   woken by curl_multi_wakeup. Commands are handled in order: a cancel always finds its request started, and when a
//   Forget is acknowledged, every earlier command of that context has been handled, and the I/O thread holds nothing
//   of it.
// - The I/O thread is started under the engine's lifecycle mutex when its first user (a context) arrives, and joined
//   under it when its last user goes; a context arriving meanwhile waits for the join and starts a new one.
// - Every handle a program is given begins with its kind (Handle.h), and each C function here reads it first: a
//   request made is a Request; a server and a request it received are Server.cpp's, whose functions these call.
// - A deadline scope (fminus_web_http_within_begin) is a context's, under its mutex, with the thread that began it:
//   a request records the id of the innermost one its starting thread had open there (under the mutex, as it joins
//   the list), and a scope's end waits on the context's condition variable, as fminus_web_http_wait does, for those of
//   its requests still in the list to end, then queues an Expire for each left (FMINUS_WEB_TIMEOUT), under the mutex
//   as Context::stop queues its cancels, and waits for the I/O thread to have handled them. A thread that begins one
//   has a record of the context's (State::Holder), which fminus_web_http_within_abandon reads and writes without the
//   mutex, so that a call the loop budget or a bounds check stopped leaves its thread no scope without taking a lock:
//   it moves the record's generation on, and every scope begun under an earlier one is abandoned from then on.
// - With the server (FMINUS_WEB_SERVER), a context has at most one, made by its first fminus_web_http_listen under its
//   mutex, attached to the I/O thread by a command (Serve) queued under that mutex, and detached by another (Unserve)
//   when it is closed, the context stopped or destroyed (stopServer), before it is marked closed. The I/O thread turns
//   its servers after libcurl's transfers, and waits on their sockets in the same curl_multi_poll (Server.h).
#include "web/Web.h"

#include "fminus/Config.h"
#include "runtime/Format.h"
#include "web/Handle.h"
#include "web/Policy.h"
#include "web/Roots.h"
#if FMINUS_HAS_WEB_SERVER
#include "web/Server.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace fminus::web
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Request;
    } // namespace

    struct Context::State
    {
        CompiledPolicy policy;

        std::mutex mutex;
        std::condition_variable changed;   // a request's status, a waiter leaving
        std::vector<Request*> requests;    // every request not yet closed
        std::int32_t inFlight = 0;
        std::int32_t waiters = 0;
        bool dying = false;
        std::atomic<bool> stopped{false};   // stop(): set under the mutex; the I/O thread reads it as a hop begins
        bool usesEngine = false;   // set by the thread that starts requests, read by ~Context

        CURLSH* share = nullptr;   // the I/O thread's

        // A thread that has begun a scope here, and how many it has open: what fminus_web_http_within_abandon reads
        // without the mutex. Made by a thread's first begin (under the mutex), and kept for the context's life: a
        // record whose thread has none open is taken by the next thread that begins one, so there are never more than
        // the most threads that had scopes open here at once. Only its thread writes `generation` and `open` (its
        // begins and ends, under the mutex, and its abandon, without); `thread` is written under the mutex, by a begin
        // that takes a record with none open.
        struct Holder
        {
            std::atomic<std::thread::id> thread;
            std::atomic<std::uint64_t> generation{1};   // a scope begun under another generation is abandoned
            std::atomic<std::int32_t> open{0};          // the thread's scopes here of this generation
            Holder* next = nullptr;                     // set before the record is published, never after
        };
        std::atomic<Holder*> holders{nullptr};   // pushed under the mutex, read by abandon without it

        // The deadline scopes open (fminus_web_http_within_begin), every thread's, in the order they were begun, under
        // the mutex. A thread's innermost is its last here that is not abandoned; a request it starts joins that one
        // (Request::scope).
        struct Scope
        {
            std::uint64_t id = 0;            // never 0, never reused
            std::thread::id owner;           // the thread that began it, the only one that sees it
            bool bounded = false;            // it has a deadline: its own, or an enclosing one's
            Clock::time_point deadline;      // the earlier of its own and the enclosing scope's
            Holder* holder = nullptr;        // its thread's record
            std::uint64_t generation = 0;    // the record's when it began: abandoned once the record's is past it
        };
        std::vector<Scope> scopes;
        std::uint64_t scopesBegun = 0;
        bool destroying = false;   // ~Context has begun (under the mutex): a scope's end says so

        State() = default;
        State(const State&) = delete;
        State& operator=(const State&) = delete;
        ~State()
        {
            for (Holder* holder = holders.load(std::memory_order_relaxed); holder != nullptr;)
                delete std::exchange(holder, holder->next);
        }

#if FMINUS_HAS_WEB_SERVER
        Server* server = nullptr;   // its first listen's, under the mutex; freed by ~Context
#endif
    };

    namespace
    {
        struct Request : Handle
        {
            static constexpr Kind kKind = Kind::Made;
            Request()
                : Handle(Kind::Made)
            {
            }

            // Published (Web.h): the status last.
            std::atomic<std::int32_t> status{FMINUS_WEB_PENDING};
            std::atomic<std::int32_t> received{0};
            char* body = nullptr;
            std::int32_t size = 0;

            Context::State* context = nullptr;   // null: the shared refused request
            std::uint64_t scope = 0;             // the deadline scope it joined (State::Scope::id), under the context's
                                                 // mutex; 0: none

            // The I/O thread's while in flight.
            Target target;
            bool post = false;
            std::vector<char> data;   // a POST's body
            std::string type;         // its content type
            Clock::time_point deadline;
            std::int32_t redirects = 0;
            CURL* easy = nullptr;
            curl_slist* headers = nullptr;
            curl_slist* resolve = nullptr;
            char* buffer = nullptr;
            std::int32_t used = 0;
            std::int32_t capacity = 0;
            bool refusedAddress = false;
            bool overLimit = false;
        };

        // A program given no network: one request, refused, which needs no closing.
        struct RefusedRequest : Request
        {
            RefusedRequest()
            {
                status.store(FMINUS_WEB_REFUSED, std::memory_order_relaxed);
            }
        };

        Request& refused()
        {
            static RefusedRequest request;
            return request;
        }

        void freeRequest(Request* request)
        {
            if (request->target.url != nullptr)
                curl_url_cleanup(request->target.url);
            std::free(request->body);
            std::free(request->buffer);
            delete request;
        }

        // ---- the engine: the I/O thread and its queue ---------------------------------------------------------------

        struct Command
        {
            enum Kind
            {
                Start,
                Cancel,
                Expire,   // a cancel by a deadline scope's end: the request ends as FMINUS_WEB_TIMEOUT
                Forget,
#if FMINUS_HAS_WEB_SERVER
                Serve,     // the I/O thread starts driving `server`
                Unserve,   // and stops: its sockets closed, its requests given up
#endif
            } kind;
            Request* request = nullptr;
            Context::State* context = nullptr;
            std::uint64_t sequence = 0;
#if FMINUS_HAS_WEB_SERVER
            Server* server = nullptr;
#endif
        };

        class Engine
        {
        public:
            // A context's first request: starts the I/O thread if no other context is using it. False when it
            // could not be started.
            bool acquire()
            {
                std::lock_guard lifecycle(lifecycle_);
                if (users_ == 0)
                {
                    CURLM* multi = curl_multi_init();
                    if (multi == nullptr)
                        return false;
                    {
                        std::lock_guard lock(mutex_);
                        multi_ = multi;
                        stopping_ = false;
                    }
                    try
                    {
                        thread_ = std::thread([this] { run(); });
                    }
                    catch (...)
                    {
                        curl_multi_cleanup(multi);
                        multi_ = nullptr;
                        return false;
                    }
                    running_.store(true, std::memory_order_release);
                }
                ++users_;
                return true;
            }

            // A context that used the thread is gone: joins it if it was the last.
            void release()
            {
                std::lock_guard lifecycle(lifecycle_);
                if (--users_ > 0)
                    return;
                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
                curl_multi_wakeup(multi_);
                thread_.join();
                curl_multi_cleanup(multi_);
                multi_ = nullptr;
                running_.store(false, std::memory_order_release);
            }

            // Queues a command for the I/O thread and wakes it; its sequence number, for wait().
            std::uint64_t post(Command command)
            {
                std::uint64_t sequence = 0;
                {
                    std::lock_guard lock(mutex_);
                    sequence = command.sequence = ++posted_;
                    queue_.push_back(command);
                }
                curl_multi_wakeup(multi_);
                return sequence;
            }

            // post(), trying again until the queue can take the command: for the commands that must not fail (a
            // cancel, a context's last).
            std::uint64_t postSurely(Command command)
            {
                for (;;)
                {
                    try
                    {
                        return post(command);
                    }
                    catch (const std::bad_alloc&)
                    {
                        std::this_thread::yield();
                    }
                }
            }

            // Blocks until the I/O thread has handled the command `sequence` and everything before it.
            void wait(std::uint64_t sequence)
            {
                std::unique_lock lock(mutex_);
                handledChanged_.wait(lock, [&] { return handled_ >= sequence; });
            }

            // The sequence number of the last command queued so far, for wait(): a barrier.
            std::uint64_t posted()
            {
                std::lock_guard lock(mutex_);
                return posted_;
            }

            bool running() const
            {
                return running_.load(std::memory_order_acquire);
            }

        private:
            void run();
            void handle(const Command& command);
            void begin(Request* request);
            std::int32_t setUp(Request* request);
            void endHop(Request* request);
            void done(Request* request, CURLcode result);
            void finish(Request* request, std::int32_t status);

            std::mutex lifecycle_;
            std::thread thread_;
            std::int32_t users_ = 0;
            std::atomic<bool> running_{false};

            std::mutex mutex_;
            std::condition_variable handledChanged_;
            std::vector<Command> queue_;
            std::uint64_t posted_ = 0;
            std::uint64_t handled_ = 0;
            bool stopping_ = false;
            CURLM* multi_ = nullptr;

#if FMINUS_HAS_WEB_SERVER
            // The I/O thread's.
            Servers servers_;
            std::vector<curl_waitfd> descriptors_;
#endif
        };

        // Never destroyed: a host that exits with a context alive (and the I/O thread running) must not have the
        // thread's object destroyed under it at exit.
        Engine& engine()
        {
            static Engine* instance = new Engine;
            return *instance;
        }

        // libcurl's callbacks, on the I/O thread.
        std::size_t onBody(char* data, std::size_t size, std::size_t count, void* user)
        {
            auto* request = static_cast<Request*>(user);
            const std::size_t bytes = size * count;
            const std::int64_t limit = request->context->policy.policy.maxBodyBytes;
            if (static_cast<std::int64_t>(request->used) + static_cast<std::int64_t>(bytes) > limit)
            {
                request->overLimit = true;
                return 0;   // ends the transfer: CURLE_WRITE_ERROR
            }
            const std::int32_t needed = request->used + static_cast<std::int32_t>(bytes);
            if (needed + 1 > request->capacity)
            {
                std::int64_t capacity = std::max<std::int64_t>(request->capacity, 4096);
                while (capacity < needed + 1)
                    capacity *= 2;
                capacity = std::min<std::int64_t>(capacity, limit + 1);
                void* grown = std::realloc(request->buffer, static_cast<std::size_t>(capacity));
                if (grown == nullptr)
                    return 0;
                request->buffer = static_cast<char*>(grown);
                request->capacity = static_cast<std::int32_t>(capacity);
            }
            if (bytes != 0)
                std::memcpy(request->buffer + request->used, data, bytes);
            request->used = needed;
            request->received.store(needed, std::memory_order_relaxed);
            return bytes;
        }

        // Every connection the request opens: its address checked against the policy before the socket is made.
        curl_socket_t onOpenSocket(void* user, curlsocktype purpose, curl_sockaddr* where)
        {
            auto* request = static_cast<Request*>(user);
            Address address;
            if (purpose == CURLSOCKTYPE_IPCXN && where->family == AF_INET && where->addrlen >= sizeof(sockaddr_in))
            {
                sockaddr_in in{};
                std::memcpy(&in, &where->addr, sizeof in);
                address.family = 4;
                std::memcpy(address.bytes.data(), &in.sin_addr, 4);
            }
            else if (purpose == CURLSOCKTYPE_IPCXN && where->family == AF_INET6 &&
                     where->addrlen >= sizeof(sockaddr_in6))
            {
                sockaddr_in6 in6{};
                std::memcpy(&in6, &where->addr, sizeof in6);
                address.family = 6;
                std::memcpy(address.bytes.data(), &in6.sin6_addr, 16);
            }
            if (address.family == 0 || !reachable(request->target.reach, request->target.address, address))
            {
                request->refusedAddress = true;
                return CURL_SOCKET_BAD;
            }
            return static_cast<curl_socket_t>(socket(where->family, where->socktype, where->protocol));
        }

        void Engine::run()
        {
            std::vector<Command> commands;
            for (;;)
            {
                bool stop = false;
                {
                    std::lock_guard lock(mutex_);
                    commands.swap(queue_);
                    stop = stopping_;
                }
                for (const Command& command : commands)
                    handle(command);
                if (!commands.empty())
                {
                    {
                        std::lock_guard lock(mutex_);
                        handled_ = commands.back().sequence;
                    }
                    handledChanged_.notify_all();
                    commands.clear();
                }
                if (stop)
                    return;   // every context has been forgotten: nothing is in flight
                int running = 0;
                curl_multi_perform(multi_, &running);
                int left = 0;
                while (CURLMsg* message = curl_multi_info_read(multi_, &left))
                {
                    if (message->msg != CURLMSG_DONE)
                        continue;
                    Request* request = nullptr;
                    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
                    const CURLcode result = message->data.result;
                    done(request, result);
                }
#if FMINUS_HAS_WEB_SERVER
                servers_.turn();
                servers_.descriptors(descriptors_);
                curl_multi_poll(multi_, descriptors_.empty() ? nullptr : descriptors_.data(),
                                static_cast<unsigned>(descriptors_.size()), servers_.timeoutMs(1000), nullptr);
#else
                curl_multi_poll(multi_, nullptr, 0, 1000, nullptr);
#endif
            }
        }

        void Engine::handle(const Command& command)
        {
            switch (command.kind)
            {
            case Command::Start:
                begin(command.request);
                break;
            case Command::Cancel:
                if (command.request->easy != nullptr)
                    finish(command.request, FMINUS_WEB_CANCELLED);
                break;
            case Command::Expire:
                if (command.request->easy != nullptr)
                    finish(command.request, FMINUS_WEB_TIMEOUT);
                break;
            case Command::Forget:
                if (command.context->share != nullptr)
                {
                    curl_share_cleanup(command.context->share);
                    command.context->share = nullptr;
                }
                break;
#if FMINUS_HAS_WEB_SERVER
            case Command::Serve:
                servers_.attach(command.server);
                break;
            case Command::Unserve:
                servers_.detach(command.server);
                break;
#endif
            }
        }

        // Makes the request's easy handle for its current URL and adds it to the multi handle; or ends it.
        void Engine::begin(Request* request)
        {
            Context::State& context = *request->context;
            if (context.stopped.load(std::memory_order_acquire))   // stop() ran after its start was queued
            {
                finish(request, FMINUS_WEB_CANCELLED);
                return;
            }
            if (context.share == nullptr)
            {
                context.share = curl_share_init();
                if (context.share != nullptr)
                {
                    curl_share_setopt(context.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
                    curl_share_setopt(context.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
                    curl_share_setopt(context.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
                }
            }
            const std::int32_t status = context.share == nullptr ? FMINUS_WEB_FAILED : setUp(request);
            if (status != 0)
            {
                finish(request, status);
                return;
            }
            if (curl_multi_add_handle(multi_, request->easy) != CURLM_OK)
                finish(request, FMINUS_WEB_FAILED);
        }

        // The request's easy handle for its current hop: 0, or the status that ends the request.
        std::int32_t Engine::setUp(Request* request)
        {
            const Policy& policy = request->context->policy.policy;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(request->deadline - Clock::now());
            if (left.count() <= 0)
                return FMINUS_WEB_TIMEOUT;
            request->easy = curl_easy_init();
            if (request->easy == nullptr)
                return FMINUS_WEB_FAILED;
            CURL* easy = request->easy;
            request->used = 0;
            request->refusedAddress = false;
            request->overLimit = false;
            request->received.store(0, std::memory_order_relaxed);
            bool ok = true;
            auto set = [&](CURLoption option, auto value)
            {
                ok = ok && curl_easy_setopt(easy, option, value) == CURLE_OK;
            };
            set(CURLOPT_PRIVATE, static_cast<void*>(request));
            set(CURLOPT_SHARE, request->context->share);
            set(CURLOPT_CURLU, request->target.url);
            set(CURLOPT_PROTOCOLS_STR, request->target.https ? "https" : "http");
            set(CURLOPT_FOLLOWLOCATION, 0L);
            set(CURLOPT_NOSIGNAL, 1L);
            set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
            set(CURLOPT_USERAGENT, "FMinus/" FMINUS_VERSION_STRING);
            set(CURLOPT_TIMEOUT_MS, static_cast<long>(left.count()));
            set(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(left.count()));
            set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(policy.maxBodyBytes));
            set(CURLOPT_WRITEFUNCTION, &onBody);
            set(CURLOPT_WRITEDATA, static_cast<void*>(request));
            set(CURLOPT_OPENSOCKETFUNCTION, &onOpenSocket);
            set(CURLOPT_OPENSOCKETDATA, static_cast<void*>(request));
            set(CURLOPT_SSL_VERIFYPEER, 1L);
            set(CURLOPT_SSL_VERIFYHOST, 2L);
            set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
            for (const std::string& entry : policy.resolve)
            {
                // libcurl moves the entries into the context's DNS cache (its share) when the transfer starts.
                curl_slist* entries = curl_slist_append(request->resolve, entry.c_str());
                if (entries == nullptr)
                    return FMINUS_WEB_FAILED;
                request->resolve = entries;
            }
            if (request->resolve != nullptr)
                set(CURLOPT_RESOLVE, request->resolve);
            if (request->target.https)
            {
                if (!policy.caFile.empty())
                {
                    set(CURLOPT_CAINFO, policy.caFile.c_str());
                    set(CURLOPT_CAPATH, static_cast<const char*>(nullptr));
                }
                else if (!useSystemRoots(easy))
                    return FMINUS_WEB_CERTIFICATE;
            }
            if (request->post)
            {
                const std::string type =
                    "Content-Type: " +
                    (request->type.empty() ? std::string("application/octet-stream") : request->type);
                request->headers = curl_slist_append(nullptr, type.c_str());
                curl_slist* headers =
                    request->headers == nullptr ? nullptr : curl_slist_append(request->headers, "Expect:");
                if (headers == nullptr)
                    return FMINUS_WEB_FAILED;
                set(CURLOPT_HTTPHEADER, request->headers);
                set(CURLOPT_POST, 1L);
                set(CURLOPT_POSTFIELDS, request->data.empty() ? "" : request->data.data());
                set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request->data.size()));
            }
            else
                set(CURLOPT_HTTPGET, 1L);
            return ok ? 0 : FMINUS_WEB_FAILED;
        }

        // The current hop's handle and lists go (the request's URL handle stays).
        void Engine::endHop(Request* request)
        {
            if (request->easy != nullptr)
            {
                curl_multi_remove_handle(multi_, request->easy);
                curl_easy_cleanup(request->easy);
                request->easy = nullptr;
            }
            curl_slist_free_all(request->headers);
            request->headers = nullptr;
            curl_slist_free_all(request->resolve);
            request->resolve = nullptr;
        }

        // Ends the request's current hop: its easy handle and lists go; then the request ends with `status`, or, for
        // a redirect the policy allows, starts its next hop.
        void Engine::done(Request* request, CURLcode result)
        {
            if (result == CURLE_OK)
            {
                long code = 0;
                curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &code);
                const bool redirect = code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
                char* location = nullptr;
                if (redirect)
                    curl_easy_getinfo(request->easy, CURLINFO_REDIRECT_URL, &location);
                if (!redirect || location == nullptr)
                {
                    finish(request, static_cast<std::int32_t>(code));
                    return;
                }
                if (request->redirects >= request->context->policy.policy.maxRedirects)
                {
                    finish(request, FMINUS_WEB_LIMIT);
                    return;
                }
                Target next;
                const std::int32_t checked = checkUrl(request->context->policy, location, next);
                if (checked != 0)
                {
                    finish(request, checked);
                    return;
                }
                // The next hop: the old handle and lists go, and a 301, 302 or 303 after a POST is a GET.
                endHop(request);
                curl_url_cleanup(request->target.url);
                request->target = next;
                ++request->redirects;
                if (request->post && code != 307 && code != 308)
                {
                    request->post = false;
                    request->data.clear();
                }
                begin(request);
                return;
            }
            std::int32_t status = FMINUS_WEB_TRANSPORT;
            switch (result)
            {
            case CURLE_OPERATION_TIMEDOUT:
                status = FMINUS_WEB_TIMEOUT;
                break;
            case CURLE_PEER_FAILED_VERIFICATION:
            case CURLE_SSL_CACERT_BADFILE:
            case CURLE_SSL_ISSUER_ERROR:
                status = FMINUS_WEB_CERTIFICATE;
                break;
            case CURLE_FILESIZE_EXCEEDED:
                status = FMINUS_WEB_LIMIT;
                break;
            case CURLE_OUT_OF_MEMORY:
                status = FMINUS_WEB_FAILED;
                break;
            case CURLE_WRITE_ERROR:
                status = request->overLimit ? FMINUS_WEB_LIMIT : FMINUS_WEB_FAILED;
                break;
            default:
                if (request->refusedAddress)
                    status = FMINUS_WEB_REFUSED;
                break;
            }
            finish(request, status);
        }

        // Ends the request: its handle leaves the multi handle, its body is kept for an HTTP status and freed for a
        // failure, and its status is published, last.
        void Engine::finish(Request* request, std::int32_t status)
        {
            endHop(request);
            if (request->target.url != nullptr)
            {
                curl_url_cleanup(request->target.url);
                request->target.url = nullptr;
            }
            request->data.clear();
            request->data.shrink_to_fit();
            if (status > 0)
            {
                if (request->buffer == nullptr)
                    request->buffer = static_cast<char*>(std::malloc(1));
                if (request->buffer == nullptr)
                    status = FMINUS_WEB_FAILED;
                else
                {
                    request->buffer[request->used] = '\0';
                    request->body = request->buffer;
                    request->size = request->used;
                    request->buffer = nullptr;
                }
            }
            if (status <= 0)
            {
                std::free(request->buffer);
                request->buffer = nullptr;
                request->received.store(0, std::memory_order_relaxed);
            }
            else
                request->received.store(request->size, std::memory_order_relaxed);
            Context::State& context = *request->context;
            {
                std::lock_guard lock(context.mutex);
                --context.inFlight;
                request->status.store(status, std::memory_order_release);   // the request is the program's again
            }
            context.changed.notify_all();
        }

        // Whether `scope` was abandoned (fminus_web_http_within_abandon): its thread's record has moved on to a later
        // generation. Under the context's mutex; for another thread's scope, a stale read says it is not, which only
        // keeps it in the list a little longer (no thread but its own sees it).
        bool abandoned(const Context::State::Scope& scope)
        {
            return scope.holder->generation.load(std::memory_order_relaxed) != scope.generation;
        }

        // The calling thread's innermost deadline scope under the context, or null: its last begun that it has neither
        // ended nor abandoned. Under the context's mutex; the pointer only until the mutex is let go (another thread's
        // begin may move the scopes).
        Context::State::Scope* innermost(Context::State& context)
        {
            const std::thread::id me = std::this_thread::get_id();
            for (auto scope = context.scopes.rbegin(); scope != context.scopes.rend(); ++scope)
            {
                if (scope->owner == me && !abandoned(*scope))
                    return &*scope;
            }
            return nullptr;
        }

        // Drops the abandoned scopes from the list, under the context's mutex (it allocates nothing).
        void dropAbandoned(Context::State& context)
        {
            std::erase_if(context.scopes, [](const Context::State::Scope& scope) { return abandoned(scope); });
        }

        // The calling thread's record under the context, made or taken if it has none, or null when memory ran out.
        // Under the context's mutex.
        Context::State::Holder* holderOf(Context::State& context)
        {
            static_assert(std::atomic<std::thread::id>::is_always_lock_free,
                          "fminus_web_http_within_abandon reads a record's thread without a lock");
            const std::thread::id me = std::this_thread::get_id();
            Context::State::Holder* free = nullptr;
            for (Context::State::Holder* holder = context.holders.load(std::memory_order_relaxed); holder != nullptr;
                 holder = holder->next)
            {
                if (holder->thread.load(std::memory_order_relaxed) == me)
                    return holder;
                // A record with none open is no thread's: its abandon, if one ran, stored `open` last, so its
                // generation is the one a scope begun now must carry.
                if (free == nullptr && holder->open.load(std::memory_order_acquire) == 0)
                    free = holder;
            }
            if (free == nullptr)
            {
                free = new (std::nothrow) Context::State::Holder;
                if (free == nullptr)
                    return nullptr;
                free->next = context.holders.load(std::memory_order_relaxed);
                context.holders.store(free, std::memory_order_release);
            }
            free->thread.store(me, std::memory_order_release);
            return free;
        }

        // A POST's body a JSON literal's writer makes (fminus_web_http_post_json): the writer, the literal's
        // description and its values.
        struct JsonBody
        {
            fminus_web_json_writer write;
            const void* description;
            const void* values;
        };

        // A request under `context`, checked, and started unless the check or a limit ended it; or, with a `refusal`
        // (the URL's places could not be filled: expandUrl), ended with it before its URL is looked at. A POST's body
        // is the `bytes` bytes at `data`, or with `json` the text its writer makes, measured first and then written
        // into the request's own body.
        Request* start(Context::State* context, std::string url, bool post, const void* data, std::int32_t bytes,
                       std::string_view type, std::int32_t refusal = 0, const JsonBody* json = nullptr)
        {
            Request* request = new (std::nothrow) Request;
            if (request == nullptr)
                return nullptr;
            request->context = context;
            const Policy& policy = context->policy.policy;
            std::int32_t status = refusal != 0 ? refusal : checkUrl(context->policy, url, request->target);
            if (status == 0 && post && json != nullptr)
            {
                request->post = true;
                const std::int32_t measured =
                    json->write == nullptr ? -1 : json->write(nullptr, 0, json->description, json->values);
                if (measured < 0)
                    status = FMINUS_WEB_INVALID;
                else if (measured > policy.maxBodyBytes)
                    status = FMINUS_WEB_LIMIT;
                else if (!validContentType(type))
                    status = FMINUS_WEB_REFUSED;
                else
                {
                    try
                    {
                        request->data.resize(static_cast<std::size_t>(measured));
                        if (json->write(request->data.data(), measured, json->description, json->values) != measured)
                            status = FMINUS_WEB_INVALID;   // the values changed between the two passes
                        request->type = std::string(type);
                    }
                    catch (const std::bad_alloc&)
                    {
                        status = FMINUS_WEB_FAILED;
                    }
                }
            }
            else if (status == 0 && post)
            {
                request->post = true;
                if (bytes < 0 || (data == nullptr && bytes > 0))
                    status = FMINUS_WEB_INVALID;
                else if (bytes > policy.maxBodyBytes)
                    status = FMINUS_WEB_LIMIT;
                else if (!validContentType(type))
                    status = FMINUS_WEB_REFUSED;
                else
                {
                    try
                    {
                        request->data.assign(static_cast<const char*>(data), static_cast<const char*>(data) + bytes);
                        request->type = std::string(type);
                    }
                    catch (const std::bad_alloc&)
                    {
                        status = FMINUS_WEB_FAILED;
                    }
                }
            }
            {
                std::lock_guard lock(context->mutex);
                if (status == 0 && !context->usesEngine)
                {
                    if (engine().acquire())   // the context's mutex, then the engine's lifecycle mutex
                        context->usesEngine = true;
                    else
                        status = FMINUS_WEB_FAILED;
                }
                try
                {
                    context->requests.push_back(request);
                }
                catch (const std::bad_alloc&)
                {
                    if (request->target.url != nullptr)
                        curl_url_cleanup(request->target.url);
                    delete request;
                    return nullptr;
                }
                if (const Context::State::Scope* scope = innermost(*context))
                    request->scope = scope->id;
                if (status == 0 && context->inFlight >= policy.maxRequests)
                    status = FMINUS_WEB_LIMIT;
                if (status == 0 && context->stopped.load(std::memory_order_relaxed))
                    status = FMINUS_WEB_CANCELLED;
                if (status != 0)
                {
                    if (request->target.url != nullptr)
                    {
                        curl_url_cleanup(request->target.url);
                        request->target.url = nullptr;
                    }
                    request->status.store(status, std::memory_order_release);
                    return request;
                }
                ++context->inFlight;
            }
            request->deadline = Clock::now() + std::chrono::milliseconds(policy.timeoutMs);
            try
            {
                engine().post({Command::Start, request, context, 0});
            }
            catch (const std::bad_alloc&)
            {
                std::lock_guard lock(context->mutex);
                --context->inFlight;
                request->status.store(FMINUS_WEB_FAILED, std::memory_order_release);
            }
            return request;
        }

        std::string expand(const char* text, std::int32_t length, const std::int32_t* ints, std::int32_t count)
        {
            std::string out;
            runtime::expandFormat(text, length, ints, count, [&](char c) { out.push_back(c); });
            return out;
        }

        // The URL a format with %d and %s places makes (runtime/Format.h), in `out`: each %s's text percent-encoded,
        // every byte but A-Z, a-z, 0-9, "-", ".", "_" and "~" written %XX, so that a text never ends a part of the URL
        // nor begins one. 0, or FMINUS_WEB_INVALID when a text is not one (a negative length, or a null address and
        // bytes), when a %s is in the scheme or the authority (its host, its port: everything before the "://" the
        // literal writes and up to the "/", "?" or "#" after it; with no "://" before it, a %s is in the scheme), or
        // when a path segment a %s is written in is empty, "." or ".." once filled in. The parts are the literal's: a
        // %d writes digits and a minus, and an encoded text none of ":/?#", so every delimiter is the literal's own.
        // Only the first 8193 bytes are kept (one more than a URL may have, so checkUrl finds it too long) and the
        // rest only looked at: a text of any length takes no more memory.
        std::int32_t expandUrl(const char* format, std::int32_t length, const std::int32_t* ints, std::int32_t count,
                               const char* const* texts, const std::int32_t* lengths, std::int32_t textCount,
                               std::string& out)
        {
            constexpr std::size_t kKept = 8192 + 1;
            enum class Part
            {
                Scheme,
                Authority,
                Path,
                Query,   // or the fragment: a text there is encoded and nothing more
            };
            Part part = Part::Scheme;
            int slashes = 0;   // in the scheme: 1 after a ":", 2 after ":/"
            // The path segment being written: its bytes, whether they are all "." (two at most), and whether a %s is
            // in it.
            std::size_t segment = 0;
            bool dots = true;
            bool hasText = false;
            bool invalid = false;
            const auto endSegment = [&]
            {
                if (hasText && (segment == 0 || (dots && segment <= 2)))
                    invalid = true;
                segment = 0;
                dots = true;
                hasText = false;
            };
            const auto put = [&](char c)
            {
                if (out.size() < kKept)
                    out.push_back(c);
            };
            const auto literal = [&](char c)
            {
                put(c);
                switch (part)
                {
                case Part::Scheme:
                    if (c == ':')
                        slashes = 1;
                    else if (c == '/' && slashes > 0)
                    {
                        if (++slashes == 3)
                            part = Part::Authority;
                    }
                    else
                        slashes = 0;
                    break;
                case Part::Authority:
                    if (c == '/')
                        part = Part::Path;
                    else if (c == '?' || c == '#')
                        part = Part::Query;
                    break;
                case Part::Path:
                    if (c == '/' || c == '?' || c == '#')
                    {
                        endSegment();
                        if (c != '/')
                            part = Part::Query;
                    }
                    else
                    {
                        dots = dots && c == '.';
                        ++segment;
                    }
                    break;
                case Part::Query:
                    break;
                }
            };
            const auto text = [&](const char* bytes, std::int32_t size)
            {
                static constexpr char kHex[] = "0123456789ABCDEF";
                if (part == Part::Scheme || part == Part::Authority)
                    invalid = true;
                hasText = true;
                for (std::int32_t i = 0; i < size; ++i)
                {
                    const char c = bytes[i];
                    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                            c == '-' || c == '.' || c == '_' || c == '~';
                    if (unreserved)
                        put(c);
                    else
                    {
                        put('%');
                        put(kHex[static_cast<unsigned char>(c) >> 4]);
                        put(kHex[static_cast<unsigned char>(c) & 15]);
                    }
                    if (part == Part::Path)
                    {
                        dots = dots && c == '.';
                        segment += unreserved ? 1 : 3;
                    }
                    if (out.size() >= kKept && (part != Part::Path || !dots || segment > 2))
                        break;   // too long already, and nothing more to tell of its segment
                }
            };
            if (!runtime::expandPlaces(format, length, ints, count, texts, lengths, textCount, literal, text))
                return FMINUS_WEB_INVALID;
            if (part == Part::Path)
                endSegment();
            return invalid ? FMINUS_WEB_INVALID : 0;
        }

        // Closes the context's server, if it has one: the I/O thread lets go of it (its sockets closed, its requests
        // given up), then it is marked closed (its port FMINUS_WEB_CANCELLED, every thread in accept woken). Waits for
        // the I/O thread: not under the context's mutex, which the I/O thread takes.
        void stopServer([[maybe_unused]] Context::State& state)
        {
#if FMINUS_HAS_WEB_SERVER
            Server* server = nullptr;
            bool usesEngine = false;
            {
                std::lock_guard lock(state.mutex);
                server = state.server;
                usesEngine = state.usesEngine;
            }
            if (server == nullptr)
                return;
            if (usesEngine)
            {
                Command command{Command::Unserve, nullptr, &state, 0};
                command.server = server;
                engine().wait(engine().postSurely(command));
            }
            closeServer(*server);
#endif
        }
    } // namespace

#if FMINUS_HAS_WEB_SERVER
    bool scopeDeadline(Context::State* context, Clock::time_point& deadline) noexcept
    {
        std::lock_guard lock(context->mutex);
        const Context::State::Scope* scope = innermost(*context);
        if (scope == nullptr || !scope->bounded)
            return false;
        deadline = scope->deadline;
        return true;
    }
#endif

    std::string checkPolicy(const Policy& policy)
    {
        CompiledPolicy compiled;
        return compilePolicy(policy, compiled);
    }

    std::unique_ptr<Context> Context::create(const Policy& policy, std::string& error)
    {
        auto state = std::make_unique<State>();
        error = compilePolicy(policy, state->policy);
        if (!error.empty())
            return nullptr;
        // libcurl's global state (Mbed TLS's or Schannel's among it), held while the context lives: libcurl counts
        // these calls, and is thread-safe in them.
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            error = "libcurl could not be initialized";
            return nullptr;
        }
        return std::unique_ptr<Context>(new Context(std::move(state)));
    }

    Context::Context(std::unique_ptr<State> state)
        : state_(std::move(state))
    {
    }

    Context::~Context()
    {
        State& state = *state_;
        {
            std::lock_guard lock(state.mutex);
            state.destroying = true;
        }
        stopServer(state);
        if (state.usesEngine)
        {
            // Cancels what is in flight; the Forget, handled after those, frees the share.
            std::vector<Request*> pending;
            pending.reserve(state.requests.size());   // may throw only here, before anything is cancelled
            {
                std::lock_guard lock(state.mutex);
                for (Request* request : state.requests)
                {
                    if (request->status.load(std::memory_order_acquire) == FMINUS_WEB_PENDING)
                        pending.push_back(request);
                }
            }
            for (Request* request : pending)
                engine().postSurely({Command::Cancel, request, &state, 0});
            engine().wait(engine().postSurely({Command::Forget, nullptr, &state, 0}));
        }
        {
            std::unique_lock lock(state.mutex);
            state.dying = true;
            state.changed.notify_all();
            state.changed.wait(lock, [&] { return state.waiters == 0; });
        }
        for (Request* request : state.requests)
            freeRequest(request);
        state.requests.clear();
#if FMINUS_HAS_WEB_SERVER
        freeServer(state.server);   // its threads in accept, woken above, gone first
        state.server = nullptr;
#endif
        if (state.usesEngine)
            engine().release();
        curl_global_cleanup();
    }

    std::int32_t Context::port() const noexcept
    {
#if FMINUS_HAS_WEB_SERVER
        Server* server = nullptr;
        {
            std::lock_guard lock(state_->mutex);
            server = state_->server;   // freed only by ~Context
        }
        if (server == nullptr)
            return 0;
        const std::int32_t port = fminus_web_http_port(serverHandle(server));
        return port > 0 ? port : 0;
#else
        return 0;
#endif
    }

    // The flag first, under the mutex: a start that takes the mutex after it is cancelled there, one before it is in
    // the list, and its Start command, if it is queued after the cancels below, finds the flag as its hop begins. Then
    // a cancel of each request still pending, one at a time, each waited for. Each is queued under the mutex, so that
    // a thread closing that request meanwhile (close removes it under the mutex) queues its own cancel after it, and
    // frees the request only once both have been handled.
    void Context::stop() noexcept
    {
        State& state = *state_;
        bool usesEngine = false;
        {
            std::lock_guard lock(state.mutex);
            state.stopped.store(true, std::memory_order_release);
            usesEngine = state.usesEngine;
        }
        stopServer(state);
        if (!usesEngine)
            return;
        for (;;)
        {
            std::uint64_t sequence = 0;
            {
                std::lock_guard lock(state.mutex);
                for (Request* request : state.requests)
                {
                    if (request->status.load(std::memory_order_acquire) == FMINUS_WEB_PENDING)
                    {
                        sequence = engine().postSurely({Command::Cancel, request, &state, 0});
                        break;
                    }
                }
            }
            if (sequence == 0)
                return;
            engine().wait(sequence);
        }
    }

    void* Context::handle() noexcept
    {
        return state_.get();
    }

    std::int32_t Context::check(std::string_view url) const
    {
        Target target;
        const std::int32_t status = checkUrl(state_->policy, url, target);
        if (target.url != nullptr)
            curl_url_cleanup(target.url);
        return status;
    }

    bool ioThreadRunning()
    {
        return engine().running();
    }
} // namespace fminus::web

using fminus::web::Context;

namespace
{
    // A request made, as its handle (null stays null).
    void* madeHandle(fminus::web::Request* request) noexcept
    {
        return request == nullptr ? nullptr : fminus::web::toHandle(request);
    }

    fminus::web::Request* made(void* handle) noexcept
    {
        return fminus::web::as<fminus::web::Request>(handle);
    }

    const fminus::web::Request* made(const void* handle) noexcept
    {
        return fminus::web::as<fminus::web::Request>(handle);
    }
} // namespace

extern "C"
{
    void* fminus_web_http_get(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept
    {
        if (context == nullptr)
            return madeHandle(&fminus::web::refused());
        try
        {
            return madeHandle(fminus::web::start(static_cast<Context::State*>(context),
                                                 fminus::web::expand(url, length, ints, count), false, nullptr, 0,
                                                 {}));
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    void* fminus_web_http_post(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                               std::int32_t count, const void* data, std::int32_t bytes, const char* type,
                               std::int32_t typeLength) noexcept
    {
        if (context == nullptr)
            return madeHandle(&fminus::web::refused());
        try
        {
            const std::string_view contentType = type == nullptr || typeLength <= 0
                                                     ? std::string_view()
                                                     : std::string_view(type, static_cast<std::size_t>(typeLength));
            return madeHandle(fminus::web::start(static_cast<Context::State*>(context),
                                                 fminus::web::expand(url, length, ints, count), true, data, bytes,
                                                 contentType));
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    void* fminus_web_http_get_s(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                                std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                std::int32_t textCount) noexcept
    {
        if (context == nullptr)
            return madeHandle(&fminus::web::refused());
        try
        {
            std::string filled;
            const std::int32_t refusal =
                fminus::web::expandUrl(url, length, ints, count, texts, lengths, textCount, filled);
            return madeHandle(fminus::web::start(static_cast<Context::State*>(context), std::move(filled), false,
                                                 nullptr, 0, {}, refusal));
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    void* fminus_web_http_post_s(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                                 std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                 std::int32_t textCount, const void* data, std::int32_t bytes, const char* type,
                                 std::int32_t typeLength) noexcept
    {
        if (context == nullptr)
            return madeHandle(&fminus::web::refused());
        try
        {
            const std::string_view contentType = type == nullptr || typeLength <= 0
                                                     ? std::string_view()
                                                     : std::string_view(type, static_cast<std::size_t>(typeLength));
            std::string filled;
            const std::int32_t refusal =
                fminus::web::expandUrl(url, length, ints, count, texts, lengths, textCount, filled);
            return madeHandle(fminus::web::start(static_cast<Context::State*>(context), std::move(filled), true, data,
                                                 bytes, contentType, refusal));
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    void* fminus_web_http_post_json(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                                    std::int32_t count, const char* const* texts, const std::int32_t* lengths,
                                    std::int32_t textCount, fminus_web_json_writer write, const void* description,
                                    const void* values) noexcept
    {
        if (context == nullptr)
            return madeHandle(&fminus::web::refused());
        try
        {
            std::string filled;
            const std::int32_t refusal =
                fminus::web::expandUrl(url, length, ints, count, texts, lengths, textCount, filled);
            const fminus::web::JsonBody body{write, description, values};
            return madeHandle(fminus::web::start(static_cast<Context::State*>(context), std::move(filled), true, nullptr,
                                                 0, "application/json", refusal, &body));
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    std::int32_t fminus_web_http_wait(void* handle, std::int32_t timeoutMs) noexcept
    {
        fminus::web::Request* request = made(handle);
        if (request == nullptr)
            return FMINUS_WEB_INVALID;
        const std::int32_t now = request->status.load(std::memory_order_acquire);
        if (now != FMINUS_WEB_PENDING || timeoutMs == 0 || request->context == nullptr)
            return now;
        Context::State& context = *request->context;
        const auto started = fminus::web::Clock::now();
        std::unique_lock lock(context.mutex);
        ++context.waiters;
        const auto ready = [&]
        {
            return context.dying || request->status.load(std::memory_order_acquire) != FMINUS_WEB_PENDING;
        };
        // The calling thread's innermost deadline scope bounds the wait as its own timeout does.
        bool bounded = timeoutMs >= 0;
        auto deadline =
            bounded ? started + std::chrono::milliseconds(timeoutMs) : fminus::web::Clock::time_point::max();
        if (const Context::State::Scope* scope = fminus::web::innermost(context); scope != nullptr && scope->bounded)
        {
            deadline = bounded ? std::min(deadline, scope->deadline) : scope->deadline;
            bounded = true;
        }
        if (!bounded)
            context.changed.wait(lock, ready);
        else
            context.changed.wait_until(lock, deadline, ready);
        const std::int32_t status = request->status.load(std::memory_order_acquire);
        if (--context.waiters == 0 && context.dying)
            context.changed.notify_all();
        return status;
    }

    std::int32_t fminus_web_http_within_begin(void* context, std::int32_t timeoutMs) noexcept
    {
        if (context == nullptr)
            return 0;   // a program given no network: its requests are refused at once, and nothing waits
        Context::State& state = *static_cast<Context::State*>(context);
        const auto now = fminus::web::Clock::now();
        std::lock_guard lock(state.mutex);
        fminus::web::dropAbandoned(state);
        Context::State::Holder* holder = fminus::web::holderOf(state);
        if (holder == nullptr)
            return FMINUS_WEB_FAILED;
        Context::State::Scope scope;
        scope.owner = std::this_thread::get_id();
        scope.holder = holder;
        scope.generation = holder->generation.load(std::memory_order_relaxed);
        scope.bounded = timeoutMs >= 0;
        if (scope.bounded)
            scope.deadline = now + std::chrono::milliseconds(timeoutMs);
        if (const Context::State::Scope* outer = fminus::web::innermost(state); outer != nullptr && outer->bounded)
        {
            if (!scope.bounded || outer->deadline < scope.deadline)
                scope.deadline = outer->deadline;
            scope.bounded = true;
        }
        scope.id = state.scopesBegun + 1;
        try
        {
            state.scopes.push_back(scope);
        }
        catch (const std::bad_alloc&)
        {
            return FMINUS_WEB_FAILED;
        }
        state.scopesBegun = scope.id;
        holder->open.store(holder->open.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return 0;
    }

    // Counted among the context's waiters from its first look to its last (as fminus_web_http_wait is), so that
    // ~Context waits for it, and wakes it. The scope is found again by its id after each wait: another thread's begin
    // may have moved the scopes meanwhile. Each expiry is queued under the mutex, as Context::stop's cancels are, so a
    // thread closing one of the requests meanwhile frees it only once the expiry has been handled.
    std::int32_t fminus_web_http_within_end(void* context) noexcept
    {
        if (context == nullptr)
            return 0;
        Context::State& state = *static_cast<Context::State*>(context);
        std::unique_lock lock(state.mutex);
        const Context::State::Scope* found = fminus::web::innermost(state);
        if (found == nullptr)
            return FMINUS_WEB_INVALID;
        const Context::State::Scope scope = *found;
        const auto joined = [&](const fminus::web::Request* request)
        {
            return request->scope == scope.id &&
                   request->status.load(std::memory_order_acquire) == FMINUS_WEB_PENDING;
        };
        const auto ready = [&]
        {
            return state.dying || std::none_of(state.requests.begin(), state.requests.end(), joined);
        };
        ++state.waiters;
        if (scope.bounded)
            state.changed.wait_until(lock, scope.deadline, ready);
        else
            state.changed.wait(lock, ready);
        std::int32_t result = 0;
        if (state.dying)
            result = FMINUS_WEB_CANCELLED;   // its requests were cancelled; the scope goes with the context
        else
        {
            std::uint64_t sequence = 0;
            if (state.usesEngine)
            {
                for (fminus::web::Request* request : state.requests)
                {
                    if (joined(request))
                        sequence = fminus::web::engine().postSurely(
                            {fminus::web::Command::Expire, request, &state, 0});
                }
            }
            std::erase_if(state.scopes, [&](const Context::State::Scope& open) { return open.id == scope.id; });
            scope.holder->open.store(scope.holder->open.load(std::memory_order_relaxed) - 1,
                                     std::memory_order_relaxed);
            fminus::web::dropAbandoned(state);
            if (sequence != 0)
            {
                result = FMINUS_WEB_TIMEOUT;
                lock.unlock();
                fminus::web::engine().wait(sequence);
                lock.lock();
            }
            if (state.stopped.load(std::memory_order_acquire) || state.destroying)
                result = FMINUS_WEB_CANCELLED;
        }
        if (--state.waiters == 0 && state.dying)
            state.changed.notify_all();
        return result;
    }

    // Without the mutex: a walk of the records (each published, with its `next`, before the list's head points to it,
    // and never freed before the context) for the calling thread's, and, when it has scopes open, its generation moved
    // on and its count stored 0, in that order: a begin that then takes the record, seeing 0, sees the new generation
    // (holderOf). Only this thread writes either, so no store races another. The scopes stay in the list, abandoned
    // (innermost skips them), until a begin or an end of any thread drops them under the mutex. Its steps are bounded
    // by the records, at most the most threads that had scopes open here at once.
    std::int32_t fminus_web_http_within_abandon(void* context) noexcept
    {
        if (context == nullptr)
            return 0;
        Context::State& state = *static_cast<Context::State*>(context);
        const std::thread::id me = std::this_thread::get_id();
        for (Context::State::Holder* holder = state.holders.load(std::memory_order_acquire); holder != nullptr;
             holder = holder->next)
        {
            if (holder->thread.load(std::memory_order_relaxed) != me)
                continue;
            const std::int32_t open = holder->open.load(std::memory_order_relaxed);
            if (open == 0)
                return 0;
            holder->generation.store(holder->generation.load(std::memory_order_relaxed) + 1,
                                     std::memory_order_relaxed);
            holder->open.store(0, std::memory_order_release);
            return open;
        }
        return 0;
    }

    std::int32_t fminus_web_http_status(const void* handle) noexcept
    {
        const fminus::web::Request* request = made(handle);
        return request == nullptr ? FMINUS_WEB_INVALID : request->status.load(std::memory_order_acquire);
    }

    std::int32_t fminus_web_http_size(const void* handle) noexcept
    {
        const fminus::web::Request* request = made(handle);
#if FMINUS_HAS_WEB_SERVER
        if (request == nullptr)
            return fminus::web::receivedSize(handle);
#endif
        if (request == nullptr || request->status.load(std::memory_order_acquire) <= 0)
            return 0;
        return request->size;
    }

    const void* fminus_web_http_body(const void* handle) noexcept
    {
        const fminus::web::Request* request = made(handle);
#if FMINUS_HAS_WEB_SERVER
        if (request == nullptr)
            return fminus::web::receivedBody(handle);
#endif
        if (request == nullptr || request->status.load(std::memory_order_acquire) <= 0)
            return nullptr;
        return request->body;
    }

    std::int32_t fminus_web_http_progress(const void* handle) noexcept
    {
        const fminus::web::Request* request = made(handle);
        return request == nullptr ? 0 : request->received.load(std::memory_order_relaxed);
    }

    void fminus_web_http_cancel(void* handle) noexcept
    {
        fminus::web::Request* request = made(handle);
        if (request == nullptr || request->context == nullptr ||
            request->status.load(std::memory_order_acquire) != FMINUS_WEB_PENDING)
            return;
        // Waits for the command itself, not only for the status: a request that ends before its Cancel is handled
        // could otherwise be closed and freed with the command still queued.
        fminus::web::Engine& engine = fminus::web::engine();
        engine.wait(engine.postSurely({fminus::web::Command::Cancel, request, request->context, 0}));
    }

    void fminus_web_http_close(void* handle) noexcept
    {
#if FMINUS_HAS_WEB_SERVER
        if (fminus::web::Server* server = fminus::web::asServer(handle))
        {
            if (Context::State* context = fminus::web::serverContext(*server))
                fminus::web::stopServer(*context);
            return;
        }
#endif
        fminus::web::Request* request = made(handle);
        if (request == nullptr || request->context == nullptr)
            return;
        fminus_web_http_cancel(handle);
        Context::State& context = *request->context;
        bool engine = false;
        {
            std::lock_guard lock(context.mutex);
            auto& requests = context.requests;
            requests.erase(std::remove(requests.begin(), requests.end(), request), requests.end());
            engine = context.usesEngine;
        }
        // A cancel Context::stop() queued for it before it left the list may still be queued, if it ended first:
        // every command queued so far is handled before it is freed.
        if (engine)
            fminus::web::engine().wait(fminus::web::engine().posted());
        fminus::web::freeRequest(request);
    }

#if FMINUS_HAS_WEB_SERVER
    // Web.h. The server is made, and its sockets bound, under the context's mutex, so a context listens once and a
    // stop() either comes first (the server never listens) or finds it (and detaches it after its Serve, which is
    // queued under the same mutex).
    void* fminus_web_http_listen(void* context, const char* const* routes, const std::int32_t* lengths,
                                 std::int32_t count) noexcept
    {
        if (context == nullptr)
            return fminus::web::serverHandle(fminus::web::refusedServer());
        Context::State& state = *static_cast<Context::State*>(context);
        try
        {
            std::lock_guard lock(state.mutex);
            if (state.server != nullptr)
                return fminus::web::serverHandle(state.server);
            if (!state.policy.server.listens)
                return fminus::web::serverHandle(fminus::web::refusedServer());
            fminus::web::Server* server =
                fminus::web::makeServer(&state, state.policy, routes, lengths, count,
                                        state.stopped.load(std::memory_order_relaxed));
            state.server = server;
            if (fminus::web::serverListens(*server))
            {
                if (!state.usesEngine)
                {
                    if (fminus::web::engine().acquire())   // the context's mutex, then the engine's lifecycle mutex
                        state.usesEngine = true;
                }
                bool attached = false;
                if (state.usesEngine)
                {
                    fminus::web::Command command{fminus::web::Command::Serve, nullptr, &state, 0};
                    command.server = server;
                    try
                    {
                        fminus::web::engine().post(command);
                        attached = true;
                    }
                    catch (const std::bad_alloc&)
                    {
                    }
                }
                if (!attached)
                    fminus::web::failServer(*server);
            }
            return fminus::web::serverHandle(server);
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }
#endif
}
