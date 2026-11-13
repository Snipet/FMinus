// tests/unit/WebServeTest.cpp: programs that serve (docs/research/web/server.md, §4 and §7), through the language and
// the host: a program compiled with fminus::Program and Options::web.server listens on loopback, and its requests are
// taken two ways, in a [[clang::nonblocking]] method a host thread calls in a loop as an audio thread calls process(),
// and by a loader thread in http_accept; another Program in the same process drives it with the library's own client
// (http_get, http_post). The library answers 404 and 405 itself from the program's route table, and serves the host's
// static files; webPort() says where the program listens, and stopWeb() wakes the loader and closes the server. A
// polling loop that a wrong attribute on http_next would let LLVM hoist out of it sees a request arrive. Then the same
// kind of program as an executable, started with its environment (FMINUS_WEB_LISTEN, FMINUS_WEB_ROOT, FMINUS_WEB_TOKEN)
// and driven over loopback, and refused without it. Nothing here binds anything but 127.0.0.1, or connects anywhere
// else. In a build without FMINUS_WEB_SERVER nothing here is compiled (tests/unit/HttpServerTest.cpp tests the
// builtins' names there), nor on Windows, whose sockets and processes it is not written for.
#include "fminus/Config.h"

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)

#include "HeapCalls.h"
#include "fminus/FMinus.h"
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "support/Aot.h"
#include "support/HostTarget.h"
#include "web/Web.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Target/TargetMachine.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace
{
#if FMINUS_HAS_JSON
    // The program that serves: a synth whose process() takes the requests waiting, between blocks, and whose load()
    // takes them on a thread of the host's, in http_accept, until its server closes.
    constexpr const char* kSynth =
        "module Synth\n"
        "{\n"
        "    void* server;\n"
        "    float gain;\n"
        "    float level;\n"
        "    int taken;\n"
        "    int preset;\n"
        "    float table[8];\n"
        "    void init() { server = http_listen(); gain = 1.0f; level = 0.25f; }\n"
        "    int port() { return http_port(server); }\n"
        "    void process(float* out, int n) [[clang::nonblocking]]\n"
        "    {\n"
        "        for (void* r = http_next(server); r; r = http_next(server))\n"
        "        {\n"
        "            taken = taken + 1;\n"
        "            if (http_is(r, \"POST /gain\"))\n"
        "            {\n"
        "                gain = http_query_float(r, \"value\", gain);\n"
        "                http_reply(r, 204);\n"
        "            }\n"
        "            else if (http_is(r, \"GET /state\"))\n"
        "                http_reply_text(r, 200, \"application/json\", \"{\\\"gain\\\":%g,\\\"level\\\":%.9g,"
        "\\\"taken\\\":%d}\", gain, level, taken);\n"
        "            else if (http_is(r, \"POST /table/%d\"))\n"
        "                http_reply(r, bytes_floats(http_body(r), http_size(r), &table[http_arg(r, 0) & 7], 1) == 1 ? "
        "204 : 400);\n"
        "            else if (http_is(r, \"GET /table\"))\n"
        "                http_reply_bytes(r, 200, table, 32, \"application/octet-stream\");\n"
        "            else\n"
        "                http_reply(r, 500);\n"
        "        }\n"
        "        for (int i = 0; i < n; ++i)\n"
        "            out[i] = gain;\n"
        "    }\n"
        "    // A request waiting is answered, after at most `tries` looks: the look must happen each time round.\n"
        "    int poll(int tries) [[clang::nonblocking]]\n"
        "    {\n"
        "        for (int i = 0; i < tries; ++i)\n"
        "        {\n"
        "            void* r = http_next(server);\n"
        "            if (r)\n"
        "            {\n"
        "                http_reply(r, http_is(r, \"POST /gain\") ? 202 : 500);\n"
        "                return i;\n"
        "            }\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "    int load()\n"
        "    {\n"
        "        int served = 0;\n"
        "        for (void* r = http_accept(server, -1); r; r = http_accept(server, -1))\n"
        "        {\n"
        "            if (http_is(r, \"POST /preset/%d\"))\n"
        "            {\n"
        "                preset = http_arg(r, 0) + http_query_int(r, \"bank\", 0) * 100;\n"
        "                http_reply_text(r, 201, \"\", \"preset %d of %d\", preset, served + 1);\n"
        "            }\n"
        "            else\n"
        "                http_reply(r, 500);\n"
        "            served = served + 1;\n"
        "        }\n"
        "        return served;\n"
        "    }\n"
        "};\n";

    // The client, another program: each function makes one request of the server at `port` and gives its status (or
    // what it read of the body).
    constexpr const char* kClient =
        "int post_gain(int port, int value)\n"
        "{\n"
        "    float none[1] = {0.0f};\n"
        "    void* r = http_post(\"http://127.0.0.1:%d/gain?value=%d\", none, 0, \"\", port, value);\n"
        "    const int status = http_wait(r, 10000);\n"
        "    http_close(r);\n"
        "    return status;\n"
        "}\n"
        "int post_table(int port, int k, float x)\n"
        "{\n"
        "    float data[1] = {x};\n"
        "    void* r = http_post(\"http://127.0.0.1:%d/table/%d\", data, 4, \"application/octet-stream\", port, k);\n"
        "    const int status = http_wait(r, 10000);\n"
        "    http_close(r);\n"
        "    return status;\n"
        "}\n"
        "float table(int port, int k)\n"
        "{\n"
        "    void* r = http_get(\"http://127.0.0.1:%d/table\", port);\n"
        "    float out[8];\n"
        "    float got = -1000.0f;\n"
        "    if (http_wait(r, 10000) == 200 && bytes_floats(http_body(r), http_size(r), out, 8) == 8)\n"
        "        got = out[k & 7];\n"
        "    http_close(r);\n"
        "    return got;\n"
        "}\n"
        "float state(int port, int what)\n"
        "{\n"
        "    void* r = http_get(\"http://127.0.0.1:%d/state\", port);\n"
        "    float got = -1000.0f;\n"
        "    if (http_wait(r, 10000) == 200)\n"
        "    {\n"
        "        void* doc = json_parse(http_body(r), http_size(r));\n"
        "        got = what == 0 ? json_float(doc, \"/gain\", -2.0f)\n"
        "            : what == 1 ? json_float(doc, \"/level\", -2.0f) : (float)json_int(doc, \"/taken\", -2);\n"
        "        json_free(doc);\n"
        "    }\n"
        "    http_close(r);\n"
        "    return got;\n"
        "}\n"
        "int get(int port, int which)\n"
        "{\n"
        "    void* r = which == 0 ? http_get(\"http://127.0.0.1:%d/nothing\", port)\n"
        "            : which == 1 ? http_get(\"http://127.0.0.1:%d/gain\", port)\n"
        "            : http_get(\"http://127.0.0.1:%d/\", port);\n"
        "    const int status = http_wait(r, 10000);\n"
        "    const int size = http_size(r);\n"
        "    http_close(r);\n"
        "    return status * 1000 + size;\n"
        "}\n"
        "int post_preset(int port, int k, int bank)\n"
        "{\n"
        "    float none[1] = {0.0f};\n"
        "    void* r = http_post(\"http://127.0.0.1:%d/preset/%d?bank=%d\", none, 0, \"\", port, k, bank);\n"
        "    const int status = http_wait(r, 10000);\n"
        "    const int size = http_size(r);\n"
        "    http_close(r);\n"
        "    return status * 1000 + size;\n"
        "}\n";
#endif

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

    // A port no one listens on now, on 127.0.0.1: one the system gave a socket of the test's, closed again.
    int freePort()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        socklen_t length = sizeof address;
        REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        ::close(fd);
        return ntohs(address.sin_port);
    }
} // namespace

