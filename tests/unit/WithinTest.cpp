// tests/unit/WithinTest.cpp: `within (milliseconds) { ... }` (src/ir/Within.cpp; docs/web.md, "within"), a deadline
// scope for the web client's requests, and the library's half of it that a stopped call needs
// (fminus_web_http_within_abandon, src/web/Web.h). Programs compiled with fminus::Program, at -O0 and -O2, fetch from a
// small HTTP/1.1 server of this file's own on 127.0.0.1: several requests at once all ended at the closing brace, a
// slow one cancelled at the deadline (HTTP_TIMEOUT) while the others are 200, no deadline, one already passed, a request
// started in a function the block calls, nested blocks, each way out (return, break, continue) after the block's own
// defers, http_wait(r, -1) returning at the deadline, stopWeb() waking a thread at a block's end, a reply sent before
// its route's scope ends and a serve bounded by the deadline (with the server), requests whose URLs have holes, a
// string's among them (recipe 48's _s entry points), and the report's script (with the JSON parser), and a module
// json_read fills from what a block fetched, after the block and inside one. Then a block
// abandoned by the loop budget and by a bounds check, each with a request pending: afterwards the thread waits its own
// time, a new block has its own deadline and its promise, and the program is destroyed with
// nothing left; and a stop on a thread with no scope leaves another thread's scope alone. The library's abandon alone:
// what it abandons and what it leaves, threads racing it, and the proof that it is wait-free (HeapCalls, and seccomp's
// strict mode). Then the compiler's errors, each once at its column. Nothing here binds or connects anywhere but
// 127.0.0.1. In a build without the web library, a within is the error http_wait gives there; the rest is not compiled,
// nor on Windows, whose sockets it is not written for.
#include "Compile.h"
#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

#if FMINUS_HAS_WEB && !defined(_WIN32)
#include "HeapCalls.h"
#include "fminus/FMinus.h"
#include "web/Policy.h"
#include "web/Web.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#endif

using namespace fminus::test;

#if !FMINUS_HAS_WEB
TEST_CASE("a within, in a build without the web library, is the error http_wait gives there", "[codegen][within]")
{
    // Parsed, the one error at the within, and its block lowered for its own errors.
    CHECK(errorsOf("int f(int ms)\n"
                   "{\n"
                   "    int n = 0;\n"
                   "    within (ms)\n"
                   "    {\n"
                   "        n = n + undeclared;\n"
                   "    }\n"
                   "    return n;\n"
                   "}\n")
          == "test.fm:4:5: error: 'within' needs the web library, which this build of FMinus leaves out "
             "(FMINUS_WEB=OFF)\n"
             "test.fm:6:17: error: use of undeclared identifier 'undeclared'\n");
    CHECK(errorsOf("int f(void* r) { return http_wait(r, 10); }")
          == "test.fm:1:25: error: 'http_wait' needs the web library, which this build of FMinus leaves out "
             "(FMINUS_WEB=OFF)\n");
}
#endif

#if FMINUS_HAS_WEB
namespace
{
    bool compiles(const char* source)
    {
        std::string errors;
        const bool compiled = compile(source, &errors) != nullptr;
        INFO(errors);
        CHECK(errors.empty());
        return compiled;
    }
} // namespace

TEST_CASE("a within: its effects, the jumps into it, and its milliseconds, each error once at its column",
          "[codegen][within]")
{
    // It waits where it ends, as http_wait does, and its begin allocates: a nonblocking function refuses it in
    // http_wait's words, a nonallocating one as an allocation, and one that calls a function holding one, with a note.
    CHECK(errorsOf("void f(int ms) [[clang::nonblocking]]\n"
                   "{\n"
                   "    within (ms) { }\n"
                   "}\n")
          == "test.fm:3:5: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'http_wait'\n");
    CHECK(errorsOf("void f(int ms) [[clang::nonallocating]]\n"
                   "{\n"
                   "    within (ms) { }\n"
                   "}\n")
          == "test.fm:3:5: error: function with 'nonallocating' attribute must not allocate or deallocate memory\n");
    CHECK(errorsOf("void g() { within (10) { } }\n"
                   "void f() [[clang::nonallocating]] { g(); }\n"
                   "void h() [[clang::nonblocking]] { g(); }\n")
          == "test.fm:2:37: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'g'\n"
             "test.fm:1:12: note: function cannot be inferred 'nonallocating' because it allocates or deallocates "
             "memory\n"
             "test.fm:3:35: error: function with 'nonblocking' attribute must not call non-'nonblocking' function 'g'\n"
             "test.fm:1:12: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function 'http_wait'\n");

    // A switch may not jump into one, past where its scope begins (its end would end a scope never begun), nor past
    // a defer of its block; a label after it, or in a block of its own, is fine.
    CHECK(errorsOf("int f(int k)\n"
                   "{\n"
                   "    int n = 0;\n"
                   "    switch (k)\n"
                   "    {\n"
                   "    case 0:\n"
                   "        within (10)\n"
                   "        {\n"
                   "            defer n++;\n"
                   "        case 1:\n"
                   "            n = 2;\n"
                   "        }\n"
                   "    case 2:\n"
                   "        n = 3;\n"
                   "    }\n"
                   "    return n;\n"
                   "}\n")
          == "test.fm:10:9: error: cannot jump from switch statement to this case label\n"
             "test.fm:9:13: note: jump bypasses defer statement\n"
             "test.fm:7:9: note: jump enters a 'within' block\n");
    CHECK(compiles("int f(int k) { within (10) { switch (k) { case 1: return 1; default: break; } } return 0; }"));

    // Its milliseconds are an int, converted as an argument is.
    CHECK(errorsOf("void f(float* p) { within (p) { } }")
          == "test.fm:1:28: error: passing 'float *' to parameter of incompatible type 'int'\n");
    CHECK(errorsOf("void f() { within (\"10\") { } }").find("test.fm:1:20: error:") == 0);
    CHECK(compiles("void f() { within (10.5f) { } }"));

    // Inside a parallel region it is the iteration's, or the statement's, own; a return from one is the region's
    // error, as anywhere in it.
    CHECK(compiles("void f(int n) { parallel for (int i = 0; i < n; ++i) { within (10) { if (i > 2) continue; } } }\n"
                   "void g() { parallel { within (10) { } within (20) { } } }\n"));
    CHECK(errorsOf("void f(int n) { parallel for (int i = 0; i < n; ++i) { within (10) { return; } } }")
          == "test.fm:1:70: error: cannot return from a parallel for: its iterations run at once, and none can end the "
             "function\n");
}
#endif

