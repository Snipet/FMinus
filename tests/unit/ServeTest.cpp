// tests/unit/ServeTest.cpp: serve, route and reply, and the JSON literal a reply writes (src/ir/Serve.cpp; docs/web.md,
// "The server"). Programs compiled with fminus::Program listen on loopback and are driven with raw HTTP requests from
// this process: each kind of route parameter filled from the path and the query, each way a request does not fit
// (404 from the library, 400 from the route), each form of reply, the JSON literal's text, a reply the library refuses
// answered 500, two serves on two threads each taking its own routes' requests only, webRoutes() with the parameters,
// and the wait-free proof of a serve in a [[clang::nonblocking]] method (HeapCalls, seccomp's strict mode). Then the
// compiler's errors, each at its column, and the missing reply on each shape of control flow the missing return's tests
// cover; and where a serve meets string, char and when (a route's when, a switch on a string in a route, their errors);
// and where it meets json_read and holes (a route's string parameters, its body read into a module, stored by a file
// name with a hole in a directory the test makes beside bin/, with the JSON parser and file writing).
// Nothing here binds or connects anywhere but 127.0.0.1. In a build without the web server, a serve is the
// error that names the option; the rest is not compiled, nor on Windows, whose sockets it is not written for.
#include "Compile.h"
#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)
#include "HeapCalls.h"
#include "fminus/FMinus.h"
#include "web/Web.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
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

#if !FMINUS_HAS_WEB_SERVER
TEST_CASE("a serve, in a build without the web server, is the error its builtins give there", "[codegen][serve]")
{
    CHECK(errorsOf("void f(void* s)\n"
                   "{\n"
                   "    serve (s) { route \"GET /a\" { reply 204; } }\n"
                   "}\n")
          == "test.fm:3:5: error: 'serve' needs the web server, which this build of FMinus leaves out "
             "(FMINUS_WEB_SERVER=OFF)\n");
    CHECK(errorsOf("void f() { reply 204; }")
          == "test.fm:1:12: error: 'reply' statement not in a route\n");
}
#endif

#if FMINUS_HAS_WEB_SERVER && !FMINUS_HAS_JSON
TEST_CASE("a JSON literal, in a build without the JSON writer, is the error that names the option", "[codegen][serve]")
{
    CHECK(errorsOf("void f(http_server s)\n"
                   "{\n"
                   "    serve (s) { route \"GET /a\" { reply 200, json { \"a\": 1 }; } }\n"
                   "}\n")
          == "test.fm:3:40: error: a JSON literal needs the JSON writer, which this build of FMinus leaves out "
             "(FMINUS_JSON=OFF)\n");
}
#endif

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)
namespace
{
    std::unique_ptr<fminus::Program> compileProgram(const char* source, const fminus::Program::Options& options)
    {
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
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
        void* memory() { return storage.data(); }
        std::vector<std::max_align_t> storage;
    };

    // A connection to 127.0.0.1:`port`, the request sent; -1 when it could not connect.
    int sendRequest(int port, const std::string& method, const std::string& target, const std::string& body)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        timeval wait{20, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
        {
            ::close(fd);
            return -1;
        }
        const std::string text = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
                                 + "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size())
                                 + "\r\n\r\n" + body;
        ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
        return fd;
    }

    // What came back on `fd`, closed: its status (0 when none) and its body.
    struct Response
    {
        int status = 0;
        std::string body;
        std::string head;
    };

