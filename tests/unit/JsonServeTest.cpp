// tests/unit/JsonServeTest.cpp: the JSON literal where the web library meets it (src/ir/Shapes.cpp, Calls.cpp,
// Serve.cpp; docs/web.md, "The JSON literal"): a reply of a module, an array, an enumeration and a slice, written in the
// request's own buffer; http_reply_json, what a reply is made of, in the http_is style, and its HTTP_LIMIT over the
// host's maxReplyBytes; the wait-free proof of a reply's module in a [[clang::nonblocking]] method (HeapCalls, and a
// child under seccomp's strict mode); http_reply_json's errors. Then, with the client and file writing, the research's
// "data work" (docs/research/syntax/README.md, §8): eight sensors' days fetched at once under a within, each a JSON
// literal a loopback server of the test's own serves, summarised into an array of modules, written with file_write as
// a JSON literal and posted with http_post as one, which the server reads back with json_read; and a POST's literal
// over the policy's maxBodyBytes, which is HTTP_LIMIT before anything is sent.
//
// Nothing binds or connects anywhere but 127.0.0.1, and files are written only in a directory the test makes beside
// bin/ and removes. In a build without the web server or the JSON writer nothing here is compiled, nor on Windows.
#include "Compile.h"
#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#if FMINUS_HAS_WEB_SERVER && FMINUS_HAS_JSON && !defined(_WIN32)
#include "HeapCalls.h"
#include "fminus/FMinus.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
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

using namespace fminus::test;