#if FMINUS_HAS_WEB && !defined(_WIN32)
namespace
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    std::chrono::milliseconds since(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    }

    bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds limit = 5s)
    {
        const auto start = Clock::now();
        while (!condition())
        {
            if (Clock::now() - start > limit)
                return false;
            std::this_thread::sleep_for(2ms);
        }
        return true;
    }

    // An HTTP/1.1 server on 127.0.0.1 at an ephemeral port, a thread a connection, each closed after its response:
    //   /ok           200 "hello"
    //   /after/N      200 "late", N milliseconds after the request
    //   /slow         no response: the connection is held until the client closes it (or the server goes)
    //   /pack/K.json  200 {"table":[K,1,2,3]}, 100 milliseconds after the request
    //   anything else 404
    class Origin
    {
    public:
        Origin()
        {
            listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
            REQUIRE(listener_ >= 0);
            sockaddr_in where{};
            where.sin_family = AF_INET;
            where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t length = sizeof where;
            REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&where), sizeof where) == 0);
            REQUIRE(::listen(listener_, 64) == 0);
            REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&where), &length) == 0);
            port_ = ntohs(where.sin_port);
            thread_ = std::thread([this] { run(); });
        }

        ~Origin()
        {
            stop_.store(true);
            thread_.join();
            for (std::thread& handler : handlers_)
                handler.join();
            ::close(listener_);
        }

        Origin(const Origin&) = delete;
        Origin& operator=(const Origin&) = delete;

        int port() const
        {
            return port_;
        }

        int hits(const std::string& path) const
        {
            std::lock_guard lock(mutex_);
            const auto found = hits_.find(path);
            return found == hits_.end() ? 0 : found->second;
        }

        int open() const
        {
            return open_.load();
        }

    private:
        void run()
        {
            while (!stop_.load())
            {
                pollfd one{listener_, POLLIN, 0};
                if (::poll(&one, 1, 20) <= 0)
                    continue;
                const int fd = ::accept(listener_, nullptr, nullptr);
                if (fd < 0)
                    continue;
                ++open_;
                handlers_.emplace_back([this, fd] {
                    handle(fd);
                    ::close(fd);
                    --open_;
                });
            }
        }

        // Whether the client closed `fd` (or the server is going), polling for up to `ms` milliseconds.
        bool closedWithin(int fd, int ms)
        {
            const auto start = Clock::now();
            while (!stop_.load() && since(start).count() < ms)
            {
                pollfd one{fd, POLLIN, 0};
                if (::poll(&one, 1, 5) > 0)
                {
                    char byte;
                    if (::recv(fd, &byte, 1, MSG_PEEK) <= 0)
                        return true;
                    std::this_thread::sleep_for(1ms);
                }
            }
            return stop_.load();
        }

        void handle(int fd)
        {
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos)
            {
                pollfd one{fd, POLLIN, 0};
                if (stop_.load())
                    return;
                if (::poll(&one, 1, 20) <= 0)
                    continue;
                const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
                if (got <= 0)
                    return;
                request.append(buffer, static_cast<std::size_t>(got));
            }
            const std::size_t start = request.find(' ') + 1;
            const std::string path = request.substr(start, request.find(' ', start) - start);
            {
                std::lock_guard lock(mutex_);
                ++hits_[path];
            }
            int status = 200;
            std::string body;
            if (path == "/ok")
                body = "hello";
            else if (path.starts_with("/after/"))
            {
                if (closedWithin(fd, std::stoi(path.substr(7))))
                    return;
                body = "late";
            }
            else if (path == "/slow")
            {
                while (!closedWithin(fd, 1000))
                {
                }
                return;
            }
            else if (path.starts_with("/pack/") && path.ends_with(".json"))
            {
                if (closedWithin(fd, 100))
                    return;
                body = "{\"table\":[" + path.substr(6, path.size() - 11) + ",1,2,3]}";
            }
            else
            {
                status = 404;
                body = "none";
            }
            const std::string response = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found")
                                         + "\r\nContent-Type: text/plain\r\nContent-Length: "
                                         + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            for (std::size_t sent = 0; sent < response.size();)
            {
                const ssize_t put = ::send(fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                if (put <= 0)
                    break;
                sent += static_cast<std::size_t>(put);
            }
        }

        int listener_ = -1;
        int port_ = 0;
        std::atomic<bool> stop_{false};
        std::atomic<int> open_{0};
        mutable std::mutex mutex_;
        std::map<std::string, int> hits_;
        std::vector<std::thread> handlers_;   // the accepting thread's, until it is joined
        std::thread thread_;
    };

    // `source` compiled at -O`level`, allowed every URL of `origin`.
    std::unique_ptr<fminus::Program> compileFor(const Origin& origin, const std::string& source, int level,
                                                fminus::Program::Options options = {})
    {
        options.optimisation = level;
        options.web.allow = {"http://127.0.0.1:" + std::to_string(origin.port()) + "/*"};
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
    }

    template <typename Signature>
    Signature* function(fminus::Program& program, const char* name)
    {
        std::string errors;
        Signature* found = program.function<Signature>(name, errors);
        INFO(errors);
        REQUIRE(found != nullptr);
        return found;
    }

    // A module's instance: its bytes, zeroed and aligned as Program::Module says.
    struct Instance
    {
        explicit Instance(const fminus::Program::Module& module)
            : storage((module.size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) + 1)
        {
            REQUIRE(module.alignment <= alignof(std::max_align_t));
            std::memset(storage.data(), 0, storage.size() * sizeof(std::max_align_t));
        }
        void* memory()
        {
            return storage.data();
        }
        std::vector<std::max_align_t> storage;
    };

    constexpr const char* kStart = "http_response start(int port) { return http_get(\"http://127.0.0.1:%d/slow\", "
                                   "port); }\n";
} // namespace