#if FMINUS_HAS_JSON
TEST_CASE("a program serves through the JIT: its audio thread takes requests between blocks, a loader thread in "
          "http_accept, and another program's client drives both",
          "[web][server][library]")
{
    fminus::Program::Options options;
    options.web.server.listen = "127.0.0.1:0";
    options.web.server.files = {{"/", "text/html; charset=utf-8", "<p>synth</p>"}};
    auto synth = compileProgram(kSynth, options);
    REQUIRE(synth->webRoutes().size() == 6);
    CHECK(synth->webRoutes()[0].method == "POST");
    CHECK(synth->webRoutes()[0].pattern == "/gain");
    CHECK(synth->webRoutes()[2].pattern == "/table/*");
    CHECK(synth->webPort() == 0);   // before it listens

    std::string errors;
    using Process = void(void*, float*, int);
    auto* init = synth->function<void(void*)>("Synth::init", errors);
    auto* port = synth->function<int(void*)>("Synth::port", errors);
    auto* process = synth->function<Process>("Synth::process", errors);
    auto* poll = synth->function<int(void*, int)>("Synth::poll", errors);
    auto* load = synth->function<int(void*)>("Synth::load", errors);
    REQUIRE(init);
    REQUIRE(port);
    REQUIRE(process);
    REQUIRE(poll);
    REQUIRE(load);
    const fminus::Program::Module* module = synth->module("Synth");
    REQUIRE(module != nullptr);
    Instance instance(*module);
    init(instance.memory());
    const int listening = port(instance.memory());
    REQUIRE(listening > 0);
    CHECK(synth->webPort() == listening);

    fminus::Program::Options clientOptions;
    clientOptions.web.allow = {"http://127.0.0.1:" + std::to_string(listening) + "/*"};
    auto client = compileProgram(kClient, clientOptions);
    auto* postGain = client->function<int(int, int)>("post_gain", errors);
    auto* postTable = client->function<int(int, int, float)>("post_table", errors);
    auto* table = client->function<float(int, int)>("table", errors);
    auto* state = client->function<float(int, int)>("state", errors);
    auto* get = client->function<int(int, int)>("get", errors);
    auto* postPreset = client->function<int(int, int, int)>("post_preset", errors);
    REQUIRE(postGain);
    REQUIRE(postTable);
    REQUIRE(table);
    REQUIRE(state);
    REQUIRE(get);
    REQUIRE(postPreset);

    SECTION("the audio thread")
    {
        // The host's audio thread: process() a block at a time, about a millisecond apart.
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            float out[64];
            while (!stop.load())
            {
                process(instance.memory(), out, 64);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        CHECK(postGain(listening, 3) == 204);
        CHECK(state(listening, 0) == 3.0f);
        CHECK(state(listening, 1) == 0.25f);
        CHECK(postTable(listening, 5, 0.75f) == 204);
        CHECK(postTable(listening, 13, -2.5f) == 204);   // 13 & 7: the table's 5th again
        CHECK(table(listening, 5) == -2.5f);
        CHECK(table(listening, 4) == 0.0f);
        CHECK(state(listening, 2) == 8.0f);   // the requests the program took: each of the above, and this one
        // What no route names never reaches the program: the library answers it.
        CHECK(get(listening, 0) == 404000);   // GET /nothing
        CHECK(get(listening, 1) == 405000);   // GET /gain: POST only
        CHECK(get(listening, 2) == 200000 + 12);   // the host's file, served by the library
        CHECK(state(listening, 2) == 9.0f);
        stop.store(true);
        audio.join();
    }

    SECTION("a polling loop looks each time round")
    {
        // A wrong attribute on http_next (readonly, say) would let LLVM look once, before the loop, and miss it.
        int status = 0;
        std::thread request([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            status = postGain(listening, 9);
        });
        const int looks = poll(instance.memory(), 2000000000);
        request.join();
        CHECK(looks > 0);
        CHECK(status == 202);
    }

    SECTION("a loader thread in http_accept, woken by stopWeb")
    {
        int served = -1;
        std::thread loader([&] { served = load(instance.memory()); });
        CHECK(postPreset(listening, 7, 2) == 201000 + 15);   // "preset 207 of 1"
        CHECK(postPreset(listening, 3, 0) == 201000 + 13);   // "preset 3 of 2"
        synth->stopWeb();   // the loader's http_accept(-1) returns null
        loader.join();
        CHECK(served == 2);
        CHECK(synth->webPort() == 0);
        CHECK(port(instance.memory()) == FMINUS_WEB_CANCELLED);
        CHECK(get(listening, 2) / 1000 == FMINUS_WEB_TRANSPORT);   // no one listens there now
    }
}
#endif

TEST_CASE("a Program with a listen and nothing to reach has a context; destroying it closes the server", "[web][server]")
{
    fminus::Program::Options options;
    options.web.server.listen = "127.0.0.1:0";
    auto program = compileProgram("int start() { return http_port(http_listen()); }\n"
                                  "void* next() { return http_next(http_listen()); }\n"
                                  "void* fetch(int port) { return http_get(\"http://127.0.0.1:%d/\", port); }\n"
                                  "int status(void* r) { const int s = http_wait(r, 1000); http_close(r); return s; }\n",
                                  options);
    std::string errors;
    auto* start = program->function<int()>("start", errors);
    auto* next = program->function<void*()>("next", errors);
    auto* fetch = program->function<void*(int)>("fetch", errors);
    auto* status = program->function<int(void*)>("status", errors);
    REQUIRE(start);
    REQUIRE(next);
    REQUIRE(fetch);
    REQUIRE(status);
    const int port = start();
    REQUIRE(port > 0);
    CHECK(start() == port);   // a context listens once: its second http_listen is its first's server
    CHECK(program->webPort() == port);
    CHECK(next() == nullptr);
    CHECK(status(fetch(port)) == FMINUS_WEB_REFUSED);   // it may listen, and reach nothing
    program.reset();
    // Gone with the Program: nothing listens there.
    fminus::Program::Options clientOptions;
    clientOptions.web.allow = {"http://127.0.0.1:" + std::to_string(port) + "/"};
    auto client = compileProgram("int status(int port)\n"
                                 "{\n"
                                 "    void* r = http_get(\"http://127.0.0.1:%d/\", port);\n"
                                 "    const int s = http_wait(r, 5000);\n"
                                 "    http_close(r);\n"
                                 "    return s;\n"
                                 "}\n",
                                 clientOptions);
    auto* probe = client->function<int(int)>("status", errors);
    REQUIRE(probe);
    CHECK(probe(port) == FMINUS_WEB_TRANSPORT);
}

TEST_CASE("an executable serves with FMINUS_WEB_LISTEN, its files from FMINUS_WEB_ROOT, and nothing without it",
          "[web][server][aot]")
{
    const std::string source = "float main(float count, float wait)\n"
                               "{\n"
                               "    void* server = http_listen();\n"
                               "    if (http_port(server) <= 0)\n"
                               "        return (float)http_port(server);\n"
                               "    int served = 0;\n"
                               "    float sum = 0.0f;\n"
                               "    for (void* r = http_accept(server, (int)wait); r; r = http_accept(server, (int)wait))\n"
                               "    {\n"
                               "        if (http_is(r, \"POST /add/%d\"))\n"
                               "        {\n"
                               "            sum = sum + (float)http_arg(r, 0);\n"
                               "            http_reply_text(r, 200, \"text/plain\", \"%g\", sum);\n"
                               "        }\n"
                               "        else\n"
                               "            http_reply(r, 500);\n"
                               "        served = served + 1;\n"
                               "        if ((float)served >= count)\n"
                               "            return sum;\n"
                               "    }\n"
                               "    return -1000.0f + sum;\n"
                               "}\n";
    fminus::Diagnostics diagnostics("serve.fm");
    const auto program = fminus::frontend::parse(source, diagnostics);
    REQUIRE(program);
    llvm::LLVMContext llvmContext;
    auto machine = llvm::cantFail(fminus::hostTargetMachine());
    fminus::ir::Options lowering;
    lowering.polyLanes = fminus::polyLanes(*machine);
    lowering.dataLayout = machine->createDataLayout().getStringRepresentation();
    std::unique_ptr<llvm::Module> module = fminus::ir::lower(*program, llvmContext, diagnostics, lowering);
    INFO(diagnostics.format());
    REQUIRE(module);
    REQUIRE_FALSE(llvm::errorToBool(fminus::aot::addEntryPoint(*module)));
    fminus::prepareModule(*module, *machine);
    fminus::optimiseModule(*module, *machine, llvm::OptimizationLevel::O2);
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / ("fminus-web-serve-" + std::to_string(::getpid()));
    const std::filesystem::path exe = base.string() + ".exe";
    const std::filesystem::path out = base.string() + ".out";
    const std::filesystem::path root = base.string() + ".root";
    llvm::Error built = fminus::aot::emitExecutable(*module, *machine, {.output = exe.string()});
    REQUIRE_FALSE(llvm::errorToBool(std::move(built)));
    std::filesystem::create_directories(root / "app");
    std::ofstream(root / "index.html") << "<p>serve</p>";
    std::ofstream(root / "app" / "main.js") << "1;";

    const auto run = [&](const std::string& environment, const std::string& arguments) {
        const std::string command =
            environment + " '" + exe.string() + "' " + arguments + " > '" + out.string() + "' 2>&1";
        const int status = std::system(command.c_str());
        std::string text;
        if (std::FILE* file = std::fopen(out.c_str(), "rb"))
        {
            char buffer[512];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof buffer, file)) > 0)
                text.append(buffer, got);
            std::fclose(file);
        }
        return std::pair{WEXITSTATUS(status), text};
    };

    // Without FMINUS_WEB_LISTEN it listens nowhere: http_port is HTTP_REFUSED, at once.
    CHECK(run("env -u FMINUS_WEB_LISTEN", "3 100") == std::pair{0, std::string("-4\n")});
    CHECK(run("env FMINUS_WEB_LISTEN='0.0.0.0:8080'", "3 100").first == 2);
    CHECK(run("env FMINUS_WEB_LISTEN='127.0.0.1:0' FMINUS_WEB_ROOT='" + (base.string() + ".none") + "'", "3 100")
              .second.find("FMINUS_WEB_ROOT: '") != std::string::npos);

    const int port = freePort();
    fminus::Program::Options clientOptions;
    clientOptions.web.allow = {"http://127.0.0.1:" + std::to_string(port) + "/*"};
    auto client = compileProgram("int add(int port, int k)\n"
                                 "{\n"
                                 "    float none[1] = {0.0f};\n"
                                 "    void* r = http_post(\"http://127.0.0.1:%d/add/%d\", none, 0, \"\", port, k);\n"
                                 "    const int status = http_wait(r, 5000);\n"
                                 "    const int size = http_size(r);\n"
                                 "    http_close(r);\n"
                                 "    return status < 0 ? status : status * 1000 + size;\n"
                                 "}\n"
                                 "int page(int port, int which)\n"
                                 "{\n"
                                 "    void* r = which == 0 ? http_get(\"http://127.0.0.1:%d/\", port)\n"
                                 "            : http_get(\"http://127.0.0.1:%d/app/main.js\", port);\n"
                                 "    const int status = http_wait(r, 5000);\n"
                                 "    const int size = http_size(r);\n"
                                 "    http_close(r);\n"
                                 "    return status < 0 ? status : status * 1000 + size;\n"
                                 "}\n",
                                 clientOptions);
    std::string errors;
    auto* add = client->function<int(int, int)>("add", errors);
    auto* page = client->function<int(int, int)>("page", errors);
    REQUIRE(add);
    REQUIRE(page);
    // The first request waits for the executable to listen.
    const auto first = [&](auto request) {
        int got = FMINUS_WEB_TRANSPORT;
        for (int i = 0; i < 200 && got == FMINUS_WEB_TRANSPORT; ++i)
        {
            got = request();
            if (got == FMINUS_WEB_TRANSPORT)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return got;
    };

    {
        // The third request's reply may never be sent: main returns at once after it, and its context, its server
        // with it, is destroyed then, before the I/O thread's next turn (as a Program's would be).
        std::pair<int, std::string> result;
        std::thread server([&] {
            result = run("env FMINUS_WEB_LISTEN='127.0.0.1:" + std::to_string(port) + "' FMINUS_WEB_ROOT='" +
                             root.string() + "'",
                         "3 20000");
        });
        const int index = first([&] { return page(port, 0); });
        const int script = page(port, 1);
        const int one = add(port, 40);
        const int two = add(port, 2);
        const int last = add(port, 0);
        server.join();
        CHECK((last == 200000 + 2 || last == FMINUS_WEB_TRANSPORT));
        CHECK(index == 200000 + 12);    // the root's index.html at "/", served by the library
        CHECK(script == 200000 + 2);
        CHECK(one == 200000 + 2);       // "40"
        CHECK(two == 200000 + 2);       // "42"
        CHECK(result == std::pair{0, std::string("42\n")});
    }
    {
        // With a token, a request without it never reaches the program (401); the page is served all the same.
        std::pair<int, std::string> result;
        std::thread server([&] {
            result = run("env FMINUS_WEB_LISTEN='127.0.0.1:" + std::to_string(port) + "' FMINUS_WEB_ROOT='" +
                             root.string() + "' FMINUS_WEB_TOKEN=abcdefghijklmnop-_0123456789",
                         "1 1500");
        });
        const int index = first([&] { return page(port, 0); });
        const int refused = add(port, 5);
        server.join();
        CHECK(index == 200000 + 12);
        CHECK(refused == 401000);
        CHECK(result == std::pair{0, std::string("-1000\n")});   // accept timed out: nothing reached it
    }
    std::error_code ignored;
    std::filesystem::remove(exe, ignored);
    std::filesystem::remove(out, ignored);
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("a root directory is read once: regular files beneath it, no link, no hidden name, every path one the "
          "server serves",
          "[web][server]")
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("fminus-web-root-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root / "app" / "deep");
    std::filesystem::create_directories(root / ".git");
    std::filesystem::create_directories(root / "bad name");
    std::ofstream(root / "index.html") << "<p>";
    std::ofstream(root / "app" / "index.html") << "<p>app";
    std::ofstream(root / "app" / "deep" / "Tone.WAV") << "RIFF";
    std::ofstream(root / "app" / "x.mjs") << "x";
    std::ofstream(root / "data.bin") << "\x01\x02";
    std::ofstream(root / ".git" / "config") << "secret";
    std::ofstream(root / "bad name" / "a.txt") << "a";
    std::ofstream(root / "a%2e.txt") << "a";
    std::filesystem::create_symlink("/etc/hostname", root / "link.txt");
    std::filesystem::create_directory_symlink("/etc", root / "etc");

    std::vector<fminus::web::ServerPolicy::File> files;
    std::vector<std::string> skipped;
    REQUIRE(fminus::web::readServerRoot(root.string(), files, skipped).empty());
    std::vector<std::string> paths;
    for (const auto& file : files)
        paths.push_back(file.path + " " + file.type + " " + std::to_string(file.bytes.size()));
    CHECK(paths == std::vector<std::string>{"/ text/html; charset=utf-8 3", "/app text/html; charset=utf-8 6",
                                            "/app/deep/Tone.WAV audio/wav 4",
                                            "/app/index.html text/html; charset=utf-8 6",
                                            "/app/x.mjs text/javascript; charset=utf-8 1",
                                            "/data.bin application/octet-stream 2",
                                            "/index.html text/html; charset=utf-8 3"});
    std::string notes;
    for (const std::string& note : skipped)
        notes += note + "\n";
    INFO(notes);
    CHECK(skipped.size() == 5);
    CHECK(notes.find("'.git': hidden") != std::string::npos);
    CHECK(notes.find("'bad name': not a path the server serves") != std::string::npos);
    CHECK(notes.find("'a%2e.txt': not a path") != std::string::npos);
    CHECK(notes.find("'link.txt': a symbolic link, which is never followed") != std::string::npos);
    CHECK(notes.find("'etc': a symbolic link") != std::string::npos);
    // The policy takes what it read.
    fminus::web::Policy policy;
    policy.server.listen = "127.0.0.1:0";
    policy.server.files = files;
    CHECK(fminus::web::checkPolicy(policy).empty());

    // Too many files is an error, not a part of them served; so is no directory.
    for (int i = 0; i < 260; ++i)
        std::ofstream(root / ("f" + std::to_string(i) + ".txt")) << "f";
    std::vector<fminus::web::ServerPolicy::File> none;
    CHECK(fminus::web::readServerRoot(root.string(), none, skipped).ends_with("has more than 256 files to serve"));
    CHECK(none.empty());
    CHECK(fminus::web::readServerRoot((root / "index.html").string(), none, skipped).ends_with("is not a directory"));
    CHECK(fminus::web::readServerRoot("", none, skipped) == "'' is not a directory");
    CHECK(fminus::web::readServerRoot((root / "nothing").string(), none, skipped).ends_with("is not a directory"));

    // More than 16 MiB in all; more than 4096 entries looked at, skipped ones too.
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    std::ofstream(root / "a.bin") << std::string(8u << 20, 'a');
    std::ofstream(root / "b.bin") << std::string((8u << 20) + 1, 'b');
    CHECK(fminus::web::readServerRoot(root.string(), none, skipped).ends_with("has more than 16 MiB of files to serve"));
    std::filesystem::remove(root / "b.bin");
    REQUIRE(fminus::web::readServerRoot(root.string(), none, skipped).empty());
    CHECK(none.size() == 1);
    none.clear();
    std::filesystem::create_directories(root / "many");
    for (int i = 0; i < 4096; ++i)
        std::ofstream(root / "many" / ("x " + std::to_string(i))) << "";
    CHECK(fminus::web::readServerRoot(root.string(), none, skipped).ends_with("has more than 4096 entries"));
    CHECK(none.empty());
    std::filesystem::remove_all(root, ignored);
}

#if defined(__linux__)
TEST_CASE("a [[clang::nonblocking]] handler, compiled by the JIT, takes, reads and answers a request without "
          "allocating or making a system call",
          "[web][server][library]")
{
    // WebServerTest.cpp proves it of the library's functions called from C++; this is the same proof of the code a
    // program's builtins become: the JIT'd handler's calls, through the definitions the JIT gives them.
    constexpr const char* kTaker =
        "module Taker\n"
        "{\n"
        "    void* server;\n"
        "    float table[2];\n"
        "    void init() { server = http_listen(); table[0] = 1.5f; table[1] = -2.0f; }\n"
        "    int port() { return http_port(server); }\n"
        "    // One request, if one waits: its reads summed into *sum and a reply of `kind`; 1 if it was answered.\n"
        "    int handle(int kind, double* sum) [[clang::nonblocking]]\n"
        "    {\n"
        "        void* r = http_next(server);\n"
        "        if (!r)\n"
        "            return 0;\n"
        "        double s = http_port(server);\n"
        "        s = s + (http_is(r, \"GET /hello\") ? 1 : 0) + (http_is(r, \"POST /table/%d\") ? 10 : 0);\n"
        "        s = s + http_arg(r, 0) + http_query_float(r, \"v\", 0.0f) + http_query_int(r, \"n\", 0);\n"
        "        s = s + http_size(r) + (http_body(r) ? 1 : 0);\n"
        "        int replied = -1;\n"
        "        if (kind == 0)\n"
        "            replied = http_reply(r, 204);\n"
        "        else if (kind == 1)\n"
        "            replied = http_reply_bytes(r, 200, table, 8, \"application/octet-stream\");\n"
        "        else\n"
        "            replied = http_reply_text(r, 200, \"\", \"%g %.9g %d %%\", 0.1, s, -7);\n"
        "        *sum = s;\n"
        "        return replied == 0 ? 1 : 0;\n"
        "    }\n"
        "};\n";
    fminus::Program::Options options;
    options.web.server.listen = "127.0.0.1:0";
    options.web.server.replyTimeoutMs = 20000;
    auto taker = compileProgram(kTaker, options);
    std::string errors;
    auto* init = taker->function<void(void*)>("Taker::init", errors);
    auto* port = taker->function<int(void*)>("Taker::port", errors);
    auto* handle = taker->function<int(void*, int, double*)>("Taker::handle", errors);
    REQUIRE(init);
    REQUIRE(port);
    REQUIRE(handle);
    const fminus::Program::Module* module = taker->module("Taker");
    REQUIRE(module != nullptr);
    Instance instance(*module);
    init(instance.memory());
    const int listening = port(instance.memory());
    REQUIRE(listening > 0);
    const double expected = listening + 10.0 + 12.0 + 0.75 - 3.0 + 4.0 + 1.0;
    const std::string post = "POST /table/12?v=0.75&n=-3 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(listening) +
                             "\r\nContent-Type: a/b\r\nContent-Length: 4\r\n\r\nwxyz";
    std::vector<int> clients;
    const auto send = [&]
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(listening));
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        REQUIRE(::send(fd, post.data(), post.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(post.size()));
        clients.push_back(fd);
    };

    // In this process, counted on this thread, for each kind of reply.
    if (fminus::test::HeapCalls::counted())
    {
        for (int kind = 0; kind < 3; ++kind)
        {
            send();
            bool done = false;
            for (int attempt = 0; attempt < 500 && !done; ++attempt)
            {
                double sum = 0;
                long calls = 0;
                {
                    const fminus::test::HeapCalls counting;
                    done = handle(instance.memory(), kind, &sum) == 1;
                    calls = counting.count();
                }
                if (done)
                {
                    INFO("kind " << kind);
                    CHECK(calls == 0);
                    CHECK(sum == expected);
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
    for (int kind = 0; kind < 3; ++kind)
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
                result[0] = handle(instance.memory(), kind, &result[1]);
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
                kind = 3;
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
    bool answered = false;
    for (int attempt = 0; attempt < 500 && !answered; ++attempt)
    {
        double sum = 0;
        answered = handle(instance.memory(), 0, &sum) == 1;
        if (!answered)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(answered);
    for (int fd : clients)
        ::close(fd);
}

TEST_CASE("a directory swapped for a link while the root is read never takes the reader out of the root",
          "[web][server]")
{
    // Someone who can write the root exchanges its directory 'sub' with a link to a directory outside it, over and
    // over, while the root is read: a reader that lists 'sub' and then opens 'sub/key.txt' by its path would follow
    // the link that took its place, and serve the outside file of that name and size. The reader holds each directory
    // it lists open and opens its files beneath that, so it reads the real 'sub', or skips the link, and never the
    // outside.
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / ("fminus-web-swap-" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
    const std::filesystem::path root = base / "root";
    const std::filesystem::path outside = base / "outside";
    std::filesystem::create_directories(root / "sub");
    std::filesystem::create_directories(outside);
    std::ofstream(root / "sub" / "key.txt") << "public";
    std::ofstream(outside / "key.txt") << "SECRET";
    std::filesystem::create_directory_symlink(outside, root / "swap");

    std::atomic<bool> stop{false};
    std::atomic<long> swaps{0};
    std::thread swapper([&] {
        const std::string sub = (root / "sub").string();
        const std::string swap = (root / "swap").string();
        while (!stop.load(std::memory_order_relaxed))
        {
            if (::renameat2(AT_FDCWD, sub.c_str(), AT_FDCWD, swap.c_str(), RENAME_EXCHANGE) == 0)
                swaps.fetch_add(1, std::memory_order_relaxed);
        }
    });
    int leaked = 0;
    int publicRead = 0;
    for (int round = 0; round < 3000 && leaked == 0; ++round)
    {
        std::vector<fminus::web::ServerPolicy::File> files;
        std::vector<std::string> skipped;
        if (!fminus::web::readServerRoot(root.string(), files, skipped).empty())
            continue;   // a file vanished between listing and reading: an error, which is safe
        for (const auto& file : files)
        {
            if (file.bytes == "SECRET")
                ++leaked;
            else if (file.bytes == "public")
                ++publicRead;
        }
    }
    stop = true;
    swapper.join();
    INFO("swaps " << swaps.load() << ", the real file read " << publicRead << " times");
    CHECK(swaps.load() > 0);
    CHECK(leaked == 0);
    std::filesystem::remove_all(base, ignored);
}
#endif

#endif
