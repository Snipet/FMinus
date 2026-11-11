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
#include "web/Web.h"

#include "fminus/Config.h"
#include "runtime/Format.h"
#include "web/Policy.h"
#include "web/Roots.h"

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
    };

    namespace
    {
        struct Request
        {
            // Published (Web.h): the status last.
            std::atomic<std::int32_t> status{FMINUS_WEB_PENDING};
            std::atomic<std::int32_t> received{0};
            char* body = nullptr;
            std::int32_t size = 0;

            Context::State* context = nullptr;   // null: the shared refused request

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
                Forget,
            } kind;
            Request* request = nullptr;
            Context::State* context = nullptr;
            std::uint64_t sequence = 0;
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
                curl_multi_poll(multi_, nullptr, 0, 1000, nullptr);
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
            case Command::Forget:
                if (command.context->share != nullptr)
                {
                    curl_share_cleanup(command.context->share);
                    command.context->share = nullptr;
                }
                break;
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

        // A request under `context`, checked, and started unless the check or a limit ended it.
        Request* start(Context::State* context, std::string url, bool post, const void* data, std::int32_t bytes,
                       std::string_view type)
        {
            Request* request = new (std::nothrow) Request;
            if (request == nullptr)
                return nullptr;
            request->context = context;
            const Policy& policy = context->policy.policy;
            std::int32_t status = checkUrl(context->policy, url, request->target);
            if (status == 0 && post)
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
    } // namespace

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
        if (state.usesEngine)
            engine().release();
        curl_global_cleanup();
    }

    // The flag first, under the mutex: a start that takes the mutex after it is cancelled there, one before it is in
    // the list, and its Start command, if it is queued after the cancels below, finds the flag as its hop begins. Then
    // a cancel of each request still pending, one at a time, each waited for. Each is queued under the mutex, so that
    // a thread closing that request meanwhile (close removes it under the mutex) queues its own cancel after it, and
    // frees the request only once both have been handled.
    void Context::stop() noexcept
    {
        State& state = *state_;
        {
            std::lock_guard lock(state.mutex);
            state.stopped.store(true, std::memory_order_release);
            if (!state.usesEngine)
                return;
        }
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

extern "C"
{
    void* fminus_web_http_get(void* context, const char* url, std::int32_t length, const std::int32_t* ints,
                              std::int32_t count) noexcept
    {
        if (context == nullptr)
            return &fminus::web::refused();
        try
        {
            return fminus::web::start(static_cast<Context::State*>(context),
                                      fminus::web::expand(url, length, ints, count), false, nullptr, 0, {});
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
            return &fminus::web::refused();
        try
        {
            const std::string_view contentType = type == nullptr || typeLength <= 0
                                                     ? std::string_view()
                                                     : std::string_view(type, static_cast<std::size_t>(typeLength));
            return fminus::web::start(static_cast<Context::State*>(context),
                                      fminus::web::expand(url, length, ints, count), true, data, bytes, contentType);
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    std::int32_t fminus_web_http_wait(void* handle, std::int32_t timeoutMs) noexcept
    {
        auto* request = static_cast<fminus::web::Request*>(handle);
        if (request == nullptr)
            return FMINUS_WEB_INVALID;
        const std::int32_t now = request->status.load(std::memory_order_acquire);
        if (now != FMINUS_WEB_PENDING || timeoutMs == 0 || request->context == nullptr)
            return now;
        Context::State& context = *request->context;
        std::unique_lock lock(context.mutex);
        ++context.waiters;
        const auto ready = [&]
        {
            return context.dying || request->status.load(std::memory_order_acquire) != FMINUS_WEB_PENDING;
        };
        if (timeoutMs < 0)
            context.changed.wait(lock, ready);
        else
            context.changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
        const std::int32_t status = request->status.load(std::memory_order_acquire);
        if (--context.waiters == 0 && context.dying)
            context.changed.notify_all();
        return status;
    }

    std::int32_t fminus_web_http_status(const void* handle) noexcept
    {
        const auto* request = static_cast<const fminus::web::Request*>(handle);
        return request == nullptr ? FMINUS_WEB_INVALID : request->status.load(std::memory_order_acquire);
    }

    std::int32_t fminus_web_http_size(const void* handle) noexcept
    {
        const auto* request = static_cast<const fminus::web::Request*>(handle);
        if (request == nullptr || request->status.load(std::memory_order_acquire) <= 0)
            return 0;
        return request->size;
    }

    const void* fminus_web_http_body(const void* handle) noexcept
    {
        const auto* request = static_cast<const fminus::web::Request*>(handle);
        if (request == nullptr || request->status.load(std::memory_order_acquire) <= 0)
            return nullptr;
        return request->body;
    }

    std::int32_t fminus_web_http_progress(const void* handle) noexcept
    {
        const auto* request = static_cast<const fminus::web::Request*>(handle);
        return request == nullptr ? 0 : request->received.load(std::memory_order_relaxed);
    }

    void fminus_web_http_cancel(void* handle) noexcept
    {
        auto* request = static_cast<fminus::web::Request*>(handle);
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
        auto* request = static_cast<fminus::web::Request*>(handle);
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
}