TEST_CASE("a within's requests have all ended where it ends: at once, and a slow one cancelled at the deadline",
          "[web][within]")
{
    Origin origin;
    const std::string source = "int batch(int port, int count, int slow, int ms, int* statuses)\n"
                               "{\n"
                               "    http_response r[8];\n"
                               "    within (ms)\n"
                               "    {\n"
                               "        for (int k = 0; k < count; ++k)\n"
                               "        {\n"
                               "            if (k == slow)\n"
                               "                r[k] = http_get(\"http://127.0.0.1:%d/slow\", port);\n"
                               "            else\n"
                               "                r[k] = http_get(\"http://127.0.0.1:%d/after/%d\", port, 200);\n"
                               "        }\n"
                               "    }\n"
                               "    int pending = 0;\n"
                               "    for (int k = 0; k < count; ++k)\n"
                               "    {\n"
                               "        statuses[k] = http_status(r[k]);\n"
                               "        if (statuses[k] == HTTP_PENDING)\n"
                               "            pending += 1;\n"
                               "        http_close(r[k]);\n"
                               "    }\n"
                               "    return pending;\n"
                               "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        auto program = compileFor(origin, source, level);
        auto* batch = function<int(int, int, int, int, int*)>(*program, "batch");
        const int port = origin.port();

        // Six at once, each 200 ms: the brace waits for all of them, which together take about one's time.
        int statuses[8] = {};
        auto start = Clock::now();
        CHECK(batch(port, 6, -1, 10000, statuses) == 0);
        CHECK(since(start) >= 190ms);
        CHECK(since(start) < 1000ms);   // one after another would be 1200 ms
        for (int k = 0; k < 6; ++k)
            CHECK(statuses[k] == 200);

        // A slow one among them: cancelled at the deadline, HTTP_TIMEOUT, the others 200; none still pending.
        start = Clock::now();
        CHECK(batch(port, 4, 2, 400, statuses) == 0);
        CHECK(since(start) >= 390ms);
        CHECK(since(start) < 3000ms);
        CHECK(statuses[0] == 200);
        CHECK(statuses[1] == 200);
        CHECK(statuses[2] == FMINUS_WEB_TIMEOUT);
        CHECK(statuses[3] == 200);

        // A negative deadline is none: the brace waits for what the block started.
        start = Clock::now();
        CHECK(batch(port, 2, -1, -1, statuses) == 0);
        CHECK(since(start) >= 190ms);
        CHECK((statuses[0] == 200 && statuses[1] == 200));

        // One already passed: what is still pending at the brace is cancelled there.
        start = Clock::now();
        CHECK(batch(port, 1, 0, 0, statuses) == 0);
        CHECK(since(start) < 1000ms);
        CHECK(statuses[0] == FMINUS_WEB_TIMEOUT);
    }
    CHECK(eventually([&] { return origin.open() == 0; }));
}

TEST_CASE("a request a function the block calls starts is the block's; a wait inside returns at the deadline",
          "[web][within]")
{
    Origin origin;
    const std::string source = std::string(kStart)
                               + "int called(int port)\n"
                                 "{\n"
                                 "    http_response r;\n"
                                 "    within (200) { r = start(port); }\n"
                                 "    const int s = http_status(r);\n"
                                 "    http_close(r);\n"
                                 "    return s;\n"
                                 "}\n"
                                 "int waited(int port, int* seen)\n"
                                 "{\n"
                                 "    http_response r;\n"
                                 "    within (200)\n"
                                 "    {\n"
                                 "        r = start(port);\n"
                                 "        seen[0] = http_wait(r, -1);\n"
                                 "        seen[1] = http_wait(r, 100000);\n"
                                 "    }\n"
                                 "    const int s = http_status(r);\n"
                                 "    http_close(r);\n"
                                 "    return s;\n"
                                 "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        auto program = compileFor(origin, source, level);
        auto* called = function<int(int)>(*program, "called");
        auto* waited = function<int(int, int*)>(*program, "waited");
        auto start = Clock::now();
        CHECK(called(origin.port()) == FMINUS_WEB_TIMEOUT);
        CHECK(since(start) >= 190ms);
        CHECK(since(start) < 3000ms);

        // http_wait(r, -1) returns when the deadline passes, with what it gives on its own timeout: the status, 0
        // while pending; a wait after it returns at once.
        int seen[2] = {-100, -100};
        start = Clock::now();
        CHECK(waited(origin.port(), seen) == FMINUS_WEB_TIMEOUT);
        CHECK(seen[0] == FMINUS_WEB_PENDING);
        CHECK(seen[1] == FMINUS_WEB_PENDING);
        CHECK(since(start) >= 190ms);
        CHECK(since(start) < 3000ms);
    }
}

TEST_CASE("nested withins: an inner one cannot outlast the outer, and a request is the innermost's", "[web][within]")
{
    Origin origin;
    const std::string source = std::string(kStart)
                               + "void nested(int port, int* statuses)\n"
                                 "{\n"
                                 "    http_response a;\n"
                                 "    http_response b;\n"
                                 "    http_response c;\n"
                                 "    within (300)\n"
                                 "    {\n"
                                 "        a = start(port);\n"
                                 "        within (60000)\n"          // ends with the outer's deadline, at 300 ms
                                 "        {\n"
                                 "            b = start(port);\n"
                                 "        }\n"
                                 "        statuses[0] = http_status(a);\n"   // the outer's: still pending
                                 "        statuses[1] = http_status(b);\n"
                                 "        within (100)\n"            // the outer's deadline has passed: at once
                                 "        {\n"
                                 "            c = start(port);\n"
                                 "        }\n"
                                 "        statuses[2] = http_status(c);\n"
                                 "    }\n"
                                 "    statuses[3] = http_status(a);\n"
                                 "    http_close(a);\n"
                                 "    http_close(b);\n"
                                 "    http_close(c);\n"
                                 "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        auto program = compileFor(origin, source, level);
        auto* nested = function<void(int, int*)>(*program, "nested");
        int statuses[4] = {-100, -100, -100, -100};
        const auto start = Clock::now();
        nested(origin.port(), statuses);
        CHECK(since(start) >= 290ms);
        CHECK(since(start) < 3000ms);
        CHECK(statuses[0] == FMINUS_WEB_PENDING);
        CHECK(statuses[1] == FMINUS_WEB_TIMEOUT);
        CHECK(statuses[2] == FMINUS_WEB_TIMEOUT);
        CHECK(statuses[3] == FMINUS_WEB_TIMEOUT);
    }
}