    Response readResponse(int fd)
    {
        Response response;
        if (fd < 0)
            return response;
        std::string all;
        char buffer[4096];
        for (;;)
        {
            const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
            if (got <= 0)
                break;
            all.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(fd);
        const std::size_t end = all.find("\r\n\r\n");
        if (all.size() < 12 || end == std::string::npos)
            return response;
        response.status = std::stoi(all.substr(9, 3));
        response.head = all.substr(0, end);
        response.body = all.substr(end + 4);
        return response;
    }

    Response request(int port, const std::string& method, const std::string& target, const std::string& body = "")
    {
        return readResponse(sendRequest(port, method, target, body));
    }

    // The same, with header fields of its own before the end of the head ("Name: value\r\n" each).
    Response requestWith(int port, const std::string& method, const std::string& target, const std::string& fields)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return {};
        timeval wait{20, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
        {
            ::close(fd);
            return {};
        }
        const std::string text = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
                                 + "\r\nConnection: close\r\n" + fields + "Content-Length: 0\r\n\r\n";
        ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
        return readResponse(fd);
    }

    // The host's audio thread: `process` called about once a millisecond until it is destroyed.
    class Pump
    {
    public:
        explicit Pump(std::function<void()> process)
            : thread_([this, process = std::move(process)] {
                  while (!stop_.load())
                  {
                      process();
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  }
              })
        {
        }
        ~Pump()
        {
            stop_.store(true);
            thread_.join();
        }
        Pump(const Pump&) = delete;
        Pump& operator=(const Pump&) = delete;

    private:
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };

    constexpr const char* kService =
        "module Service\n"
        "{\n"
        "    http_server server;\n"
        "    float gain;\n"
        "    int note;\n"
        "    int count;\n"
        "    bool lit;\n"
        "    int deferred;\n"
        "    float table[2];\n"
        "    void init() { server = http_listen(); table[0] = 1.5f; table[1] = -2.0f; }\n"
        "    int port() { return http_port(server); }\n"
        "    void process() [[clang::nonblocking]]\n"
        "    {\n"
        "        serve (server)\n"
        "        {\n"
        "            route \"POST /gain\" (float value [[fminus::range(0, 1)]]) { gain = value; reply 204; }\n"
        "            route \"POST /note/{n}\" (int n [[fminus::range(0, 127)]]) { note = n; reply 204; }\n"
        "            route \"POST /count\" (int n [[fminus::default(7)]]) { count = n; reply 200, \"text/plain\", "
        "\"%d\", n; }\n"
        "            route \"POST /switch\" (const bool on [[fminus::default(1)]]) { lit = on; reply 200, "
        "\"text/plain\", \"%d\", on ? 1 : 0; }\n"
        "            route \"POST /body\" (http_request r) { reply 200, \"text/plain\", \"%d bytes\", http_size(r); }\n"
        "            route \"GET /table\" { reply 200, table, 8, \"application/octet-stream\"; }\n"
        "            route \"GET /a/{x}/b{y}\" (int y, int x) { reply 200, \"text/plain\", \"%d-%d\", x, y; }\n"
        "            route \"GET /a/1/b{y}\" (int y) { reply 200, \"text/plain\", \"one %d\", y; }\n"
        "            route \"GET /deferred\"\n"
        "            {\n"
        "                defer deferred += 1;\n"
        "                for (int i = 0; i < 3; ++i)\n"
        "                    if (i == 1)\n"
        "                        reply 200, \"text/plain\", \"%d\", deferred;\n"
        "                reply 500;\n"
        "            }\n"
        "            route \"GET /status/{code}\" (int code) { reply code; }\n"
        "        }\n"
        "    }\n"
        "};\n";

    // A program's port, its process run by a Pump: what the tests below drive.
    struct Served
    {
        std::unique_ptr<fminus::Program> program;
        std::unique_ptr<Instance> instance;
        int port = 0;
        void (*process)(void*) = nullptr;
    };

    Served serveProgram(const char* source, const char* module, fminus::Program::Options options = {})
    {
        options.web.server.listen = "127.0.0.1:0";
        Served served;
        served.program = compileProgram(source, options);
        std::string errors;
        const std::string name(module);
        auto* init = served.program->function<void(void*)>(name + "::init", errors);
        auto* port = served.program->function<int(void*)>(name + "::port", errors);
        served.process = served.program->function<void(void*)>(name + "::process", errors);
        INFO(errors);
        REQUIRE(init);
        REQUIRE(port);
        REQUIRE(served.process);
        const fminus::Program::Module* layout = served.program->module(name);
        REQUIRE(layout != nullptr);
        served.instance = std::make_unique<Instance>(*layout);
        init(served.instance->memory());
        served.port = port(served.instance->memory());
        REQUIRE(served.port > 0);
        return served;
    }

    template <typename T>
    T field(const fminus::Program& program, Instance& instance, const char* module, const char* name)
    {
        for (const fminus::Program::Module::Field& field : program.module(module)->fields)
        {
            if (field.name == name)
            {
                T value;
                std::memcpy(&value, static_cast<char*>(instance.memory()) + field.offset, sizeof value);
                return value;
            }
        }
        FAIL("no field " << name);
        return T{};
    }
} // namespace

TEST_CASE("a serve's routes take their parameters from the path and the query: what does not fit is 404 or 400",
          "[serve][web][server]")
{
    Served served = serveProgram(kService, "Service");
    const Pump pump([&] { served.process(served.instance->memory()); });
    const int port = served.port;
    const auto gain = [&] { return field<float>(*served.program, *served.instance, "Service", "gain"); };

    // A float from the query, in its range.
    CHECK(request(port, "POST", "/gain?value=0.5").status == 204);
    CHECK(gain() == 0.5f);
    CHECK(request(port, "POST", "/gain?value=1").status == 204);
    CHECK(gain() == 1.0f);
    CHECK(request(port, "POST", "/gain?other=1&value=0.25").status == 204);
    CHECK(gain() == 0.25f);
    // Missing, not a number, a NaN, outside its range: 400, and the block does not run.
    for (const char* target : {"/gain", "/gain?value=", "/gain?value=abc", "/gain?value=nan", "/gain?value=inf",
                               "/gain?value=1.5", "/gain?value=-0.1", "/gain?value=0.5x"})
    {
        INFO(target);
        CHECK(request(port, "POST", target).status == 400);
    }
    CHECK(gain() == 0.25f);

    // An int from a hole of the path: a number that is no int's does not match the route (the library's 404).
    CHECK(request(port, "POST", "/note/69").status == 204);
    CHECK(field<int>(*served.program, *served.instance, "Service", "note") == 69);
    CHECK(request(port, "POST", "/note/0127").status == 204);
    CHECK(field<int>(*served.program, *served.instance, "Service", "note") == 127);
    CHECK(request(port, "POST", "/note/128").status == 400);   // out of range: the route's
    CHECK(request(port, "POST", "/note/x").status == 404);     // no number: no route
    CHECK(request(port, "POST", "/note/-1").status == 404);
    CHECK(request(port, "POST", "/note/99999999999").status == 404);
    CHECK(request(port, "GET", "/note/1").status == 405);
    CHECK(field<int>(*served.program, *served.instance, "Service", "note") == 127);

    // An int from the query, with a default when it is missing; malformed is 400 all the same.
    CHECK(request(port, "POST", "/count?n=-3").body == "-3");
    CHECK(request(port, "POST", "/count?n=2147483647").body == "2147483647");
    CHECK(request(port, "POST", "/count").body == "7");
    CHECK(request(port, "POST", "/count?n=0").body == "0");
    CHECK(request(port, "POST", "/count?n=1").body == "1");
    for (const char* target : {"/count?n=", "/count?n=1.5", "/count?n=x", "/count?n=2147483648", "/count?n=+1"})
    {
        INFO(target);
        CHECK(request(port, "POST", target).status == 400);
    }

    // A bool: true, false, 1 or 0; its default when missing.
    CHECK(request(port, "POST", "/switch?on=false").body == "0");
    CHECK(request(port, "POST", "/switch?on=true").body == "1");
    CHECK(request(port, "POST", "/switch?on=0").body == "0");
    CHECK(request(port, "POST", "/switch?on=1").body == "1");
    CHECK(request(port, "POST", "/switch?on=0").body == "0");
    CHECK(request(port, "POST", "/switch").body == "1");
    for (const char* target : {"/switch?on=", "/switch?on=yes", "/switch?on=TRUE", "/switch?on=2", "/switch?on=truex"})
    {
        INFO(target);
        CHECK(request(port, "POST", target).status == 400);
    }

    // The request itself, and two holes filled in the path's order whatever the parameters' order.
    CHECK(request(port, "POST", "/body", "abcdef").body == "6 bytes");
    CHECK(request(port, "GET", "/a/12/b34").body == "12-34");
    // The more specific route takes what both match, whichever comes first in the program.
    CHECK(request(port, "GET", "/a/1/b34").body == "one 34");
    CHECK(request(port, "GET", "/a/01/b34").body == "1-34");

    // The bytes form.
    const Response table = request(port, "GET", "/table");
    CHECK(table.status == 200);
    REQUIRE(table.body.size() == 8);
    float values[2];
    std::memcpy(values, table.body.data(), 8);
    CHECK(values[0] == 1.5f);
    CHECK(values[1] == -2.0f);
    CHECK(table.head.find("Content-Type: application/octet-stream") != std::string::npos);

    // A reply leaves the route, its deferred statements run after it is sent: the first request sees 0.
    CHECK(request(port, "GET", "/deferred").body == "0");
    CHECK(request(port, "GET", "/deferred").body == "1");
    CHECK(field<int>(*served.program, *served.instance, "Service", "deferred") == 2);

    // A status the library refuses (HTTP_INVALID: the request is still the route's): answered 500.
    CHECK(request(port, "GET", "/status/204").status == 204);
    CHECK(request(port, "GET", "/status/99").status == 500);
    CHECK(request(port, "GET", "/status/600").status == 500);
    // And what no route names never reaches the program.
    CHECK(request(port, "GET", "/nothing").status == 404);
}

TEST_CASE("a JSON literal is written as the program wrote it: its numbers shortest, a NaN null, its keys escaped",
          "[serve][web][server][json]")
{
    constexpr const char* kJson =
        "module Json\n"
        "{\n"
        "    http_server server;\n"
        "    float zero;\n"
        "    float tenth;\n"
        "    int replies;\n"
        "    char kept[4];\n"
        "    void init() { server = http_listen(); tenth = 0.1f; kept[0] = 97; kept[2] = 98; }\n"
        "    int port() { return http_port(server); }\n"
        "    void process() [[clang::nonblocking]]\n"
        "    {\n"
        "        serve (server)\n"
        "        {\n"
        "            route \"GET /values\"\n"
        "            {\n"
        "                const double d = 0.1;\n"
        "                const int i = -7;\n"
        "                replies += 1;\n"
        "                reply 200, json { \"f\": tenth, \"d\": d, \"i\": i, \"b\": i < 0, \"nan\": zero / zero,\n"
        "                                  \"inf\": 1.0f / zero, \"-inf\": -1.0 / (double)zero, \"big\": 1e20f,\n"
        "                                  \"tiny\": 1e-7f, \"third\": (double)1 / 3, \"thirdf\": 1.0f / 3.0f,\n"
        "                                  \"neg0\": -zero, \"replies\": replies };\n"
        "            }\n"
        "            route \"GET /nested\" (int n)\n"
        "            {\n"
        "                reply 201, json { n, \"state\": { \"a\": [1, 2.5, false, [], {}], \"e\": {} }, "
        "\"list\": [n, n + 1,], };\n"
        "            }\n"
        "            route \"GET /keys\"\n"
        "            {\n"
        "                reply 200, json { \"q\\\"b\\\\c\\n\\t\\x01\\x7f\": 0, \"%d %%\": 1, \"\\xc3\\xa9t\\xc3\\xa9\": 2, "
        "\"a/b\": 3 };\n"
        "            }\n"
        "            route \"GET /empty\" { reply 200, json {}; }\n"
        "            route \"GET /array\" { reply 200, json [tenth, 2]; }\n"
        "            route \"GET /strings/{name}\" (string name, string q [[fminus::default(\"\")]], http_request r)\n"
        "            {\n"
        "                const string t = \"q\\\"b\\\\c\\n\\t\\x01\\x7f\\xc3\\xa9 %d\";\n"
        "                reply 200, json { name, \"q\": q, \"lit\": \"a\\\"b\", \"t\": t, \"empty\": \"\", "
        "\"none\": string_slice(t, 3, 1),\n"
        "                                  \"zero\": string_of(&kept[0], 3), \"path\": http_path(r), "
        "\"pick\": [q == \"\" ? \"no q\" : q, name] };\n"
        "            }\n"
        "            route \"GET /format/{name}\" (string name, int n)\n"
        "            {\n"
        "                reply 200, \"text/plain\", \"%s=%d [%s] %s%%\", name, n, \"lit\", string_slice(name, 1, 3);\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "};\n";
    Served served = serveProgram(kJson, "Json");
    const Pump pump([&] { served.process(served.instance->memory()); });
    const Response values = request(served.port, "GET", "/values");
    CHECK(values.status == 200);
    CHECK(values.head.find("Content-Type: application/json") != std::string::npos);
    CHECK(values.body
          == "{\"f\":0.1,\"d\":0.1,\"i\":-7,\"b\":true,\"nan\":null,\"inf\":null,\"-inf\":null,\"big\":1e+20,"
             "\"tiny\":1e-07,\"third\":0.3333333333333333,\"thirdf\":0.33333334,\"neg0\":-0,\"replies\":1}");
    const Response nested = request(served.port, "GET", "/nested?n=5");
    CHECK(nested.status == 201);
    CHECK(nested.body == "{\"n\":5,\"state\":{\"a\":[1,2.5,false,[],{}],\"e\":{}},\"list\":[5,6]}");
    CHECK(request(served.port, "GET", "/nested").status == 400);
    CHECK(request(served.port, "GET", "/keys").body
          == "{\"q\\\"b\\\\c\\n\\t\\u0001\x7f\":0,\"%d %%\":1,\"\xc3\xa9t\xc3\xa9\":2,\"a/b\":3}");
    CHECK(request(served.port, "GET", "/empty").body == "{}");
    CHECK(request(served.port, "GET", "/array").body == "[0.1,2]");
    // Strings: a parameter's, a literal's, a string's with every byte JSON escapes (and one above 0x7f, as it is), an
    // empty one, crossed ends, a 0 byte, a builtin's, and an arm of a ?:.
    CHECK(request(served.port, "GET", "/strings/warm-pad?q=a%22b").body
          == "{\"name\":\"warm-pad\",\"q\":\"a%22b\",\"lit\":\"a\\\"b\","
             "\"t\":\"q\\\"b\\\\c\\n\\t\\u0001\x7f\xc3\xa9 %d\","
             "\"empty\":\"\",\"none\":\"\",\"zero\":\"a\\u0000b\",\"path\":\"/strings/warm-pad\","
             "\"pick\":[\"a%22b\",\"warm-pad\"]}");
    CHECK(request(served.port, "GET", "/strings/x").body
          == "{\"name\":\"x\",\"q\":\"\",\"lit\":\"a\\\"b\",\"t\":\"q\\\"b\\\\c\\n\\t\\u0001\x7f\xc3\xa9 %d\","
             "\"empty\":\"\",\"none\":\"\",\"zero\":\"a\\u0000b\",\"path\":\"/strings/x\",\"pick\":[\"no q\",\"x\"]}");
    // %s in a reply's format: a string's bytes as they are, a literal's too.
    const Response formatted = request(served.port, "GET", "/format/bright?n=7");
    CHECK(formatted.body == "bright=7 [lit] ri%");
    CHECK(formatted.head.find("Content-Type: text/plain") != std::string::npos);
}