namespace
{
    std::unique_ptr<fminus::Program> compileProgram(const std::string& source, const fminus::Program::Options& options)
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
        explicit Instance(const fminus::Program::Module& module) : storage(module.size + module.alignment, std::byte{0})
        {
            void* at = storage.data();
            std::size_t room = storage.size();
            REQUIRE(std::align(module.alignment, module.size, at, room) != nullptr);
            start = at;
        }
        void* memory() { return start; }
        std::vector<std::byte> storage;
        void* start = nullptr;
    };

    // What came back for a request to 127.0.0.1:`port`: its status (0 when none), its head and its body.
    struct Response
    {
        int status = 0;
        std::string head;
        std::string body;
    };

    Response request(int port, const std::string& method, const std::string& target)
    {
        Response response;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return response;
        timeval wait{20, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0)
        {
            ::close(fd);
            return response;
        }
        const std::string text = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
                                 + "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        ::send(fd, text.data(), text.size(), MSG_NOSIGNAL);
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

    // A program whose module `name` has init(), port() and a nonblocking process(), listening on 127.0.0.1.
    struct Served
    {
        std::unique_ptr<fminus::Program> program;
        std::unique_ptr<Instance> instance;
        int port = 0;
        void (*process)(void*) = nullptr;
    };

    Served serveProgram(const std::string& source, const std::string& name, fminus::Program::Options options = {})
    {
        options.web.server.listen = "127.0.0.1:0";
        Served served;
        served.program = compileProgram(source, options);
        std::string errors;
        auto* init = served.program->function<void(void*)>(name + "::init", errors);
        auto* port = served.program->function<int(void*)>(name + "::port", errors);
        served.process = served.program->function<void(void*)>(name + "::process", errors);
        INFO(errors);
        REQUIRE(init);
        REQUIRE(port);
        REQUIRE(served.process);
        served.instance = std::make_unique<Instance>(*served.program->module(name));
        init(served.instance->memory());
        served.port = port(served.instance->memory());
        REQUIRE(served.port > 0);
        return served;
    }

    template <typename T>
    T field(const fminus::Program& program, void* instance, const std::string& module, const std::string& name)
    {
        for (const fminus::Program::Module::Field& field : program.module(module)->fields)
        {
            if (field.name == name)
            {
                T value;
                std::memcpy(&value, static_cast<char*>(instance) + field.offset, sizeof value);
                return value;
            }
        }
        FAIL("no field " << name);
        return T{};
    }

    constexpr const char* kPresets = R"(
enum Wave { SINE, SAW, SQUARE };

module Preset
{
    float gain [[fminus::range(0, 1)]];
    float cutoff [[fminus::range(20, 20000)]];
    enum Wave wave;
    char name[12];
};

module Store
{
    http_server server;
    Preset presets[3];
    float taps[4];
    int count;
    int answered;
    int limited;

    void init()
    {
        server = http_listen();
        for (int k = 0; k < 3; ++k)
        {
            presets[k].gain = 0.25f * (float)(k + 1);
            presets[k].cutoff = 1000.0f * (float)(k + 1);
            presets[k].wave = (enum Wave)k;
            presets[k].name[0] = 97 + k;
        }
        taps[0] = 0.5f;
        taps[1] = -1.0f;
        taps[2] = 2.0f;
        taps[3] = 0.125f;
        count = 2;
    }
    int port() { return http_port(server); }

    void process() [[clang::nonblocking]]
    {
        serve (server)
        {
            route "GET /presets/{k}" (int k [[fminus::range(0, 2)]])
            {
                answered += 1;
                reply 200, json (presets[k]);
            }
            route "GET /presets"
            {
                answered += 1;
                reply 200, json { "count": count, "presets": json_slice(presets, count), "taps": taps,
                                  "wave": presets[count].wave, "first": presets[0].name };
            }
            route "GET /all" { answered += 1; reply 200, json [presets, json_slice(taps, count)]; }
        }
    }

    // http_reply_json in the style of http_is: what a reply is made of, its result the library's.
    void answer() [[clang::nonblocking]]
    {
        for (http_request r = http_next(server); r; r = http_next(server))
        {
            if (http_is(r, "GET /raw/%d"))
            {
                const int k = http_arg(r, 0);
                const int status = k >= 0 && k < 3 ? http_reply_json(r, 201, json { "preset": presets[k] })
                                                   : http_reply_json(r, 200, json { "presets": presets, "taps": taps });
                limited += status == HTTP_LIMIT ? 1 : 0;
                if (status != 0)
                    http_reply(r, 500);
            }
            else
                http_reply(r, 404);
            answered += 1;
        }
    }
};
)";

    constexpr const char* kPreset0 = R"({"gain":0.25,"cutoff":1000,"wave":"SINE","name":"a"})";
    constexpr const char* kPreset1 = R"({"gain":0.5,"cutoff":2000,"wave":"SAW","name":"b"})";
    constexpr const char* kPreset2 = R"({"gain":0.75,"cutoff":3000,"wave":"SQUARE","name":"c"})";
} // namespace

TEST_CASE("a reply of a module, an array of them, a slice and an enumeration, written in the request's own buffer",
          "[serve][web][server][json]")
{
    Served served = serveProgram(kPresets, "Store");
    const Pump pump([&] { served.process(served.instance->memory()); });
    const Response one = request(served.port, "GET", "/presets/1");
    CHECK(one.status == 200);
    CHECK(one.head.find("Content-Type: application/json") != std::string::npos);
    CHECK(one.body == kPreset1);
    CHECK(request(served.port, "GET", "/presets/3").status == 400);   // out of its range
    CHECK(request(served.port, "GET", "/presets").body
          == std::string(R"({"count":2,"presets":[)") + kPreset0 + "," + kPreset1
                 + R"(],"taps":[0.5,-1,2,0.125],"wave":"SQUARE","first":"a"})");
    CHECK(request(served.port, "GET", "/all").body
          == std::string("[[") + kPreset0 + "," + kPreset1 + "," + kPreset2 + "],[0.5,-1]]");
}