TEST_CASE("each way out of a within ends it, after the block's own deferred statements", "[web][within]")
{
    Origin origin;
    const std::string source = std::string(kStart)
                               + "http_response byReturn(int port, int* seen)\n"
                                 "{\n"
                                 "    within (200)\n"
                                 "    {\n"
                                 "        http_response r = start(port);\n"
                                 "        defer seen[0] = http_status(r);\n"   // before the end: still pending
                                 "        if (port > 0)\n"
                                 "            return r;\n"                     // computed, then the defer, the end
                                 "    }\n"
                                 "    return start(port);\n"
                                 "}\n"
                                 "void byLoop(int port, int* statuses)\n"
                                 "{\n"
                                 "    http_response r[3];\n"
                                 "    for (int i = 0; i < 3; ++i)\n"
                                 "    {\n"
                                 "        within (150)\n"
                                 "        {\n"
                                 "            r[i] = start(port);\n"
                                 "            if (i == 0)\n"
                                 "                continue;\n"
                                 "            if (i == 1)\n"
                                 "                break;\n"
                                 "        }\n"
                                 "        statuses[5] = 1;\n"
                                 "    }\n"
                                 "    for (int i = 0; i < 2; ++i)\n"
                                 "    {\n"
                                 "        statuses[i] = http_status(r[i]);\n"
                                 "        http_close(r[i]);\n"
                                 "    }\n"
                                 "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        auto program = compileFor(origin, source, level);
        auto* byReturn = function<void*(int, int*)>(*program, "byReturn");
        auto* byLoop = function<void(int, int*)>(*program, "byLoop");

        int seen[1] = {-100};
        auto start = Clock::now();
        void* returned = byReturn(origin.port(), seen);
        CHECK(since(start) >= 190ms);
        CHECK(seen[0] == FMINUS_WEB_PENDING);
        CHECK(fminus_web_http_status(returned) == FMINUS_WEB_TIMEOUT);
        fminus_web_http_close(returned);

        // continue, then break: each leaves its own block's scope, ended at its deadline.
        int statuses[6] = {-100, -100, -100, -100, -100, 0};
        start = Clock::now();
        byLoop(origin.port(), statuses);
        CHECK(since(start) >= 290ms);
        CHECK(since(start) < 3000ms);
        CHECK(statuses[0] == FMINUS_WEB_TIMEOUT);
        CHECK(statuses[1] == FMINUS_WEB_TIMEOUT);
        CHECK(statuses[5] == 0);
    }
}

TEST_CASE("stopWeb wakes a thread at a within's end, and destroying the program with one open leaves nothing",
          "[web][within][lifetime]")
{
    Origin origin;
    const std::string source = std::string(kStart)
                               + "int held(int port)\n"
                                 "{\n"
                                 "    http_response r;\n"
                                 "    within (60000) { r = start(port); }\n"
                                 "    const int s = http_status(r);\n"
                                 "    http_close(r);\n"
                                 "    return s;\n"
                                 "}\n";
    auto program = compileFor(origin, source, 2);
    auto* held = function<int(int)>(*program, "held");
    std::atomic<int> result{-100};
    std::thread loader([&] { result = held(origin.port()); });
    REQUIRE(eventually([&] { return origin.hits("/slow") == 1; }));
    std::this_thread::sleep_for(50ms);   // into the end's wait
    const auto start = Clock::now();
    program->stopWeb();
    loader.join();
    CHECK(since(start) < 2000ms);
    CHECK(result.load() == FMINUS_WEB_CANCELLED);
    program.reset();
    CHECK(eventually([&] { return origin.open() == 0; }));
    CHECK_FALSE(fminus::web::ioThreadRunning());
}

#if FMINUS_HAS_WEB_SERVER
TEST_CASE("a reply in a within is sent before its scope ends; a serve in one waits no longer than its deadline",
          "[web][within][serve]")
{
    Origin origin;
    const char* source = "module Front\n"
                         "{\n"
                         "    http_server server;\n"
                         "    http_response pending;\n"
                         "    void init() { server = http_listen(); }\n"
                         "    int port() { return http_port(server); }\n"
                         "    void answer(int origin)\n"
                         "    {\n"
                         "        serve (server, 5000)\n"
                         "        {\n"
                         "            route \"GET /x\"\n"
                         "            {\n"
                         "                within (300)\n"
                         "                {\n"
                         "                    pending = http_get(\"http://127.0.0.1:%d/slow\", origin);\n"
                         "                    reply 200, \"text/plain\", \"%d\", 7;\n"
                         "                }\n"
                         "            }\n"
                         "        }\n"
                         "    }\n"
                         "    void bounded() { within (200) { serve (server, 5000) { route \"GET /never\" { reply 204; "
                         "} } } }\n"
                         "    int status() { return http_status(pending); }\n"
                         "    void end() { http_close(pending); http_close(server); }\n"
                         "};\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        fminus::Program::Options options;
        options.web.server.listen = "127.0.0.1:0";
        auto program = compileFor(origin, source, level, options);
        const fminus::Program::Module* front = program->module("Front");
        REQUIRE(front != nullptr);
        Instance instance(*front);
        function<void(void*)>(*program, "Front::init")(instance.memory());
        const int port = function<int(void*)>(*program, "Front::port")(instance.memory());
        REQUIRE(port > 0);
        auto* answer = function<void(void*, int)>(*program, "Front::answer");

        std::atomic<long long> ended{0};
        const auto start = Clock::now();
        std::thread serving([&] {
            answer(instance.memory(), origin.port());
            ended = since(start).count();
        });
        // The request: its reply comes back while the route's scope still waits for its own request.
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        timeval wait{20, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        const std::string request =
            "GET /x HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
        ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
        std::string reply;
        char buffer[1024];
        for (ssize_t got = 0; (got = ::recv(fd, buffer, sizeof buffer, 0)) > 0;)
            reply.append(buffer, static_cast<std::size_t>(got));
        ::close(fd);
        const long long replied = since(start).count();
        serving.join();
        INFO(reply);
        CHECK(reply.starts_with("HTTP/1.1 200"));
        CHECK(reply.ends_with("\r\n\r\n7"));
        CHECK(replied < ended.load());          // the reply first, then the scope's end
        CHECK(ended.load() >= 290);
        CHECK(function<int(void*)>(*program, "Front::status")(instance.memory()) == FMINUS_WEB_TIMEOUT);

        // A serve's wait is one the deadline bounds: 200 ms, not 5000.
        const auto before = Clock::now();
        function<void(void*)>(*program, "Front::bounded")(instance.memory());
        CHECK(since(before) >= 190ms);
        CHECK(since(before) < 3000ms);
        function<void(void*)>(*program, "Front::end")(instance.memory());
    }
}
#endif