TEST_CASE("a reply over the host's maxReplyBytes is the library's HTTP_LIMIT, which the route answers 500",
          "[serve][web][server][json]")
{
    constexpr const char* kLimit = "module Limit\n"
                                   "{\n"
                                   "    http_server server;\n"
                                   "    void init() { server = http_listen(); }\n"
                                   "    int port() { return http_port(server); }\n"
                                   "    void process() [[clang::nonblocking]]\n"
                                   "    {\n"
                                   "        serve (server)\n"
                                   "        {\n"
                                   "            route \"GET /small\" { reply 200, json { \"a\": 1 }; }\n"
                                   "            route \"GET /large\" { reply 200, json { \"a long key, longer than "
                                   "the host's limit\": 1 }; }\n"
                                   "            route \"GET /text\" { reply 200, \"text/plain\", \"%g %g %g\", 1.0 / 3, "
                                   "2.0 / 3, 1.0; }\n"
                                   "        }\n"
                                   "    }\n"
                                   "};\n";
    fminus::Program::Options options;
    options.web.server.maxReplyBytes = 16;
    Served served = serveProgram(kLimit, "Limit", options);
    const Pump pump([&] { served.process(served.instance->memory()); });
    CHECK(request(served.port, "GET", "/small").body == "{\"a\":1}");
    CHECK(request(served.port, "GET", "/large").status == 500);
    CHECK(request(served.port, "GET", "/text").status == 500);
    CHECK(request(served.port, "GET", "/small").status == 200);   // each request consumed: the server goes on
}