TEST_CASE("http_reply_json is what a reply of a JSON literal is made of: in the http_is style, and HTTP_LIMIT over "
          "maxReplyBytes",
          "[serve][web][server][json]")
{
    fminus::Program::Options options;
    options.web.server.maxReplyBytes = 128;   // a preset fits; all three and the taps do not
    Served served = serveProgram(kPresets, "Store", options);
    std::string errors;
    auto* answer = served.program->function<void(void*)>("Store::answer", errors);
    INFO(errors);
    REQUIRE(answer);
    const Pump pump([&] { answer(served.instance->memory()); });
    const Response raw = request(served.port, "GET", "/raw/2");
    CHECK(raw.status == 201);
    CHECK(raw.head.find("Content-Type: application/json") != std::string::npos);
    CHECK(raw.body == std::string(R"({"preset":)") + kPreset2 + "}");
    CHECK(request(served.port, "GET", "/raw/9").status == 500);   // HTTP_LIMIT, then the program's 500
    CHECK(field<int>(*served.program, served.instance->memory(), "Store", "limited") == 1);
    CHECK(request(served.port, "GET", "/raw/0").body == std::string(R"({"preset":)") + kPreset0 + "}");
    CHECK(request(served.port, "GET", "/presets").status == 404);   // no serve here: http_is's, and none matches
}

TEST_CASE("a reply of a module in a [[clang::nonblocking]] method allocates nothing and makes no system call",
          "[serve][web][server][json][library]")
{
    // The proof for what a literal of a module, an array of modules, a slice, an enumeration and a char array becomes:
    // the description, the slots, the writer and the publish, from a route's block.
    Served served = serveProgram(kPresets, "Store");
    void* memory = served.instance->memory();
    const auto answered = [&] { return field<int>(*served.program, memory, "Store", "answered"); };
    const std::string get = "GET /presets HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(served.port) + "\r\n\r\n";
    std::vector<int> clients;
    const auto send = [&] {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(served.port));
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        REQUIRE(::send(fd, get.data(), get.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(get.size()));
        clients.push_back(fd);
    };

    // In this process, counted on this thread.
    if (fminus::test::HeapCalls::counted())
    {
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
                CHECK(calls == 0);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(done);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");

#if defined(__linux__)
    // In a child under seccomp's strict mode, on a request published before the fork.
    send();
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
            const int result = answered() - before;
            const ssize_t written = ::write(fds[1], &result, sizeof result);
            ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
        }
        ::close(fds[1]);
        int status = 0;
        ::waitpid(child, &status, 0);
        int result = 0;
        const ssize_t got = ::read(fds[0], &result, sizeof result);
        ::close(fds[0]);
        INFO("status " << status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        {
            WARN("seccomp's strict mode is not available here: the system calls are not checked");
            checked = true;
            break;
        }
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
        REQUIRE(got == static_cast<ssize_t>(sizeof result));
        if (result == 1)
            checked = true;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));   // not published yet
    }
    CHECK(checked);
    // The parent's copy of that request, answered here as well.
    const int before = answered();
    for (int attempt = 0; attempt < 500 && answered() == before; ++attempt)
    {
        served.process(memory);
        if (answered() == before)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(answered() > before);
#else
    WARN("seccomp is Linux's: the system calls are not checked here");
#endif
    for (int fd : clients)
        ::close(fd);
}

TEST_CASE("http_reply_json's errors, each once at its column", "[codegen][serve][json]")
{
    CHECK(errorsOf("int f(http_request r, int n, float* p)\n"
                   "{\n"
                   "    return http_reply_json(r, 200, n) + http_reply_json(r, \"200\", json (n))\n"
                   "           + http_reply_json(r, 200, json (p)) + http_reply_json(r, 200);\n"
                   "}\n"
                   "void g(http_request r) [[clang::nonblocking]] { http_reply_json(r, 200, json { \"a\": [1, 2] }); "
                   "}\n")
          == "test.fm:3:36: error: 'http_reply_json' takes a JSON literal here: json { ... }, json [ ... ] or json "
             "(value)\n"
             "test.fm:3:60: error: incompatible pointer to integer conversion passing 'char[4]' to parameter of type "
             "'int'\n"
             "test.fm:4:44: error: a JSON literal does not write a pointer, 'float *': write what it points to, *p, or "
             "its first elements, json_slice(p, count)\n"
             "test.fm:4:50: error: too few arguments to function call, expected 3, have 2\n");
}