TEST_CASE("a request whose URL has holes, a string's among them, is the within's as any other",
          "[web][within][holes]")
{
    // Where within and recipe 48 meet: a URL with holes (an int's in the port and the path, a string's in the path) is
    // started through the library's _s entry points, which join the thread's scope as http_get and http_post do. The
    // brace waits for them; one held by the server is cancelled at the deadline (HTTP_TIMEOUT). The string from the
    // char array is "slow" only while the request is made: the stores around the call must both stay, at -O2 too.
    Origin origin;
    const char* source = "int fetch(int port, int slow, int ms, int* statuses)\n"
                         "{\n"
                         "    char held[8] = \"xlow\";\n"
                         "    const string late = \"after\";\n"
                         "    const int wait = 200;\n"
                         "    http_response r[3];\n"
                         "    within (ms)\n"
                         "    {\n"
                         "        held[0] = 115;   // 's'\n"
                         "        const string stalled = string_of(held, 4);\n"
                         "        r[0] = http_get(\"http://127.0.0.1:{port}/{late}/{wait}\");\n"
                         "        r[1] = http_post(\"http://127.0.0.1:{port}/{late}/{wait}\", held, 0, \"\");\n"
                         "        if (slow)\n"
                         "            r[2] = http_get(\"http://127.0.0.1:{port}/{stalled}\");\n"
                         "        else\n"
                         "            r[2] = http_get(\"http://127.0.0.1:{port}/{late}/{wait}\");\n"
                         "        held[0] = 120;   // 'x'\n"
                         "    }\n"
                         "    int pending = 0;\n"
                         "    for (int k = 0; k < 3; ++k)\n"
                         "    {\n"
                         "        statuses[k] = http_status(r[k]);\n"
                         "        if (statuses[k] == HTTP_PENDING)\n"
                         "            pending += 1;\n"
                         "        http_close(r[k]);\n"
                         "    }\n"
                         "    return pending;\n"
                         "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        fminus::Program::Options options;
        options.keepIr = fminus::Program::KeepIr::Optimised;
        auto program = compileFor(origin, source, level, options);
        if (level == 2)
        {
            const std::string ir = program->optimisedIr();
            CHECK(ir.find("@fminus_web_http_get_s(") != std::string::npos);
            CHECK(ir.find("@fminus_web_http_post_s(") != std::string::npos);
            CHECK(ir.find("@fminus_web_http_within_end(") != std::string::npos);
        }
        auto* fetch = function<int(int, int, int, int*)>(*program, "fetch");
        const int port = origin.port();
        const int before = origin.hits("/after/200");

        // Three at once, each 200 ms: the brace waits for all of them, which together take about one's time.
        int statuses[3] = {};
        auto start = Clock::now();
        CHECK(fetch(port, 0, 10000, statuses) == 0);
        CHECK(since(start) >= 190ms);
        CHECK(since(start) < 1000ms);
        CHECK((statuses[0] == 200 && statuses[1] == 200 && statuses[2] == 200));
        CHECK(origin.hits("/after/200") == before + 3);

        // The held one, named by the char array's view: cancelled at the deadline, the others 200; none pending.
        start = Clock::now();
        CHECK(fetch(port, 1, 400, statuses) == 0);
        CHECK(since(start) >= 390ms);
        CHECK(since(start) < 3000ms);
        CHECK((statuses[0] == 200 && statuses[1] == 200));
        CHECK(statuses[2] == FMINUS_WEB_TIMEOUT);
        CHECK(origin.hits("/slow") == level / 2 + 1);
        CHECK(origin.hits("/xlow") == 0);
    }
    CHECK(eventually([&] { return origin.open() == 0; }));
}

#if FMINUS_HAS_JSON
TEST_CASE("the report's script: every pack at once in one within, then each read", "[web][within][library]")
{
    Origin origin;
    const char* source = "enum { MOST = 16 };\n"
                         "\n"
                         "float script(int port, int count)\n"
                         "{\n"
                         "    const int packs = count < MOST ? count : MOST;\n"
                         "    http_response r[MOST];\n"
                         "    within (10000)                       // all of them at once; none running after it\n"
                         "    {\n"
                         "        for (int k = 0; k < packs; ++k)\n"
                         "            r[k] = http_get(\"http://127.0.0.1:%d/pack/%d.json\", port, k);\n"
                         "    }\n"
                         "    float total = 0.0f;\n"
                         "    int loaded = 0;\n"
                         "    for (int k = 0; k < packs; ++k)\n"
                         "    {\n"
                         "        defer http_close(r[k]);\n"
                         "        if (http_status(r[k]) != 200)\n"
                         "            continue;\n"
                         "        json_doc doc = json_parse(http_body(r[k]), http_size(r[k]));\n"
                         "        defer json_free(doc);\n"
                         "        float table[4];\n"
                         "        const int n = json_floats(doc, \"/table\", table, 4);\n"
                         "        for (int i = 0; i < n; ++i)\n"
                         "            total += table[i];\n"
                         "        loaded += 1;\n"
                         "    }\n"
                         "    return 1000.0f * (float)loaded + total;\n"
                         "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        auto program = compileFor(origin, source, level);
        auto* script = function<float(int, int)>(*program, "script");
        const auto start = Clock::now();
        // Eight packs, k + 1 + 2 + 3 each: 8000 + 28 + 48. Each takes 100 ms; together, about one's time.
        CHECK(script(origin.port(), 8) == 8000.0f + 28.0f + 48.0f);
        CHECK(since(start) >= 95ms);
        CHECK(since(start) < 700ms);
        CHECK(origin.hits("/pack/7.json") == (level == 0 ? 1 : 2));
    }
}

TEST_CASE("json_read fills a module from what a within fetched: after the block, and inside it through this",
          "[web][within][json-read]")
{
    // Where within and json_read meet: the packs fetched at once in one block, each document read into a module after
    // it, by a json::name, a default and a json::skip; and a method that fetches and fills its own module inside a
    // block, json_read a wait-free read the block has no business with, its request ended at the brace.
    Origin origin;
    const char* source = "module Pack\n"
                         "{\n"
                         "    float values [[json::name(\"table\")]] [4];\n"
                         "    float level [[fminus::default(0.5)]];   // no pack has one\n"
                         "    int kept [[json::skip]];\n"
                         "    int load(int port, int k)\n"
                         "    {\n"
                         "        int status = 0;\n"
                         "        within (5000)\n"
                         "        {\n"
                         "            http_response r = http_get(\"http://127.0.0.1:%d/pack/%d.json\", port, k);\n"
                         "            defer http_close(r);\n"
                         "            http_wait(r, -1);\n"
                         "            status = http_status(r);\n"
                         "            json_doc doc = json_parse(http_body(r), http_size(r));\n"
                         "            defer json_free(doc);\n"
                         "            if (!json_read(doc, \"\", this))\n"
                         "                status = -status;\n"
                         "        }\n"
                         "        return status;\n"
                         "    }\n"
                         "};\n"
                         "\n"
                         "int fill(int port, int count, Pack* packs)\n"
                         "{\n"
                         "    http_response r[8];\n"
                         "    within (10000)\n"
                         "    {\n"
                         "        for (int k = 0; k < count; ++k)\n"
                         "            r[k] = http_get(\"http://127.0.0.1:%d/pack/%d.json\", port, k);\n"
                         "    }\n"
                         "    int filled = 0;\n"
                         "    for (int k = 0; k < count; ++k)\n"
                         "    {\n"
                         "        defer http_close(r[k]);\n"
                         "        if (http_status(r[k]) != 200)\n"
                         "            continue;\n"
                         "        json_doc doc = json_parse(http_body(r[k]), http_size(r[k]));\n"
                         "        defer json_free(doc);\n"
                         "        if (json_read(doc, \"\", &packs[k]))\n"
                         "            filled += 1;\n"
                         "    }\n"
                         "    return filled;\n"
                         "}\n"
                         "int load(Pack* pack, int port, int k) { return pack->load(port, k); }\n";
    struct Pack
    {
        float values[4];
        float level;
        std::int32_t kept;
    };
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        auto program = compileFor(origin, source, level);
        const fminus::Program::Module* module = program->module("Pack");
        REQUIRE(module != nullptr);
        REQUIRE(module->size == sizeof(Pack));
        auto* fill = function<int(int, int, Pack*)>(*program, "fill");
        auto* load = function<int(Pack*, int, int)>(*program, "load");

        Pack packs[4]{};
        for (Pack& pack : packs)
            pack.kept = 7;
        const auto start = Clock::now();
        CHECK(fill(origin.port(), 4, packs) == 4);   // each 100 ms; together, about one's time
        CHECK(since(start) >= 95ms);
        CHECK(since(start) < 700ms);
        for (int k = 0; k < 4; ++k)
        {
            INFO("pack " << k);
            const Pack& pack = packs[k];
            CHECK((pack.values[0] == static_cast<float>(k) && pack.values[1] == 1 && pack.values[2] == 2
                   && pack.values[3] == 3));
            CHECK(pack.level == 0.5f);
            CHECK(pack.kept == 7);
        }

        Pack one{};
        one.level = 9;
        one.kept = 7;
        CHECK(load(&one, origin.port(), 5) == 200);
        CHECK((one.values[0] == 5 && one.values[3] == 3 && one.level == 0.5f && one.kept == 7));
        CHECK(eventually([&] { return origin.open() == 0; }));   // the block's request closed and ended
    }
}
#endif