TEST_CASE("two serves on two threads each take their own routes' requests, and leave the other's waiting",
          "[serve][web][server]")
{
    constexpr const char* kSplit =
        "module Split\n"
        "{\n"
        "    http_server server;\n"
        "    int audio;\n"
        "    int loaded;\n"
        "    void init() { server = http_listen(); }\n"
        "    int port() { return http_port(server); }\n"
        "    void process() [[clang::nonblocking]]\n"
        "    {\n"
        "        serve (server) { route \"POST /a\" { audio += 1; reply 204; } }\n"
        "    }\n"
        "    int load(int wait)\n"
        "    {\n"
        "        const int before = loaded;\n"
        "        serve (server, wait)\n"
        "        {\n"
        "            route \"PUT /b/{k}\" (int k) { loaded += k; reply 201; }\n"
        "            route \"GET /b/{k}\" (int k) { reply 200, json { k, \"loaded\": loaded }; }\n"
        "        }\n"
        "        return loaded - before;\n"
        "    }\n"
        "};\n";
    Served served = serveProgram(kSplit, "Split");
    std::string errors;
    auto* load = served.program->function<int(void*, int)>("Split::load", errors);
    REQUIRE(load);
    const auto audio = [&] { return field<int>(*served.program, *served.instance, "Split", "audio"); };
    {
        const Pump pump([&] { served.process(served.instance->memory()); });
        CHECK(request(served.port, "POST", "/a").status == 204);
        // The loader's route: the audio thread's serve leaves it waiting.
        const int waiting = sendRequest(served.port, "PUT", "/b/3", "");
        REQUIRE(waiting >= 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(request(served.port, "POST", "/a").status == 204);   // not in the way
        CHECK(field<int>(*served.program, *served.instance, "Split", "loaded") == 0);
        // The loader, on a thread of its own, takes it (and only it), while the audio thread goes on.
        int taken = -1;
        std::thread loader([&] { taken = load(served.instance->memory(), 2000); });
        CHECK(readResponse(waiting).status == 201);
        loader.join();
        CHECK(taken == 3);
        // A loader that waits, then takes what came meanwhile: a request sent 50 ms into its wait.
        std::thread late([&] { taken = load(served.instance->memory(), 5000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const Response got = request(served.port, "GET", "/b/9");
        late.join();
        CHECK(got.body == "{\"k\":9,\"loaded\":3}");
        CHECK(taken == 0);
        CHECK(audio() == 2);
    }
    // With no request, a loader's serve waits its time, then is done.
    const auto start = std::chrono::steady_clock::now();
    CHECK(load(served.instance->memory(), 30) == 0);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(25));
    CHECK(load(served.instance->memory(), 0) == 0);
}

TEST_CASE("webRoutes lists a route statement with its parameters: name, type, what fills it, range and default",
          "[serve][web][server][library]")
{
    fminus::Program::Options options;
    auto program = compileProgram(kService, options);
    const std::vector<fminus::Program::WebRoute>& routes = program->webRoutes();
    REQUIRE(routes.size() == 10);
    CHECK(routes[0].method == "POST");
    CHECK(routes[0].pattern == "/gain");
    CHECK(routes[0].statement);
    CHECK(routes[0].line == 16);
    CHECK(routes[0].column == 13);
    REQUIRE(routes[0].parameters.size() == 1);
    const fminus::Program::RouteParameter& value = routes[0].parameters[0];
    CHECK(value.name == "value");
    CHECK(value.type == "float");
    CHECK(value.source == "query");
    CHECK(value.minimum == 0.0);
    CHECK(value.maximum == 1.0);
    CHECK_FALSE(value.defaultValue);
    CHECK(routes[1].pattern == "/note/*");
    REQUIRE(routes[1].parameters.size() == 1);
    CHECK(routes[1].parameters[0].source == "path");
    CHECK(routes[1].parameters[0].type == "int");
    CHECK(routes[1].parameters[0].maximum == 127.0);
    CHECK(routes[2].parameters[0].defaultValue == 7.0);
    CHECK_FALSE(routes[2].parameters[0].minimum);
    CHECK(routes[3].parameters[0].type == "bool");
    CHECK(routes[3].parameters[0].defaultValue == 1.0);
    CHECK(routes[4].parameters[0].type == "http_request");
    CHECK(routes[4].parameters[0].source == "request");
    CHECK(routes[5].pattern == "/table");
    CHECK(routes[5].parameters.empty());
    CHECK(routes[6].pattern == "/a/*/b*");
    REQUIRE(routes[6].parameters.size() == 2);
    CHECK(routes[6].parameters[0].name == "y");
    CHECK(routes[6].parameters[0].source == "path");
    // An http_is call's, beside them, has none.
    auto is = compileProgram("bool f(http_request r) { return http_is(r, \"GET /x/%d\"); }", options);
    REQUIRE(is->webRoutes().size() == 1);
    CHECK_FALSE(is->webRoutes()[0].statement);
    CHECK(is->webRoutes()[0].parameters.empty());
}

TEST_CASE("a string parameter is a whole segment of the path, or a query's field as sent; a request's header, path "
          "and query read as text",
          "[serve][web][server][string]")
{
    constexpr const char* kTexts =
        "module Texts\n"
        "{\n"
        "    http_server server;\n"
        "    char kept[32];\n"
        "    int size;\n"
        "    void init() { server = http_listen(); }\n"
        "    int port() { return http_port(server); }\n"
        "    void process() [[clang::nonblocking]]\n"
        "    {\n"
        "        serve (server)\n"
        "        {\n"
        "            route \"PUT /presets/{name}\" (string name)\n"
        "            {\n"
        "                size = string_copy(kept, 32, name);\n"
        "                reply 201, string_data(name), string_size(name), \"text/plain\";\n"
        "            }\n"
        "            route \"GET /presets/{name}\" (string name, string mode [[fminus::default(\"full\")]])\n"
        "            {\n"
        "                switch (mode)\n"
        "                {\n"
        "                case \"full\":\n"
        "                    reply 200, string_data(name), string_size(name), \"text/plain\";\n"
        "                case \"size\":\n"
        "                    reply 200, \"text/plain\", \"%d\", string_size(name);\n"
        "                default:\n"
        "                    reply 200, string_data(mode), string_size(mode), \"text/plain\";\n"
        "                }\n"
        "            }\n"
        "            route \"GET /presets/default\" { reply 200, \"text/plain\", \"the literal\"; }\n"
        "            route \"GET /presets/{k}\" (int k) { reply 200, \"text/plain\", \"number %d\", k; }\n"
        "            route \"GET /a/{x}/{n}/{y}\" (string y, int n, string x)\n"
        "            {\n"
        "                reply 200, \"text/plain\", \"%d %d %d %d\", string_size(x), n, string_size(y), x == y;\n"
        "            }\n"
        "            route \"GET /need\" (string v) { reply 200, string_data(v), string_size(v), \"text/plain\"; }\n"
        "            route \"GET /header\" (http_request q)\n"
        "            {\n"
        "                const string h = http_header(q, \"X-Preset\");\n"
        "                reply 200, string_data(h), string_size(h), \"text/plain\";\n"
        "            }\n"
        "            route \"GET /path/{k}\" (int k, http_request q)\n"
        "            {\n"
        "                const string p = http_path(q);\n"
        "                reply 200, string_data(p), string_size(p), \"text/plain\";\n"
        "            }\n"
        "            route \"GET /query\" (http_request q)\n"
        "            {\n"
        "                const string v = http_query_string(q, \"v\", \"(none)\");\n"
        "                reply 200, string_data(v), string_size(v), \"text/plain\";\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "};\n";
    Served served = serveProgram(kTexts, "Texts");
    const Pump pump([&] { served.process(served.instance->memory()); });
    const int port = served.port;

    // From the path: a whole segment, its bytes as the server holds them, kept in a char array.
    const Response put = request(port, "PUT", "/presets/warm-pad_2.v~1");
    CHECK(put.status == 201);
    CHECK(put.body == "warm-pad_2.v~1");
    CHECK(field<int>(*served.program, *served.instance, "Texts", "size") == 14);
    CHECK(request(port, "GET", "/presets/bright").body == "bright");
    // A literal segment before a number's hole before a string's, whichever the program wrote first.
    CHECK(request(port, "GET", "/presets/default").body == "the literal");
    CHECK(request(port, "GET", "/presets/0042").body == "number 42");
    CHECK(request(port, "GET", "/presets/42x").body == "42x");
    // A path the server holds no segment of (a byte it takes no path with, an empty segment) is the library's 400.
    CHECK(request(port, "GET", "/presets/a%20b").status == 400);
    CHECK(request(port, "GET", "/presets/").status == 400);
    // From the query, as sent (nothing decoded), its default when it is missing; empty is empty.
    CHECK(request(port, "GET", "/presets/x?mode=size").body == "1");
    CHECK(request(port, "GET", "/presets/x?mode=a%20b+c").body == "a%20b+c");
    CHECK(request(port, "GET", "/presets/x?mode=").body == "");
    CHECK(request(port, "GET", "/presets/xyz").body == "xyz");
    CHECK(request(port, "GET", "/need?v=hello").body == "hello");
    CHECK(request(port, "GET", "/need?v=").status == 200);
    CHECK(request(port, "GET", "/need?v").status == 200);
    CHECK(request(port, "GET", "/need").status == 400);
    CHECK(request(port, "GET", "/need?w=1").status == 400);
    // Holes filled in the path's order whatever the parameters' order, strings and ints together.
    CHECK(request(port, "GET", "/a/abc/7/de").body == "3 7 2 0");
    CHECK(request(port, "GET", "/a/q/0/q").body == "1 0 1 1");
    // A header: its first field of the name, in any letter case, without the white space around its value; empty
    // when there is none.
    CHECK(requestWith(port, "GET", "/header", "x-preset:   warm pad \t\r\nX-Preset: second\r\n").body == "warm pad");
    CHECK(requestWith(port, "GET", "/header", "X-PRESET: Up\r\n").body == "Up");
    CHECK(requestWith(port, "GET", "/header", "").body == "");
    CHECK(requestWith(port, "GET", "/header", "X-Presets: no\r\n").body == "");
    // The path as sent, without its query.
    CHECK(request(port, "GET", "/path/007?x=1").body == "/path/007");
    // A query's field, or the fallback.
    CHECK(request(port, "GET", "/query?v=%41&w=2").body == "%41");
    CHECK(request(port, "GET", "/query?w=2").body == "(none)");
    CHECK(request(port, "GET", "/query?v=").body == "");

    // And the host's list of them.
    const std::vector<fminus::Program::WebRoute>& routes = served.program->webRoutes();
    REQUIRE(routes.size() == 9);
    CHECK(routes[0].pattern == "/presets/*");
    REQUIRE(routes[0].parameters.size() == 1);
    CHECK(routes[0].parameters[0].type == "string");
    CHECK(routes[0].parameters[0].source == "path");
    CHECK_FALSE(routes[0].parameters[0].defaultText);
    REQUIRE(routes[1].parameters.size() == 2);
    CHECK(routes[1].parameters[1].name == "mode");
    CHECK(routes[1].parameters[1].type == "string");
    CHECK(routes[1].parameters[1].source == "query");
    CHECK(routes[1].parameters[1].defaultText == "full");
    CHECK_FALSE(routes[1].parameters[1].defaultValue);
    CHECK_FALSE(routes[1].parameters[1].minimum);
    CHECK(routes[4].pattern == "/a/*/*/*");
    CHECK_FALSE(routes[5].parameters[0].defaultText);
}

TEST_CASE("a serve in a [[clang::nonblocking]] method, compiled by the JIT, takes, fills and answers without "
          "allocating or making a system call",
          "[serve][web][server][library]")
{
    // WebServeTest.cpp proves it of the builtins a program calls; this is the proof for what a serve, its parameters,
    // its replies and a JSON literal become.
    constexpr const char* kTaker =
        "module Taker\n"
        "{\n"
        "    http_server server;\n"
        "    float table[2];\n"
        "    int kind;\n"
        "    double sum;\n"
        "    int answered;\n"
        "    void init() { server = http_listen(); table[0] = 1.5f; table[1] = -2.0f; }\n"
        "    int port() { return http_port(server); }\n"
        "    void process() [[clang::nonblocking]]\n"
        "    {\n"
        "        serve (server)\n"
        "        {\n"
        "            route \"POST /table/{k}/{name}\" (int k [[fminus::range(0, 99)]], float v, int n, bool b "
        "[[fminus::default(0)]], http_request r, string name, string mode [[fminus::default(\"none\")]])\n"
        "            {\n"
        "                sum = k + v + n + (b ? 100 : 0) + http_size(r) + string_size(name) + string_size(mode)\n"
        "                      + string_size(http_header(r, \"x-n\")) + string_size(http_path(r))\n"
        "                      + string_size(http_query_string(r, \"v\", \"\"));\n"
        "                answered += 1;\n"
        "                if (kind == 0)\n"
        "                    reply 204;\n"
        "                if (kind == 1)\n"
        "                    reply 200, table, 8, \"application/octet-stream\";\n"
        "                if (kind == 2)\n"
        "                    reply 200, \"\", \"%g %.9g %d %%\", 0.1, sum, -7;\n"
        "                reply 200, json { \"sum\": sum, \"k\": k, \"v\": v, \"b\": b, \"t\": [table[0], table[1]] };\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "};\n";
    fminus::Program::Options options;
    options.web.server.replyTimeoutMs = 20000;
    Served served = serveProgram(kTaker, "Taker", options);
    // The bytes, the name (3), the mode's default (4), the header (5), the path (13) and the query's v (4).
    const double expected = 12 + 0.75 - 3 + 100 + 4 + 3 + 4 + 5 + 13 + 4;
    const std::string post = "POST /table/12/abc?v=0.75&n=-3&b=true HTTP/1.1\r\nHost: 127.0.0.1:"
                             + std::to_string(served.port) + "\r\nX-N: hello\r\nContent-Length: 4\r\n\r\nwxyz";
    std::vector<int> clients;
    const auto send = [&] {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(served.port));
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        REQUIRE(::send(fd, post.data(), post.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(post.size()));
        clients.push_back(fd);
    };
    void* memory = served.instance->memory();
    const auto set = [&](const char* name, auto value) {
        for (const fminus::Program::Module::Field& field : served.program->module("Taker")->fields)
        {
            if (field.name == name)
                std::memcpy(static_cast<char*>(memory) + field.offset, &value, sizeof value);
        }
    };
    const auto answered = [&] { return field<int>(*served.program, *served.instance, "Taker", "answered"); };

    // In this process, counted on this thread, for each kind of reply.
    if (fminus::test::HeapCalls::counted())
    {
        for (int kind = 0; kind < 4; ++kind)
        {
            set("kind", kind);
            send();
            const int before = answered();
            bool done = false;
            for (int attempt = 0; attempt < 500 && !done; ++attempt)
            {
                long calls = 0;
                {
                    const fminus::test::HeapCalls counting;
                    served.process(memory);
                    calls = counting.count();
                }
                done = answered() > before;
                if (done)
                {
                    INFO("kind " << kind);
                    CHECK(calls == 0);
                    CHECK(field<double>(*served.program, *served.instance, "Taker", "sum") == expected);
                }
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            CHECK(done);
        }
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");

    // In a child under seccomp's strict mode, on a request published before the fork.
    send();
    for (int kind = 0; kind < 4; ++kind)
    {
        set("kind", kind);
        bool checked = false;
        for (int attempt = 0; attempt < 200 && !checked; ++attempt)
        {
            int fds[2];
            REQUIRE(::pipe(fds) == 0);
            const int before = answered();
            const pid_t child = ::fork();
            REQUIRE(child >= 0);
            if (child == 0)
            {
                ::close(fds[0]);
                if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
                    ::syscall(SYS_exit, 2);
                served.process(memory);
                double result[2] = {double(answered() - before),
                                    field<double>(*served.program, *served.instance, "Taker", "sum")};
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
                std::this_thread::sleep_for(std::chrono::milliseconds(5));   // not published yet
        }
        CHECK(checked);
    }
    // The parent's copy of that request is still waiting, and is answered here.
    set("kind", 0);
    const int before = answered();
    for (int attempt = 0; attempt < 500 && answered() == before; ++attempt)
    {
        served.process(memory);
        if (answered() == before)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(answered() > before);
    for (int fd : clients)
        ::close(fd);
}

TEST_CASE("every path through a route must reach a reply: the missing return's analysis, at the closing brace",
          "[codegen][serve]")
{
    const auto routed = [](const std::string& block) {
        return "enum { RUNNING = 1 };\nvoid f(http_server s, int n)\n{\n\n    serve (s) { route \"GET /a\" "
               + block + " }\n}\n";
    };
    // Every way the missing return's tests leave a function only by a return (RecoveryTest.cpp).
    for (const char* block : {"{ while (1) { if (n > 3) reply 200; ++n; } }", "{ for (;;) { if (n > 3) reply 1; ++n; } }",
                              "{ while (RUNNING) { if (n > 3) reply 1; ++n; } }", "{ if (1) reply 1; }",
                              "{ if (0) { } else reply 1; }",
                              "{ if (true) { while (true) { if (n > 0) reply 1; ++n; } } }",
                              "{ switch (n) { case 1: reply 1; default: reply 2; } }", "{ { reply 1; } }"})
    {
        INFO(block);
        std::string errors;
        CHECK(compile(routed(block), &errors) != nullptr);
        CHECK(errors.empty());
    }
    // A break, a condition that is no constant, a constant that skips the reply, a switch without a default.
    for (const char* block : {"{ while (1) { if (n) break; if (n > 1) reply 1; } }",
                              "{ while (n) { reply 1; } }", "{ if (0) reply 1; }", "{ for (; 0;) reply 1; }",
                              "{ if (n) reply 1; else { } }", "{ switch (n) { case 1: reply 1; } }",
                              "{ if (n) reply 1; }"})
    {
        INFO(block);
        const std::size_t close = routed(block).find(" }\n}\n") - 1;   // the route block's closing brace
        const std::size_t lineStart = routed(block).rfind('\n', close) + 1;
        CHECK(errorsOf(routed(block)) == "test.fm:5:" + std::to_string(close - lineStart + 1)
                                             + ": error: route 'GET /a' does not reply in all control paths\n");
    }
    CHECK(errorsOf(routed("{ n = 1; }")) == "test.fm:5:41: error: route 'GET /a' does not reply\n");
    CHECK(errorsOf(routed("{ }")) == "test.fm:5:34: error: route 'GET /a' does not reply\n");
}

TEST_CASE("serve, route and reply's errors, each once at its column", "[codegen][serve]")
{
    CHECK(errorsOf("void f(http_server s)\n"
                   "{\n"
                   "    reply 204;\n"
                   "    serve (s) { }\n"
                   "    serve (s, 1, 2) { route \"GET /a\" { reply 1; } }\n"
                   "    serve (1.5f) { route \"GET /b\" { reply 1; } }\n"
                   "    serve (s, s) { route \"GET /c\" { reply 1; } }\n"
                   "}\n")
          == "test.fm:3:5: error: 'reply' statement not in a route\n"
             "test.fm:4:5: error: a serve has no route: it takes the requests waiting for its routes, and has none\n"
             "test.fm:5:18: error: a serve takes a server and, to wait for a request, a timeout in milliseconds: serve "
             "(server) or serve (server, timeout_ms)\n"
             "test.fm:6:12: error: passing 'float' to parameter of incompatible type 'http_server'\n"
             "test.fm:7:15: error: passing 'http_server' to parameter of incompatible type 'int'\n");
    // What a route's block may not do, each at its keyword; what it may.
    CHECK(errorsOf("void f(http_server s, int n)\n"
                   "{\n"
                   "    for (;;)\n"
                   "    {\n"
                   "        serve (s)\n"
                   "        {\n"
                   "            route \"GET /a\" { if (n) return; if (n > 1) break; if (n > 2) continue; reply 1; }\n"
                   "            route \"GET /b\" { serve (s) { route \"GET /c\" { reply 1; } } reply 1; }\n"
                   "            route \"GET /d\" { while (n) { if (n) break; continue; } switch (n) { case 1: break; } "
                   "reply 1; }\n"
                   "            route \"GET /e\" { defer reply 1; reply 2; }\n"
                   "        }\n"
                   "        switch (n) { case 0: serve (s) { route \"GET /f\" { case 1: reply 1; } } }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:7:37: error: cannot return from a route: a route ends with a reply\n"
             "test.fm:7:56: error: 'break' cannot leave a route: a route ends with a reply\n"
             "test.fm:7:74: error: 'continue' cannot leave a route: a route ends with a reply\n"
             "test.fm:8:30: error: a route cannot hold a serve: a serve takes its own routes' requests, and a route "
             "ends with a reply to its own\n"
             "test.fm:10:36: error: cannot reply from a defer statement\n"
             "test.fm:12:59: error: cannot jump from switch statement to this case label\n"
             "test.fm:12:42: note: jump enters a route\n");
    // A route's literal: holes, and every mistake http_is's has, at its byte.
    CHECK(errorsOf("void f(http_server s)\n"
                   "{\n"
                   "    serve (s)\n"
                   "    {\n"
                   "        route \"GET /a/%d\" { reply 1; }\n"
                   "        route \"GET /b/{x}{y}\" (int x, int y) { reply 1; }\n"
                   "        route \"GET /c/{x}2\" (int x) { reply 1; }\n"
                   "        route \"GET /d/{x\" (int x) { reply 1; }\n"
                   "        route \"GET /e/{1x}\" { reply 1; }\n"
                   "        route \"GET /f/{x}\" { reply 1; }\n"
                   "        route \"GET /g/{x}/{x}\" (int x) { reply 1; }\n"
                   "        route \"HEAD /h\" { reply 1; }\n"
                   "        route \"GET /i?x=1\" { reply 1; }\n"
                   "        route \"GET /j/{a}{b}\" (int a, int b) { reply 1; }\n"
                   "        route \"GET /k/{a}/{b}/{c}/{d}/{e}/{f}/{g}/{h}/{i}\" (int a, int b, int c, int d, int e, "
                   "int f, int g, int h, int i) { reply 1; }\n"
                   "        route \"GET /l/{x} y\" (int x) { reply 1; }\n"
                   "        route \"GET /m/{x}/..\" (int x) { reply 1; }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:5:23: error: a route statement writes a hole as {name}, a parameter's name: %d is http_is's "
             "spelling\n"
             "test.fm:6:26: error: a hole right after another can never match: each takes all the digits there are\n"
             "test.fm:7:26: error: a digit right after a hole can never match: a hole takes all the digits there are\n"
             "test.fm:8:23: error: a hole is a parameter's name in braces, {name}: this one has no '}'\n"
             "test.fm:9:24: error: a hole is a parameter's name in braces, {name}\n"
             "test.fm:10:23: error: hole {x} names no parameter of the route\n"
             "test.fm:11:27: error: parameter 'x' fills a hole of the route already: each hole is another parameter's\n"
             "test.fm:12:16: error: 'HEAD' is not a method a route can name: GET, POST, PUT, PATCH or DELETE (a GET "
             "route answers HEAD too)\n"
             "test.fm:13:22: error: a route is a path alone: its parameters that no hole names are read from the "
             "request's query\n"
             "test.fm:14:26: error: a hole right after another can never match: each takes all the digits there are\n"
             "test.fm:15:55: error: a route takes at most 8 holes\n"
             "test.fm:16:26: error: character ' ' is not allowed in a route's path: letters, digits, '/', '-', '.', "
             "'_', '~' and {name}\n"
             "test.fm:17:27: error: a route's path has no '.' or '..' segment\n");
    // Its parameters and the attributes after them.
    CHECK(errorsOf("void f(http_server s)\n"
                   "{\n"
                   "    serve (s)\n"
                   "    {\n"
                   "        route \"GET /a\" (int* p, float4 v, double d) { reply 1; }\n"
                   "        route \"GET /b/{x}\" (float x, int y [[fminus::range(2, 1)]], int x2 [[fminus::default(1.5)]]) "
                   "{ reply 1; }\n"
                   "        route \"GET /c/{x}\" (int x [[fminus::default(3)]], http_request r [[fminus::range(0, 1)]]) "
                   "{ reply 1; }\n"
                   "        route \"GET /d\" (http_request r, http_request q, int r) [[clang::nonblocking]] { reply 1; }\n"
                   "        route \"GET /e\" (int n [[ui::unit(\"dB\")]], int m [[fminus::default(9)]] "
                   "[[fminus::range(0, 1)]]) { reply 1; }\n"
                   "        route \"GET /f\" (const int n) { n = 2; reply 1; }\n"
                   "        route \"GET /g\" (bool b [[fminus::default(2)]]) { reply 1; }\n"
                   "        route \"GET /h\" (int a_query_name_that_is_longer_than_sixty_four_bytes_which_no_query_name_is) "
                   "{ reply 1; }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:5:30: error: a route's parameter is an int, a float, a bool, a string or an http_request, not "
             "'int *'\n"
             "test.fm:5:40: error: a route's parameter is an int, a float, a bool, a string or an http_request, not "
             "'float4'\n"
             "test.fm:5:50: error: a route's parameter is an int, a float, a bool, a string or an http_request, not "
             "'double'\n"
             "test.fm:6:35: error: parameter 'x' fills the hole {x}, a segment of the path or a number in one: it is "
             "an int or a string, not 'float'\n"
             "test.fm:6:46: error: fminus::range's minimum must be below its maximum\n"
             "test.fm:6:78: error: the default of an int parameter is an int, not 1.5\n"
             "test.fm:7:37: error: 'fminus::default' does not apply to a hole's parameter: a request the route takes "
             "always has its number\n"
             "test.fm:7:76: error: 'fminus::range' attribute does not apply to the route's request\n"
             "test.fm:8:54: error: a route has one http_request parameter at most, its request\n"
             "test.fm:8:38: note: the route's request is here\n"
             "test.fm:8:61: error: redefinition of parameter 'r'\n"
             "test.fm:8:66: error: 'clang::nonblocking' attribute cannot be applied to a route: a route is a block of "
             "the function its serve is in, whose attributes cover it\n"
             "test.fm:9:33: error: unknown attribute 'ui::unit': a route's parameter takes fminus::range(min, max) and "
             "fminus::default(value)\n"
             "test.fm:9:55: error: the default of 'm', 9, is outside its fminus::range(0, 1)\n"
             "test.fm:10:42: error: cannot assign to variable 'n' with const-qualified type 'const int'\n"
             "test.fm:11:34: error: the default of a bool parameter is 0 or 1, not 2\n"
             "test.fm:12:29: error: parameter 'a_query_name_that_is_longer_than_sixty_four_bytes_which_no_query_name_is' "
             "is read from the query's field of its name, which is 1 to 64 bytes, not 72\n");
    // A reply's forms, each builtin's checks of its arguments, and the JSON literal's.
    CHECK(errorsOf("enum E { A, B };\n"
                   "void f(http_server s, const float* t, int* p)\n"
                   "{\n"
                   "    serve (s)\n"
                   "    {\n"
                   "        route \"GET /a\" (int n)\n"
                   "        {\n"
                   "            if (n == 1) reply 200, \"text/plain\";\n"
                   "            if (n == 2) reply 200, t, 4;\n"
                   "            if (n == 3) reply 200, \"text/plain\", \"%g\", n;\n"
                   "            if (n == 4) reply 200, t, 4, \"a\\nb\";\n"
                   "            if (n == 5) reply 200, json { \"a\": 1 }, 3;\n"
                   "            if (n == 6) reply 200, json { \"a\": p, \"b\": \"text\", \"a\": 2 };\n"
                   "            if (n == 7) reply \"200\";\n"
                   "            if (n == 8) reply 200, json { \"\\xff\": 1, \"c\": json { } };\n"
                   "            reply 200, json { \"e\": (enum E)1, \"f\": t[0] + json [1] };\n"
                   "        }\n"
                   "    }\n"
                   "}\n"
                   "float g() { return json { }; }\n")
          == "test.fm:8:25: error: no form of reply takes 2 values: a reply is 'reply status;', 'reply status, "
             "data, bytes, \"type\";', 'reply status, \"type\", \"format\", values...;' or 'reply status, json { "
             "... };'\n"
             "test.fm:9:25: error: no form of reply takes 3 values: a reply is 'reply status;', 'reply status, "
             "data, bytes, \"type\";', 'reply status, \"type\", \"format\", values...;' or 'reply status, json { "
             "... };'\n"
             "test.fm:10:56: error: format specifies type 'double' but the argument has type 'int'\n"
             "test.fm:11:44: error: character '\\x0a' is not allowed in a content type: it is printable ASCII\n"
             "test.fm:12:53: error: a JSON literal is a reply's whole body: nothing follows it\n"
             "test.fm:13:48: error: a JSON literal's value is a float, a double, an int, a bool, a string, an object "
             "or an array, not 'int *'\n"
             "test.fm:13:64: error: duplicate key 'a' in a JSON object\n"
             "test.fm:13:43: note: previous key is here\n"
             "test.fm:14:31: error: incompatible pointer to integer conversion passing 'char[4]' to parameter of "
             "type 'int'\n"
             "test.fm:15:44: error: a JSON key is UTF-8 text, and this byte is no part of any\n"
             "test.fm:15:59: error: FMinus has no JSON values: a JSON literal is only a reply's body, 'reply "
             "status, json { ... };'\n"
             "test.fm:16:36: error: a JSON literal does not take a value of an enumerated type yet, which a later "
             "version writes as its constant's name: cast it to int for its number\n"
             "test.fm:16:59: error: FMinus has no JSON values: a JSON literal is only a reply's body, 'reply "
             "status, json { ... };'\n"
             "test.fm:20:20: error: FMinus has no JSON values: a JSON literal is only a reply's body, 'reply "
             "status, json { ... };'\n");
}

TEST_CASE("no two routes one request could match unless one is more specific: an error at the second, a note at the "
          "first",
          "[codegen][serve]")
{
    // Legal: disjoint, one more specific in Go's ServeMux's sense and in the library's ranking, the same literal in
    // two http_is calls, the same path for two methods.
    std::string errors;
    CHECK(compile("void f(http_server s, http_request q)\n"
                  "{\n"
                  "    serve (s)\n"
                  "    {\n"
                  "        route \"GET /a/{x}\" (int x) { reply 1; }\n"
                  "        route \"GET /a/1\" { reply 1; }\n"
                  "        route \"GET /a/1/{y}\" (int y) { reply 1; }\n"
                  "        route \"GET /b/v{x}\" (int x) { reply 1; }\n"
                  "        route \"GET /b/{x}-{y}\" (int x, int y) { reply 1; }\n"
                  "        route \"GET /b/{x}-1\" (int x) { reply 1; }\n"
                  "        route \"POST /a/{x}\" (int x) { reply 1; }\n"
                  "        route \"GET /d/{x}/y\" (int x) { reply 1; }\n"
                  "        route \"GET /d/1/{z}\" (int z) { reply 1; }\n"
                  "    }\n"
                  "    if (http_is(q, \"GET /c/%d\") || http_is(q, \"GET /c/%d\") || http_is(q, \"GET /c\"))\n"
                  "        http_reply(q, 204);\n"
                  "}\n",
                  &errors)
          != nullptr);
    CHECK(errors.empty());
    CHECK(errorsOf("void f(http_server s, http_server t, http_request q)\n"
                   "{\n"
                   "    serve (s)\n"
                   "    {\n"
                   "        route \"GET /a/{x}/2\" (int x) { reply 1; }\n"
                   "        route \"GET /a/1/{z}\" (int z) { reply 1; }\n"
                   "        route \"GET /b/{x}\" (int x) { reply 1; }\n"
                   "        route \"GET /b/{y}\" (int y) { reply 1; }\n"
                   "        route \"GET /c/1{x}\" (int x) { reply 1; }\n"
                   "        route \"GET /c/{x}\" (int x) { reply 1; }\n"
                   "    }\n"
                   "    serve (t) { route \"GET /state\" { reply 1; } }\n"
                   "    serve (t, 10) { route \"GET /state\" { reply 1; } }\n"
                   "    if (http_is(q, \"GET /state\") || http_is(q, \"PUT /d/%d\"))\n"
                   "        http_reply(q, 204);\n"
                   "}\n"
                   "void g(http_server s) { serve (s) { route \"PUT /d/{n}\" (int n) { reply 1; } } }\n")
          == "test.fm:6:15: error: route 'GET /a/1/{z}' conflicts with 'GET /a/{x}/2': GET /a/1/2 matches both, and "
             "neither is more specific than the other\n"
             "test.fm:5:15: note: the other route is here\n"
             "test.fm:8:15: error: route 'GET /b/{y}' conflicts with 'GET /b/{x}': they match the same requests, and "
             "neither would take them before the other\n"
             "test.fm:7:15: note: the other route is here\n"
             "test.fm:10:15: error: route 'GET /c/{x}' conflicts with 'GET /c/1{x}': GET /c/10 matches both, and each "
             "has as many holes as the other in each segment, so neither is taken before the other\n"
             "test.fm:9:15: note: the other route is here\n"
             "test.fm:13:27: error: the route 'GET /state' is written twice: which would take its requests?\n"
             "test.fm:12:23: note: the other route is here\n"
             "test.fm:14:20: error: the route 'GET /state' is written twice: which would take its requests?\n"
             "test.fm:12:23: note: the other route is here\n"
             "test.fm:17:43: error: route 'PUT /d/{n}' conflicts with 'PUT /d/%d': they match the same requests, and "
             "neither would take them before the other\n"
             "test.fm:14:48: note: the other route is here\n");
}

// Where serve meets string, char and when (src/ir/Serve.cpp, Text.cpp, Statements.cpp's lowerWhen, Modules.cpp's
// collectWhens): a route's block is a block of its method, so a when there keeps its bool in the module's instance,
// and a switch on a string replies from its cases.
TEST_CASE("a route in a [[clang::nonblocking]] method uses a when, a switch on a string and a char array: each edge "
          "once, each case's reply, nothing allocated",
          "[serve][web][server][when][string]")
{
    constexpr const char* kGate =
        "module Gate\n"
        "{\n"
        "    http_server server;\n"
        "    char mode[8];\n"
        "    int size;\n"
        "    int fired;\n"
        "    int answered;\n"
        "    void init() { server = http_listen(); size = string_copy(mode, 8, \"saw\"); }\n"
        "    int port() { return http_port(server); }\n"
        "    void process() [[clang::nonblocking]]\n"
        "    {\n"
        "        serve (server)\n"
        "        {\n"
        "            route \"POST /gate\" (int level)\n"
        "            {\n"
        "                answered += 1;\n"
        "                when (level > 0) fired += 1;\n"
        "                when (level > 5) reply 202;\n"
        "                const string shape = string_of(mode, size);\n"
        "                switch (shape)\n"
        "                {\n"
        "                case \"saw\":\n"
        "                    reply 200, \"text/plain\", \"saw %d\", fired;\n"
        "                case \"sine\":\n"
        "                    reply 201, \"text/plain\", \"sine %d\", fired;\n"
        "                default:\n"
        "                    reply 404;\n"
        "                }\n"
        "            }\n"
        "            route \"POST /mode/{k}\" (int k)\n"
        "            {\n"
        "                const string names[3] = {\"saw\", \"sine\", \"square\"};\n"
        "                size = string_copy(mode, 8, k >= 0 && k < 3 ? names[k] : \"none\");\n"
        "                reply 204;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "};\n";
    Served served = serveProgram(kGate, "Gate");
    // The module's two whens are two bools after its declared fields, of which the host is told nothing.
    const fminus::Program::Module* layout = served.program->module("Gate");
    REQUIRE(layout != nullptr);
    CHECK(layout->fields.size() == 5);
    {
        const Pump pump([&] { served.process(served.instance->memory()); });
        const int port = served.port;
        // The first when fires on each rising edge of level > 0, request after request; the second answers 202 on
        // each rising edge of level > 5, and the switch answers the rest by the mode's text.
        CHECK(request(port, "POST", "/gate?level=1").body == "saw 1");
        CHECK(request(port, "POST", "/gate?level=2").body == "saw 1");
        CHECK(request(port, "POST", "/gate?level=0").body == "saw 1");
        CHECK(request(port, "POST", "/gate?level=3").body == "saw 2");
        CHECK(request(port, "POST", "/gate?level=6").status == 202);
        CHECK(request(port, "POST", "/gate?level=7").body == "saw 2");
        CHECK(request(port, "POST", "/gate?level=7").body == "saw 2");
        CHECK(request(port, "POST", "/mode/1").status == 204);
        const Response sine = request(port, "POST", "/gate?level=0");
        CHECK(sine.status == 201);
        CHECK(sine.body == "sine 2");
        CHECK(request(port, "POST", "/mode/2").status == 204);
        CHECK(request(port, "POST", "/gate?level=9").status == 202);   // both whens fire
        CHECK(request(port, "POST", "/gate?level=0").status == 404);   // "square" is no case
        CHECK(request(port, "POST", "/gate").status == 400);
    }
    CHECK(field<int>(*served.program, *served.instance, "Gate", "fired") == 3);

    // Taken, filled, switched and answered on this thread, without a call of the heap: the mode is "square" still.
    if (fminus::test::HeapCalls::counted())
    {
        const auto answered = [&] { return field<int>(*served.program, *served.instance, "Gate", "answered"); };
        const int fd = sendRequest(served.port, "POST", "/gate?level=1", "");
        REQUIRE(fd >= 0);
        const int before = answered();
        bool done = false;
        for (int attempt = 0; attempt < 500 && !done; ++attempt)
        {
            long calls = 0;
            {
                const fminus::test::HeapCalls counting;
                served.process(served.instance->memory());
                calls = counting.count();
            }
            done = answered() > before;
            if (done)
                CHECK(calls == 0);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(done);
        const Response response = readResponse(fd);
        CHECK(response.status == 404);
        CHECK(field<int>(*served.program, *served.instance, "Gate", "fired") == 4);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");
}

TEST_CASE("a route sees through a when as through an if without an else, and through a switch on a string as through "
          "one on an int",
          "[codegen][serve][when][string]")
{
    const auto routed = [](const std::string& block) {
        return "module M\n{\n    void f(http_server s, int n, string t)\n    {\n        serve (s) { route \"GET /a\" "
               + block + " }\n    }\n};\n";
    };
    // A reply in a when's statement is allowed; a switch on a string with a default replies on every path.
    for (const char* block : {"{ when (n > 0) reply 204; reply 200; }", "{ when (n > 0) { reply 204; } reply 200; }",
                              "{ switch (t) { case \"a\": reply 1; case \"b\": reply 2; default: reply 3; } }",
                              "{ switch (t) { case \"a\": when (n > 0) reply 1; reply 2; default: reply 3; } }"})
    {
        INFO(block);
        std::string errors;
        CHECK(compile(routed(block), &errors) != nullptr);
        CHECK(errors.empty());
    }
    // A when may not run its statement; a switch on a string without a default may take no case.
    for (const char* block : {"{ when (n > 0) reply 204; }", "{ when (n > 0) { reply 204; } }",
                              "{ switch (t) { case \"a\": reply 1; case \"b\": reply 2; } }",
                              "{ switch (t) { case \"a\": reply 1; default: when (n > 0) reply 2; } }"})
    {
        INFO(block);
        const std::size_t close = routed(block).find(" }\n    }\n") - 1;   // the route block's closing brace
        const std::size_t lineStart = routed(block).rfind('\n', close) + 1;
        CHECK(errorsOf(routed(block)) == "test.fm:5:" + std::to_string(close - lineStart + 1)
                                             + ": error: route 'GET /a' does not reply in all control paths\n");
    }
}

TEST_CASE("a JSON literal's values a later version writes are each an error that says so", "[codegen][serve][json]")
{
    CHECK(errorsOf("enum Wave { SINE, SAW };\n"
                   "module M { float x; };\n"
                   "void f(http_server s, float* p)\n"
                   "{\n"
                   "    float a[3];\n"
                   "    float4 v;\n"
                   "    M m;\n"
                   "    enum Wave w = SAW;\n"
                   "    serve (s) { route \"GET /a\" { reply 200, json { \"a\": a, \"v\": v, \"m\": m, \"w\": w, "
                   "\"p\": p, \"s\": s }; } }\n"
                   "}\n")
          == "test.fm:9:57: error: a JSON literal does not take an array yet, which a later version writes: write its "
             "elements, [a[0], a[1], ...]\n"
             "test.fm:9:65: error: a JSON literal does not take a vector yet, which a later version writes: write its "
             "lanes, [v.x, v.y, ...]\n"
             "test.fm:9:73: error: a JSON literal does not take a module yet, which a later version writes: write its "
             "fields, { \"name\": m.name, ... }\n"
             "test.fm:9:81: error: a JSON literal does not take a value of an enumerated type yet, which a later "
             "version writes as its constant's name: cast it to int for its number\n"
             "test.fm:9:89: error: a JSON literal's value is a float, a double, an int, a bool, a string, an object or "
             "an array, not 'float *'\n"
             "test.fm:9:97: error: a JSON literal's value is a float, a double, an int, a bool, a string, an object or "
             "an array, not 'http_server'\n");
}

TEST_CASE("serve meets string, char and when: each error once, at its column", "[codegen][serve][when][string]")
{
    // A char parameter is no route's, as any other type but theirs; a string's default is a string, it has no range,
    // and its hole is a whole segment; a when in a route of a function that is no method has nowhere to keep its state.
    const std::string kinds = "a route's parameter is an int, a float, a bool, a string or an http_request, not ";
    CHECK(errorsOf("void f(http_server s, int n)\n"
                   "{\n"
                   "    serve (s)\n"
                   "    {\n"
                   "        route \"GET /a\" (string text, const char c, char* p, string q [[fminus::default(1)]]) "
                   "{ reply 1; }\n"
                   "        route \"GET /b/{k}x\" (string k [[fminus::range(0, 1)]]) { reply 1; }\n"
                   "        route \"GET /c\" (int k) { when (k > 1) reply 1; reply 2; }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:5:49: error: " + kinds + "'char'\n"
             "test.fm:5:58: error: " + kinds + "'char *'\n"
             "test.fm:5:72: error: the default of a string parameter is a string literal, as fminus::default(\"saw\")\n"
             "test.fm:6:23: error: a string's hole is a whole segment of the path, as \"/presets/{name}\": it takes "
             "all the segment's bytes\n"
             "test.fm:6:41: error: 'fminus::range' does not apply to a string parameter: a string has no range\n"
             "test.fm:7:34: error: a 'when' keeps its state, one bool, in the instance of the module whose method it "
             "is in, and 'f' is no method: move it into a method of a module\n");
    // A string in a JSON literal, a literal, a string or a ?: of literals, is a JSON string (a char element an int):
    // no error; a string where a reply's content type or format goes is the text form's non-literal, and among a
    // format's values for a %g the error another non-number is; a string as a serve's server or timeout, the error
    // each gives.
    const std::string literal = "argument to 'http_reply_text' must be a string literal\n";
    CHECK(errorsOf("module M\n"
                   "{\n"
                   "    http_server server;\n"
                   "    char name[4];\n"
                   "    void f(bool b) [[clang::nonblocking]]\n"
                   "    {\n"
                   "        string t = \"text/plain\";\n"
                   "        serve (server)\n"
                   "        {\n"
                   "            route \"GET /a\" (int n)\n"
                   "            {\n"
                   "                if (n == 1) reply 200, json { \"a\": \"lit\", \"c\": name[0] };\n"
                   "                if (n == 2) reply 200, json { \"a\": t, \"b\": [b ? \"x\" : \"y\", string_slice(t, 0, "
                   "1)] };\n"
                   "                if (n == 3) reply 200, t, \"%d\", n;\n"
                   "                if (n == 4) reply 200, \"text/plain\", t, n;\n"
                   "                if (n == 5) reply 200, \"text/plain\", \"%d %g\", n, t;\n"
                   "                if (n == 6) reply 200, string_slice(t, 0, 4), \"%d\", n;\n"
                   "                reply 200, b ? t : \"text/html\", \"%d\", n;\n"
                   "            }\n"
                   "        }\n"
                   "    }\n"
                   "    void g()\n"
                   "    {\n"
                   "        string t = \"x\";\n"
                   "        serve (\"server\") { route \"GET /b\" { reply 1; } }\n"
                   "        serve (t) { route \"GET /c\" { reply 1; } }\n"
                   "        serve (server, \"10\") { route \"GET /d\" { reply 1; } }\n"
                   "        serve (server, t) { route \"GET /e\" { reply 1; } }\n"
                   "    }\n"
                   "};\n")
          == "test.fm:14:40: error: " + literal + "test.fm:15:54: error: " + literal
                 + "test.fm:16:66: error: format specifies type 'double' but the argument has type 'string'\n"
                 + "test.fm:17:40: error: " + literal + "test.fm:18:28: error: " + literal
                 + "test.fm:25:16: error: passing 'char[7]' to parameter of incompatible type 'http_server'\n"
                   "test.fm:26:16: error: passing 'string' to parameter of incompatible type 'http_server'\n"
                   "test.fm:27:24: error: incompatible pointer to integer conversion passing 'char[3]' to parameter of "
                   "type 'int'\n"
                   "test.fm:28:24: error: passing 'string' to parameter of incompatible type 'int'\n");
}

TEST_CASE("a route's parameters and a module's fields: one rule for their defaults and the json:: attributes",
          "[codegen][serve][json-read][string]")
{
    // A string field takes no default (json_read leaves it out), nor a range; a number's default is no string, a
    // field's or a parameter's; a route's string parameter takes a string literal of the query's, and no range;
    // json::name and json::skip are a module's fields', unknown on a route's parameter. Each error once, at its column.
    const std::string field = "'fminus::default' does not apply to a string field: json_read leaves a string field "
                              "out, so its default would never be used\n";
    const std::string unknown = ": a route's parameter takes fminus::range(min, max) and fminus::default(value)\n";
    CHECK(errorsOf("module Fields\n"
                   "{\n"
                   "    string s [[fminus::default(\"x\")]];\n"
                   "    string t [[fminus::default(1)]] [[fminus::range(0, 1)]];\n"
                   "    float f [[fminus::default(\"x\")]];\n"
                   "    param float g [[fminus::default(\"x\")]];\n"
                   "    param string h [[fminus::default(\"x\")]];\n"
                   "    float k [[json::name(\"kk\")]];\n"
                   "};\n"
                   "\n"
                   "void routes(http_server server)\n"
                   "{\n"
                   "    serve (server)\n"
                   "    {\n"
                   "        route \"GET /a\" (int n [[fminus::default(\"x\")]], string s [[fminus::default(1)]]) "
                   "{ reply 204; }\n"
                   "        route \"GET /b\" (string s [[json::name(\"x\")]], float f [[json::skip]]) { reply 204; }\n"
                   "        route \"GET /c\" (string s [[fminus::default(\"ok\")]], string t [[fminus::range(0, 1)]]) "
                   "{ reply 204; }\n"
                   "        route \"GET /d/{s}\" (string s [[fminus::default(1)]]) { reply 204; }\n"
                   "    }\n"
                   "}\n")
          == "test.fm:3:16: error: " + field + "test.fm:4:16: error: " + field
                 + "test.fm:4:39: error: 'fminus::range' attribute only applies to fields of numbers (float, double, "
                   "int, a float vector and arrays of them), not 'string'\n"
                   "test.fm:5:15: error: 'fminus::default' takes one number\n"
                   "test.fm:6:21: error: 'fminus::default' takes one number\n"
                   "test.fm:7:18: error: a parameter is a float, an int or a bool, not 'string'\n"
                   "test.fm:15:33: error: 'fminus::default' takes one number\n"
                   "test.fm:15:68: error: the default of a string parameter is a string literal, as "
                   "fminus::default(\"saw\")\n"
                   "test.fm:16:36: error: unknown attribute 'json::name'" + unknown
                 + "test.fm:16:65: error: unknown attribute 'json::skip'" + unknown
                 + "test.fm:17:72: error: 'fminus::range' does not apply to a string parameter: a string has no "
                   "range\n"
                   "test.fm:18:40: error: 'fminus::default' does not apply to a hole's parameter: a request the route "
                   "takes always has its segment\n");
}

#if FMINUS_HAS_JSON && FMINUS_HAS_FILES_WRITE
namespace
{
    // Where recipes 48 and 50 meet: a route with a string parameter of its path and one of its query, and the request,
    // whose body is parsed and read into a module with json_read, then stored by a file name with a hole.
    constexpr const char* kStore =
        "enum Wave { SINE, SAW, SQUARE };\n"
        "module Preset\n"
        "{\n"
        "    char label[16];\n"
        "    float gain [[fminus::range(0, 1)]];\n"
        "    enum Wave wave [[fminus::default(1)]];\n"
        "    int steps [[json::name(\"n\")]] [[fminus::default(4)]];\n"
        "};\n"
        "module Store\n"
        "{\n"
        "    http_server server;\n"
        "    int stored;\n"
        "    void init() { server = http_listen(); }\n"
        "    int port() { return http_port(server); }\n"
        "    void process()\n"
        "    {\n"
        "        serve (server)\n"
        "        {\n"
        "            route \"PUT /presets/{name}\" (string name, http_request q,\n"
        "                                         string tag [[fminus::default(\"none\")]])\n"
        "            {\n"
        "                json_doc doc = json_parse(http_body(q), http_size(q));\n"
        "                defer json_free(doc);\n"
        "                Preset p;\n"
        "                if (!json_read(doc, \"\", &p))\n"
        "                    reply 422, json { \"error\": \"incomplete\", name, tag };\n"
        "                switch (file_write(\"presets/{name}.json\", http_body(q), http_size(q)))\n"
        "                {\n"
        "                case FILE_OK:\n"
        "                    stored += 1;\n"
        "                    reply 201, json { name, tag, \"first\": p.label[0], \"gain\": p.gain,\n"
        "                                      \"wave\": (int)p.wave, \"n\": p.steps };\n"
        "                case FILE_INVALID:\n"
        "                    reply 400, json { \"error\": \"not a name\", name };\n"
        "                default:\n"
        "                    reply 500;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "};\n";

    // A directory of this test's own beside bin/ (the build tree), removed when it goes.
    class StoreDirectory
    {
    public:
        StoreDirectory()
        {
            namespace fs = std::filesystem;
            fs::path tree;
#if defined(__linux__)
            std::error_code error;
            const fs::path self = fs::read_symlink("/proc/self/exe", error);
            if (!error)
                tree = self.parent_path().parent_path();
#endif
            if (tree.empty())
                tree = fs::current_path();
            path_ = tree / "serve-test" / ("t" + std::to_string(::getpid()));
            fs::remove_all(path_);
            fs::create_directories(path_);
            path_ = fs::canonical(path_);
        }
        ~StoreDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
            std::filesystem::remove(path_.parent_path(), ignored);
        }
        StoreDirectory(const StoreDirectory&) = delete;
        StoreDirectory& operator=(const StoreDirectory&) = delete;

        const std::filesystem::path& path() const { return path_; }

    private:
        std::filesystem::path path_;
    };
} // namespace

TEST_CASE("a route with string parameters reads its body into a module with json_read and stores it by a name with a "
          "hole",
          "[serve][web][server][json-read][holes][files]")
{
    const StoreDirectory directory;
    fminus::Program::Options options;
    options.files.folders = {{"presets", directory.path().string(), true, {}}};
    Served served = serveProgram(kStore, "Store", options);
    const Pump pump([&] { served.process(served.instance->memory()); });
    const int port = served.port;
    const auto stored = [&] { return field<int>(*served.program, *served.instance, "Store", "stored"); };
    const auto contents = [&](const std::string& name) {
        std::ifstream in(directory.path() / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    // Whole: the module filled (a char array, a number in its range, an enumeration by name, a default), the body
    // stored as presets/warm.json, and the reply's strings the route's parameters.
    const std::string warm = R"({"label":"warm pad","gain":0.25,"wave":"SQUARE","extra":[1,2]})";
    Response response = request(port, "PUT", "/presets/warm", warm);
    CHECK(response.status == 201);
    CHECK(response.body == R"({"name":"warm","tag":"none","first":119,"gain":0.25,"wave":2,"n":4})");
    CHECK(contents("warm.json") == warm);
    CHECK(stored() == 1);

    // The query's string as sent; a gain outside its range does not fill the field, json_read says so, and nothing is
    // stored.
    response = request(port, "PUT", "/presets/cold?tag=a%20b", R"({"label":"c","gain":2,"n":8})");
    CHECK(response.status == 422);
    CHECK(response.body == R"({"error":"incomplete","name":"cold","tag":"a%20b"})");
    CHECK_FALSE(std::filesystem::exists(directory.path() / "cold.json"));

    // A name the file's rules refuse (a hidden file): FILE_INVALID, the route's 400, nothing written.
    response = request(port, "PUT", "/presets/.hidden", warm);
    CHECK(response.status == 400);
    CHECK(response.body == R"({"error":"not a name","name":".hidden"})");
    CHECK_FALSE(std::filesystem::exists(directory.path() / ".hidden.json"));

    // Another whole one, the enumeration by its number and the field by its json::name.
    const std::string bright = R"({"gain":1,"label":"b","wave":0,"n":16})";
    response = request(port, "PUT", "/presets/bright-2?tag=x", bright);
    CHECK(response.status == 201);
    CHECK(response.body == R"({"name":"bright-2","tag":"x","first":98,"gain":1,"wave":0,"n":16})");
    CHECK(contents("bright-2.json") == bright);
    CHECK(stored() == 2);
}
#endif
#endif