#if FMINUS_HAS_WEB && FMINUS_HAS_FILES_WRITE
namespace
{
    // A directory of the test's own beside bin/ (the build tree), removed when it goes.
    class Scratch
    {
    public:
        Scratch()
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
            path_ = tree / "json-serve-test" / ("t" + std::to_string(::getpid()));
            fs::remove_all(path_);
            fs::create_directories(path_);
            path_ = fs::canonical(path_);
        }
        ~Scratch()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
            std::filesystem::remove(path_.parent_path(), ignored);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        const std::filesystem::path& path() const { return path_; }

    private:
        std::filesystem::path path_;
    };

    // The sensors' service, the test's own, on a thread of the test's: each sensor's day of readings (sensor k's i-th
    // is (k - i) / 4), and a summary posted to it, its body kept as `body` and read with json_read into `summary`.
    constexpr const char* kSensors = R"(
enum { SENSORS = 8, SAMPLES = 6 };

module Day { int sensor; float mean; float peak; };
module Summary { Day days[SENSORS]; };

module Sensors
{
    http_server server;
    Summary summary;
    int posted;
    char body[512];
    int bytes;

    void init() { server = http_listen(); }
    int port() { return http_port(server); }

    void process()
    {
        serve (server)
        {
            route "GET /sensors/{k}/day.json" (int k [[fminus::range(0, 7)]])
            {
                float values[SAMPLES];
                for (int i = 0; i < SAMPLES; ++i)
                    values[i] = (float)(k - i) / 4.0f;
                reply 200, json { "sensor": k, "values": values };
            }
            route "POST /summaries" (http_request q)
            {
                if (!string_starts(http_header(q, "content-type"), "application/json"))
                    reply 415;
                bytes = string_copy(&body[0], 512, string_of(http_body(q), http_size(q)));
                json_doc doc = json_parse(http_body(q), http_size(q));
                defer json_free(doc);
                posted += 1;
                if (!json_read(doc, "", &summary))
                    reply 422;
                reply 201;
            }
        }
    }
};
)";
} // namespace