// ---- a block a stopped call abandons -------------------------------------------------------------------------------

namespace
{
    // `stopped` and `checked` start a request in a within whose deadline (5 ms) has passed by the time anything else
    // runs, then stop inside the block: the loop budget, an index outside its array. Then the same thread waits
    // outside any scope, begins a new one, and the program goes.
    const std::string kAbandoned = std::string(kStart)
                                   + "float spin(float x) { while (x >= 1.0f) x = x - 1.0f; return x; }\n"
                                     "float stopped(int port, float x)\n"
                                     "{\n"
                                     "    float y = 0.0f;\n"
                                     "    within (5)\n"
                                     "    {\n"
                                     "        http_response r = start(port);\n"
                                     "        y = spin(x) + (float)http_status(r);\n"
                                     "    }\n"
                                     "    return y;\n"
                                     "}\n"
                                     "float checked(int port, int i)\n"
                                     "{\n"
                                     "    float table[4] = {1, 2, 3, 4};\n"
                                     "    float y = 0.0f;\n"
                                     "    within (5)\n"
                                     "    {\n"
                                     "        http_response r = start(port);\n"
                                     "        y = table[i] + (float)http_status(r);\n"
                                     "    }\n"
                                     "    return y;\n"
                                     "}\n"
                                     "int waitFor(http_response r, int ms) { return http_wait(r, ms); }\n"
                                     "void finish(http_response r) { http_close(r); }\n"
                                     "int fresh(int port)\n"
                                     "{\n"
                                     "    http_response r;\n"
                                     "    within (150) { r = start(port); }\n"
                                     "    const int s = http_status(r);\n"
                                     "    http_close(r);\n"
                                     "    return s;\n"
                                     "}\n";

    // After a stopped call on this thread: a wait outside any scope waits its own 50 ms, and a new within has its own
    // deadline (150 ms, not the abandoned one's 5, long past) and its promise (its request ended at its brace).
    void checkNoScopeLeft(fminus::Program& program, const Origin& origin)
    {
        auto* start = function<void*(int)>(program, "start");
        auto* waitFor = function<int(void*, int)>(program, "waitFor");
        auto* finish = function<void(void*)>(program, "finish");
        auto* fresh = function<int(int)>(program, "fresh");
        void* request = start(origin.port());
        auto begun = Clock::now();
        CHECK(waitFor(request, 50) == FMINUS_WEB_PENDING);
        CHECK(since(begun) >= 45ms);
        finish(request);
        begun = Clock::now();
        CHECK(fresh(origin.port()) == FMINUS_WEB_TIMEOUT);
        CHECK(since(begun) >= 140ms);
        CHECK(since(begun) < 3000ms);
    }
} // namespace

TEST_CASE("a within the loop budget stops leaves its thread no scope, and nothing when the program goes",
          "[web][within][budget]")
{
    Origin origin;
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        fminus::Program::Options options;
        options.loopBudget = true;
        options.keepIr = fminus::Program::KeepIr::Optimised;
        auto program = compileFor(origin, kAbandoned, level, options);
        CHECK(program->optimisedIr().find("@fminus_web_http_within_abandon") != std::string::npos);
        auto* stopped = function<float(int, float)>(*program, "stopped");
        const int before = origin.hits("/slow");
        program->setLoopBudget(10000);
        CHECK(stopped(origin.port(), INFINITY) == 0.0f);
        CHECK(program->loopBudgetExceeded());
        CHECK(eventually([&] { return origin.hits("/slow") == before + 1; }));   // started in the block, pending
        std::this_thread::sleep_for(20ms);            // past the abandoned block's deadline
        program->setLoopBudget(1'000'000'000);
        checkNoScopeLeft(*program, origin);
        program.reset();                              // the abandoned block's request, never closed, goes with it
        CHECK(eventually([&] { return origin.open() == 0; }));
        CHECK_FALSE(fminus::web::ioThreadRunning());
    }
}

TEST_CASE("a within a bounds check stops leaves its thread no scope, and nothing when the program goes",
          "[web][within][bounds]")
{
    Origin origin;
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        fminus::Program::Options options;
        options.boundsCheck = true;
        options.keepIr = fminus::Program::KeepIr::Optimised;
        auto program = compileFor(origin, kAbandoned, level, options);
        CHECK(program->optimisedIr().find("@fminus_web_http_within_abandon") != std::string::npos);
        auto* checked = function<float(int, int)>(*program, "checked");
        const int before = origin.hits("/slow");
        CHECK(checked(origin.port(), 2) == 3.0f);   // in bounds: as written (its request pending in the block)
        CHECK(checked(origin.port(), 9) == 0.0f);
        REQUIRE(program->boundsViolation());
        CHECK(program->boundsViolation()->index == 9);
        CHECK(eventually([&] { return origin.hits("/slow") >= before + 1; }));   // the stopped one's, pending
        program->clearBoundsViolation();
        std::this_thread::sleep_for(20ms);
        checkNoScopeLeft(*program, origin);
        program.reset();
        CHECK(eventually([&] { return origin.open() == 0; }));
        CHECK_FALSE(fminus::web::ioThreadRunning());
    }
}

TEST_CASE("a stop on a thread with no scope open leaves another thread's scope alone; a program without a within "
          "has no abandon",
          "[web][within][budget]")
{
    Origin origin;
    const std::string source = std::string(kStart)
                               + "float spin(float x) { while (x >= 1.0f) x = x - 1.0f; return x; }\n"
                                 "int held(int port, int ms)\n"
                                 "{\n"
                                 "    http_response r;\n"
                                 "    int seen = 0;\n"
                                 "    within (ms)\n"
                                 "    {\n"
                                 "        r = start(port);\n"
                                 "        seen = http_wait(r, -1);\n"
                                 "    }\n"
                                 "    const int s = http_status(r);\n"
                                 "    http_close(r);\n"
                                 "    return 1000 * seen + s;\n"
                                 "}\n"
                                 "float stopsFirst(int port, float x)\n"   // stopped before its block begins
                                 "{\n"
                                 "    const float y = spin(x);\n"
                                 "    within (10) { http_close(start(port)); }\n"
                                 "    return y;\n"
                                 "}\n";
    for (const int level : {0, 2})
    {
        INFO("-O" << level);
        fminus::Program::Options options;
        options.loopBudget = true;
        auto program = compileFor(origin, source, level, options);
        auto* held = function<int(int, int)>(*program, "held");
        auto* stopsFirst = function<float(int, float)>(*program, "stopsFirst");
        program->setLoopBudget(1'000'000'000);
        const int before = origin.hits("/slow");
        std::atomic<int> result{-100};
        std::atomic<long long> took{0};
        std::thread other([&] {
            const auto start = Clock::now();
            result = held(origin.port(), 400);
            took = since(start).count();
        });
        REQUIRE(eventually([&] { return origin.hits("/slow") == before + 1; }));
        std::this_thread::sleep_for(50ms);   // into its wait
        program->setLoopBudget(10000);
        CHECK(stopsFirst(origin.port(), INFINITY) == 0.0f);   // abandons this thread's scopes: none
        CHECK(program->loopBudgetExceeded());
        other.join();
        // Its wait lasted to its own deadline, and its end ended its request: had its scope gone, the end would have
        // found none and left the request pending.
        CHECK(result.load() == FMINUS_WEB_TIMEOUT);
        CHECK(took.load() >= 390);
        CHECK(took.load() < 3000);
    }

    // With both passes, a program without a within: no abandon anywhere (and the existing lit tests pin its IR).
    fminus::Program::Options options;
    options.loopBudget = true;
    options.boundsCheck = true;
    options.keepIr = fminus::Program::KeepIr::Optimised;
    auto plain = compileFor(origin,
                            std::string(kStart) + "float spin(float x) { while (x >= 1.0f) x = x - 1.0f; return x; }\n"
                                                  "float f(int port, float x, int i) { float t[2] = {1, 2}; "
                                                  "http_response r = start(port); http_close(r); return spin(x) + "
                                                  "t[i]; }\n",
                            2, options);
    CHECK(plain->optimisedIr().find("within") == std::string::npos);
}

// ---- the library's abandon -----------------------------------------------------------------------------------------

namespace
{
    std::unique_ptr<fminus::web::Context> loopbackContext(const Origin& origin)
    {
        fminus::web::Policy policy;
        policy.allow = {"http://127.0.0.1:" + std::to_string(origin.port()) + "/*"};
        std::string error;
        auto context = fminus::web::Context::create(policy, error);
        INFO(error);
        REQUIRE(context);
        return context;
    }

    void* get(fminus::web::Context& context, const Origin& origin, const std::string& path)
    {
        const std::string url = "http://127.0.0.1:" + std::to_string(origin.port()) + path;
        return fminus_web_http_get(context.handle(), url.data(), static_cast<std::int32_t>(url.size()), nullptr, 0);
    }
} // namespace

TEST_CASE("the library's abandon: the thread's scopes, all of them, and nothing else", "[web][within]")
{
    Origin origin;
    auto context = loopbackContext(origin);
    void* handle = context->handle();
    CHECK(fminus_web_http_within_abandon(nullptr) == 0);
    CHECK(fminus_web_http_within_abandon(handle) == 0);   // no scope ever begun

    // Two nested, a request in each: both abandoned at once, without waiting; the end then finds none.
    REQUIRE(fminus_web_http_within_begin(handle, 20) == 0);
    void* outer = get(*context, origin, "/after/150");
    REQUIRE(fminus_web_http_within_begin(handle, 5000) == 0);
    void* inner = get(*context, origin, "/slow");
    auto start = Clock::now();
    CHECK(fminus_web_http_within_abandon(handle) == 2);
    CHECK(since(start) < 100ms);
    CHECK(fminus_web_http_within_abandon(handle) == 0);
    CHECK(fminus_web_http_within_end(handle) == FMINUS_WEB_INVALID);
    std::this_thread::sleep_for(30ms);   // past the outer deadline

    // Their requests go on as any other: the late one completes, not timed out at the abandoned deadline; a wait on
    // the slow one takes its own time.
    CHECK(fminus_web_http_wait(outer, 3000) == 200);
    start = Clock::now();
    CHECK(fminus_web_http_wait(inner, 50) == FMINUS_WEB_PENDING);
    CHECK(since(start) >= 45ms);

    // A new scope has its own deadline, ends only its own requests, and an abandon after it abandons it alone.
    REQUIRE(fminus_web_http_within_begin(handle, 100) == 0);
    void* fresh = get(*context, origin, "/slow");
    start = Clock::now();
    CHECK(fminus_web_http_within_end(handle) == FMINUS_WEB_TIMEOUT);
    CHECK(since(start) >= 90ms);
    CHECK(fminus_web_http_status(fresh) == FMINUS_WEB_TIMEOUT);
    CHECK(fminus_web_http_status(inner) == FMINUS_WEB_PENDING);
    REQUIRE(fminus_web_http_within_begin(handle, -1) == 0);
    CHECK(fminus_web_http_within_abandon(handle) == 1);

    // Another thread's scope is left alone: its wait lasts to its deadline, and its end ends its request.
    std::atomic<int> ended{-100};
    std::atomic<int> status{-100};
    std::atomic<bool> waiting{false};
    std::atomic<long long> took{0};
    std::thread other([&] {
        const auto begun = Clock::now();
        fminus_web_http_within_begin(handle, 300);
        void* request = get(*context, origin, "/slow");
        waiting = true;
        fminus_web_http_wait(request, -1);
        ended = fminus_web_http_within_end(handle);
        took = since(begun).count();
        status = fminus_web_http_status(request);
        fminus_web_http_close(request);
    });
    REQUIRE(eventually([&] { return waiting.load(); }));
    CHECK(fminus_web_http_within_abandon(handle) == 0);   // this thread has none
    REQUIRE(fminus_web_http_within_begin(handle, 5000) == 0);
    CHECK(fminus_web_http_within_abandon(handle) == 1);   // its own only
    other.join();
    CHECK(ended.load() == FMINUS_WEB_TIMEOUT);
    CHECK(status.load() == FMINUS_WEB_TIMEOUT);
    CHECK(took.load() >= 290);

    // Threads come and go, each abandoning or ending what it began; the records are reused, and a later thread that
    // the system may give a gone one's id finds no scope of the gone one's.
    for (int round = 0; round < 8; ++round)
    {
        std::thread([&, round] {
            REQUIRE(fminus_web_http_within_begin(handle, 1) == 0);
            if (round % 2 == 0)
            {
                CHECK(fminus_web_http_within_abandon(handle) == 1);
                return;
            }
            std::this_thread::sleep_for(5ms);
            void* request = get(*context, origin, "/slow");
            const auto begun = Clock::now();
            CHECK(fminus_web_http_wait(request, 30) == FMINUS_WEB_PENDING);
            CHECK(since(begun) < 25ms);    // its own block's deadline, passed
            CHECK(fminus_web_http_within_end(handle) == FMINUS_WEB_TIMEOUT);
            fminus_web_http_close(request);
        }).join();
        std::thread([&] {
            void* request = get(*context, origin, "/slow");
            const auto begun = Clock::now();
            CHECK(fminus_web_http_wait(request, 30) == FMINUS_WEB_PENDING);
            CHECK(since(begun) >= 25ms);   // no scope of the thread before it
            CHECK(fminus_web_http_within_end(handle) == FMINUS_WEB_INVALID);
            fminus_web_http_close(request);
        }).join();
    }
    fminus_web_http_close(outer);
    fminus_web_http_close(inner);
    fminus_web_http_close(fresh);
}

TEST_CASE("threads beginning, ending and abandoning scopes at once under one context", "[web][within]")
{
    Origin origin;
    auto context = loopbackContext(origin);
    void* handle = context->handle();
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 6; ++t)
    {
        threads.emplace_back([&, t] {
            std::mt19937 random(static_cast<unsigned>(1000 + t));
            int open = 0;
            for (int step = 0; step < 400; ++step)
            {
                switch (random() % 4)
                {
                case 0:
                case 1:
                    if (fminus_web_http_within_begin(handle, static_cast<std::int32_t>(random() % 3)) == 0)
                        ++open;
                    else
                        ++wrong;
                    break;
                case 2:
                    if (fminus_web_http_within_end(handle) != (open > 0 ? 0 : FMINUS_WEB_INVALID))
                        ++wrong;
                    open = open > 0 ? open - 1 : 0;
                    break;
                default:
                    if (fminus_web_http_within_abandon(handle) != open)
                        ++wrong;
                    open = 0;
                    break;
                }
            }
            if (fminus_web_http_within_abandon(handle) != open)
                ++wrong;
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    CHECK(wrong.load() == 0);
    // And with requests among them, which every thread's scopes and abandons leave as they must.
    threads.clear();
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&, t] {
            for (int round = 0; round < 10; ++round)
            {
                REQUIRE(fminus_web_http_within_begin(handle, 30) == 0);
                void* request = get(*context, origin, round % 2 == 0 ? "/after/5" : "/slow");
                if ((round + t) % 3 == 0)
                {
                    CHECK(fminus_web_http_within_abandon(handle) == 1);
                    fminus_web_http_close(request);
                    continue;
                }
                const std::int32_t ended = fminus_web_http_within_end(handle);
                const std::int32_t status = fminus_web_http_status(request);
                CHECK((ended == 0 || ended == FMINUS_WEB_TIMEOUT));
                CHECK((status == 200 || status == FMINUS_WEB_TIMEOUT));
                fminus_web_http_close(request);
            }
        });
    }
    for (std::thread& thread : threads)
        thread.join();
}