TEST_CASE("the data work: days fetched at once, summarised into an array of modules, written as JSON and posted",
          "[web][server][json][files][within]")
{
    Served sensors = serveProgram(kSensors, "Sensors");
    const Pump pump([&] { sensors.process(sensors.instance->memory()); });
    const Scratch scratch;
    fminus::Program::Options options;
    options.web.allow = {"http://127.0.0.1:" + std::to_string(sensors.port) + "/*"};
    options.files.folders = {{"out", scratch.path().string(), true, {}}};
    // The research's program (§8), its URLs on loopback, its samples six.
    auto data = compileProgram(R"(
enum { SENSORS = 8, SAMPLES = 6 };

module Day { int sensor; float mean; float peak; };

int run(int port)
{
    http_response r[SENSORS];
    within (30000)
    {
        for (int k = 0; k < SENSORS; ++k)
            r[k] = http_get("http://127.0.0.1:{port}/sensors/{k}/day.json");
    }
    Day day[SENSORS];
    for (int k = 0; k < SENSORS; ++k)
    {
        defer http_close(r[k]);
        json_doc doc = json_parse(http_body(r[k]), http_size(r[k]));     // no body: no document, and no values
        defer json_free(doc);
        float value[SAMPLES];
        int n = json_floats(doc, "/values", value, SAMPLES);
        n = n < SAMPLES ? n : SAMPLES;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            sum += value[i];
            day[k].peak = fmaxf(day[k].peak, fabsf(value[i]));
        }
        day[k].sensor = json_int(doc, "/sensor", -1);
        day[k].mean = n > 0 ? (float)(sum / n) : 0.0f;
    }
    const int written = file_write("out/summary.json", json { "days": day });   // an array of modules
    http_response sent = http_post("http://127.0.0.1:{port}/summaries", json { "days": day });
    defer http_close(sent);
    return written * 1000 + http_wait(sent, 10000);
}
)",
                               options);
    std::string errors;
    auto* run = data->function<int(int)>("run", errors);
    INFO(errors);
    REQUIRE(run);
    CHECK(run(sensors.port) == 201);   // FILE_OK, and the service's answer: the summary read whole

    // The file is the summary's JSON: each day as the program computed it, the reference computed here the same way.
    std::string expected = R"({"days":[)";
    struct Day
    {
        std::int32_t sensor;
        float mean;
        float peak;
    };
    std::vector<Day> days;
    for (int k = 0; k < 8; ++k)
    {
        double sum = 0.0;
        float peak = 0.0f;
        for (int i = 0; i < 6; ++i)
        {
            const float value = static_cast<float>(k - i) / 4.0f;
            sum += static_cast<double>(value);
            peak = std::fmax(peak, std::fabs(value));
        }
        days.push_back({k, static_cast<float>(sum / 6), peak});
    }
    const auto shortest = [](float value) {
        char digits[32];
        const auto result = std::to_chars(digits, digits + sizeof digits, value);
        return std::string(digits, result.ptr);
    };
    for (const Day& day : days)
        expected += (day.sensor > 0 ? "," : "") + std::string(R"({"sensor":)") + std::to_string(day.sensor)
                    + R"(,"mean":)" + shortest(day.mean) + R"(,"peak":)" + shortest(day.peak) + "}";
    expected += "]}";
    std::ifstream in(scratch.path() / "summary.json", std::ios::binary);
    const std::string file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(file == expected);

    // The POST's body, as the service kept it, is the same text, and json_read took it into its own modules whole.
    void* memory = sensors.instance->memory();
    CHECK(field<std::int32_t>(*sensors.program, memory, "Sensors", "posted") == 1);
    const auto bytes = field<std::int32_t>(*sensors.program, memory, "Sensors", "bytes");
    std::string body(static_cast<std::size_t>(bytes), '\0');
    for (const fminus::Program::Module::Field& f : sensors.program->module("Sensors")->fields)
    {
        if (f.name == "body")
            std::memcpy(body.data(), static_cast<char*>(memory) + f.offset, body.size());
    }
    CHECK(body == expected);
    for (const fminus::Program::Module::Field& f : sensors.program->module("Sensors")->fields)
    {
        if (f.name != "summary")
            continue;
        std::vector<Day> back(8);
        std::memcpy(back.data(), static_cast<char*>(memory) + f.offset, sizeof(Day) * back.size());
        for (int k = 0; k < 8; ++k)
        {
            INFO("day " << k);
            CHECK(back[static_cast<std::size_t>(k)].sensor == k);
            CHECK(back[static_cast<std::size_t>(k)].mean == days[static_cast<std::size_t>(k)].mean);
            CHECK(back[static_cast<std::size_t>(k)].peak == days[static_cast<std::size_t>(k)].peak);
        }
    }
}

TEST_CASE("a POST of a JSON literal over the policy's maxBodyBytes is HTTP_LIMIT, and nothing is sent; its values are "
          "evaluated once",
          "[web][json]")
{
    fminus::Program::Options options;
    options.web.allow = {"http://127.0.0.1:9/never/*"};   // loopback's discard port, which nothing here reaches
    options.web.maxBodyBytes = 8;
    auto program = compileProgram(R"(
module Poster
{
    int calls;
    int next() { calls += 1; return calls; }
    int post()
    {
        http_response r = http_post("http://127.0.0.1:9/never/x", json { "n": next(), "m": next() });
        defer http_close(r);
        return http_status(r) * 100 + calls;
    }
};
)",
                                  options);
    std::string errors;
    auto* post = program->function<int(void*)>("Poster::post", errors);
    INFO(errors);
    REQUIRE(post);
    Instance poster(*program->module("Poster"));
    CHECK(post(poster.memory()) == -5 * 100 + 2);   // HTTP_LIMIT, the two values once each, measured once
}
#endif
#endif