TEST_CASE("the library's abandon is wait-free: it allocates nothing and makes no system call", "[web][within]")
{
    fminus::web::Policy policy;
    std::string error;
    auto context = fminus::web::Context::create(policy, error);
    REQUIRE(context);
    void* handle = context->handle();
    auto empty = fminus::web::Context::create(policy, error);
    REQUIRE(empty);

    // Another thread holds a scope open throughout; this one has two of its own.
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        fminus_web_http_within_begin(handle, -1);
        holding = true;
        while (!release.load())
            std::this_thread::sleep_for(1ms);
        fminus_web_http_within_end(handle);
    });
    REQUIRE(eventually([&] { return holding.load(); }));
    const auto abandonAll = [&] {
        std::int32_t results[4] = {
            fminus_web_http_within_abandon(empty->handle()),   // no record at all
            fminus_web_http_within_abandon(handle),            // two of this thread's
            fminus_web_http_within_abandon(handle),            // none left: another thread's record walked past
            fminus_web_http_within_abandon(nullptr),
        };
        return results[0] * 1000 + results[1] * 100 + results[2] * 10 + results[3];
    };

    if (fminus::test::HeapCalls::counted())
    {
        REQUIRE(fminus_web_http_within_begin(handle, 1000) == 0);
        REQUIRE(fminus_web_http_within_begin(handle, -1) == 0);
        long calls = -1;
        int result = -1;
        {
            const fminus::test::HeapCalls counting;
            result = abandonAll();
            calls = counting.count();
        }
        CHECK(calls == 0);
        CHECK(result == 200);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");

#if defined(__linux__)
    REQUIRE(fminus_web_http_within_begin(handle, 1000) == 0);
    REQUIRE(fminus_web_http_within_begin(handle, -1) == 0);
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::close(fds[0]);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
            ::syscall(SYS_exit, 2);
        const int result = abandonAll();
        const ssize_t written = ::write(fds[1], &result, sizeof result);
        ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
    }
    ::close(fds[1]);
    int status = 0;
    ::waitpid(child, &status, 0);
    int result = -1;
    const ssize_t got = ::read(fds[0], &result, sizeof result);
    ::close(fds[0]);
    INFO("status " << status);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        WARN("seccomp's strict mode is not available here: the system calls are not checked");
    else
    {
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        REQUIRE(got == static_cast<ssize_t>(sizeof result));
        CHECK(result == 200);
    }
    CHECK(fminus_web_http_within_abandon(handle) == 2);   // the parent's own, which the child's did not touch
#else
    WARN("seccomp is Linux's: the system calls are not checked here");
#endif
    release = true;
    holder.join();
}
#endif
