// examples/remote/remote.cpp: the remote example's harness, a host of remote.fm through fminus::Program (docs/web.md,
// "The server"). It hands the library the page (page/index.html and page/remote.js) as static content, lets the program
// listen where it is told (Options::web.server), lists the program's routes before running anything, and calls
// Remote::process a block at a time on a thread of its own, as an audio thread would; the program takes its requests
// there.
//
//     build-web/bin/example-remote --check                       (what ctest runs)
//     build-web/bin/example-remote --listen localhost:8080       (serve the page for a minute, for a browser)
//
// --check listens on 127.0.0.1, on a port the system picks, and drives the program with the library's own client, a
// second Program in the same process: the page is served by the library, a gain and a note sent over HTTP are heard in
// the output (the level and the pitch of a second rendered after them), what no route names is answered 404 and 405 by
// the library, a value out of range is the program's 400, and once the host stops the program's network nothing listens
// there. Then it compiles the program again with the default policy, under which http_listen listens nowhere
// (HTTP_REFUSED). Nothing binds anything but 127.0.0.1, and no request leaves the machine. In a build without the web
// server or the JSON parser (FMINUS_WEB_SERVER is off by default), it says so and exits 0.
#include "fminus/Config.h"
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>

#if FMINUS_HAS_WEB_SERVER && FMINUS_HAS_JSON

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace
{
    constexpr float kRate = 48000.0f;
    constexpr int kBlock = 480;              // 10 ms
    constexpr int kTransport = -1;           // HTTP_TRANSPORT
    constexpr int kCancelled = -3;           // HTTP_CANCELLED
    constexpr int kRefused = -4;             // HTTP_REFUSED

    // The client, a second program: each function makes one request of the server at `port`, waits for it, and gives
    // its status, or a number of the body it read.
    constexpr const char* kClient = R"(
int post(int port, int which, int value)
{
    float none[1] = {0.0f};
    void* r = which == 0 ? http_post("http://127.0.0.1:%d/gain?value=0.5", none, 0, "", port)
            : which == 1 ? http_post("http://127.0.0.1:%d/note/%d", none, 0, "", port, value)
            : http_post("http://127.0.0.1:%d/gain?value=nan", none, 0, "", port);
    const int status = http_wait(r, 10000);
    http_close(r);
    return status;
}

int get(int port, int which)
{
    void* r = which == 0 ? http_get("http://127.0.0.1:%d/", port)
            : which == 1 ? http_get("http://127.0.0.1:%d/remote.js", port)
            : which == 2 ? http_get("http://127.0.0.1:%d/nothing", port)
            : http_get("http://127.0.0.1:%d/gain", port);
    const int status = http_wait(r, 10000);
    const int size = http_size(r);
    http_close(r);
    return status < 0 ? status : status * 100000 + size;
}

float state(int port, int what)
{
    void* r = http_get("http://127.0.0.1:%d/state", port);
    float got = -1000.0f;
    if (http_wait(r, 10000) == 200)
    {
        void* doc = json_parse(http_body(r), http_size(r));
        got = what == 0 ? json_float(doc, "/gain", -2.0f)
            : what == 1 ? json_float(doc, "/pitch", -2.0f)
            : what == 2 ? json_float(doc, "/level", -2.0f) : (float)json_int(doc, "/changes", -2);
        json_free(doc);
    }
    http_close(r);
    return got;
}
)";

    std::optional<std::string> readFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    struct Free
    {
        void operator()(void* memory) const
        {
            std::free(memory);
        }
    };

    // An instance of `module`, as a host makes one: aligned and zeroed, its parameters at their defaults.
    std::unique_ptr<void, Free> makeInstance(const fminus::Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        std::unique_ptr<void, Free> instance(
            std::aligned_alloc(alignment, (module.size + alignment - 1) / alignment * alignment));
        if (instance)
        {
            std::memset(instance.get(), 0, module.size);
            fminus::Program::setDefaults(instance.get(), module);
        }
        return instance;
    }

    // remote.fm, compiled under `server` (its listen, token and files), and Remote's methods.
    struct Synth
    {
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* module = nullptr;
        void (*init)(void*, float) = nullptr;
        int (*port)(void*) = nullptr;
        void (*process)(void*, float*, int) = nullptr;
    };

    std::optional<Synth> compileSynth(const std::string& path, fminus::Program::WebServerPolicy server,
                                      std::string& errors)
    {
        const std::optional<std::string> source = readFile(path);
        if (!source)
        {
            errors = "cannot read " + path;
            return std::nullopt;
        }
        fminus::Program::Options options;
        options.fileName = path;
        options.web.server = std::move(server);
        Synth synth;
        synth.program = fminus::Program::compile(*source, errors, options);
        if (!synth.program)
            return std::nullopt;
        synth.module = synth.program->module("Remote");
        synth.init = synth.program->function<void(void*, float)>("Remote::init", errors);
        synth.port = synth.program->function<int(void*)>("Remote::port", errors);
        synth.process = synth.program->function<void(void*, float*, int)>("Remote::process", errors);
        if (synth.module == nullptr || synth.init == nullptr || synth.port == nullptr || synth.process == nullptr)
        {
            errors = path + " has no module Remote with init, port and process: " + errors;
            return std::nullopt;
        }
        return synth;
    }

    // The page, as the host hands it over: each file's path, its type and its bytes.
    std::optional<std::vector<fminus::Program::WebServerPolicy::File>> pageFiles(const std::string& directory,
                                                                                  std::string& errors)
    {
        const std::optional<std::string> html = readFile(directory + "/index.html");
        const std::optional<std::string> script = readFile(directory + "/remote.js");
        if (!html || !script)
        {
            errors = "cannot read the page in " + directory;
            return std::nullopt;
        }
        return std::vector<fminus::Program::WebServerPolicy::File>{
            {"/", "text/html; charset=utf-8", *html},
            {"/remote.js", "text/javascript; charset=utf-8", *script},
        };
    }

    // A second of output, rendered here, a block at a time: its peak over the last half (when the gain has long settled)
    // and its frequency, the sine's upward zero crossings in the second.
    struct Heard
    {
        float peak = 0.0f;
        int crossings = 0;
    };

    Heard render(const Synth& synth, void* instance)
    {
        std::vector<float> out(static_cast<std::size_t>(kRate));
        for (std::size_t at = 0; at < out.size(); at += kBlock)
            synth.process(instance, out.data() + at, kBlock);
        Heard heard;
        for (std::size_t i = 1; i < out.size(); ++i)
        {
            if (out[i - 1] < 0.0f && out[i] >= 0.0f)
                ++heard.crossings;
            if (i >= out.size() / 2)
                heard.peak = std::max(heard.peak, std::fabs(out[i]));
        }
        return heard;
    }

    // The host's audio thread: process() a block at a time, `period` apart, until stopped.
    class AudioThread
    {
    public:
        AudioThread(const Synth& synth, void* instance, std::chrono::microseconds period)
            : thread_([this, &synth, instance, period] {
                  std::vector<float> out(kBlock);
                  while (!stop_.load())
                  {
                      synth.process(instance, out.data(), kBlock);
                      std::this_thread::sleep_for(period);
                  }
              })
        {
        }

        ~AudioThread()
        {
            stop_.store(true);
            thread_.join();
        }

        AudioThread(const AudioThread&) = delete;
        AudioThread& operator=(const AudioThread&) = delete;

    private:
        std::atomic<bool> stop_{false};
        std::thread thread_;
    };

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-remote: %s\n", message.c_str());
        return 1;
    }

    bool near(float value, float expected, float tolerance)
    {
        return std::fabs(value - expected) <= tolerance;
    }

    int runCheck(const std::string& path, const std::string& pageDirectory)
    {
        std::string errors;
        // The default policy: http_listen listens nowhere.
        {
            std::optional<Synth> closed = compileSynth(path, {}, errors);
            if (!closed)
                return fail(errors);
            const auto instance = makeInstance(*closed->module);
            closed->init(instance.get(), kRate);
            if (const int port = closed->port(instance.get()); port != kRefused)
                return fail(std::format("with the default policy, the port is {}, not HTTP_REFUSED (-4)", port));
            if (closed->program->webPort() != 0)
                return fail("with the default policy, webPort() is not 0");
            std::vector<float> out(kBlock);
            closed->process(instance.get(), out.data(), kBlock);   // nothing to take, and the block as ever
            std::printf("the default policy: the program's port is HTTP_REFUSED, and nothing listens\n");
        }

        const std::optional<std::vector<fminus::Program::WebServerPolicy::File>> page = pageFiles(pageDirectory, errors);
        if (!page)
            return fail(errors);
        fminus::Program::WebServerPolicy server;
        server.listen = "127.0.0.1:0";
        server.files = *page;
        std::optional<Synth> synth = compileSynth(path, server, errors);
        if (!synth)
            return fail(errors);
        // What a host shows, or checks, before it runs anything.
        for (const fminus::Program::WebRoute& route : synth->program->webRoutes())
            std::printf("%s:%d: %s %s\n", route.file.c_str(), route.line, route.method.c_str(), route.pattern.c_str());
        if (synth->program->webRoutes().size() != 4)
            return fail("remote.fm should name 4 routes");

        const auto instance = makeInstance(*synth->module);
        if (!instance)
            return fail("out of memory");
        synth->init(instance.get(), kRate);
        const int port = synth->port(instance.get());
        if (port <= 0)
            return fail(std::format("the program does not listen on 127.0.0.1: its port is {}", port));
        if (synth->program->webPort() != port)
            return fail("webPort() is not the program's port");
        std::printf("listening on 127.0.0.1:%d\n", port);

        // Before any request: the defaults, 0.25 and 440 Hz.
        const Heard before = render(*synth, instance.get());
        if (!near(before.peak, 0.25f, 0.001f) || std::abs(before.crossings - 440) > 1)
            return fail(std::format("before any request: a peak of {} at {} Hz, not 0.25 at 440", before.peak,
                                    before.crossings));

        fminus::Program::Options clientOptions;
        clientOptions.web.allow = {"http://127.0.0.1:" + std::to_string(port) + "/*"};
        std::unique_ptr<fminus::Program> client = fminus::Program::compile(kClient, errors, clientOptions);
        if (!client)
            return fail("the client: " + errors);
        auto* post = client->function<int(int, int, int)>("post", errors);
        auto* get = client->function<int(int, int)>("get", errors);
        auto* state = client->function<float(int, int)>("state", errors);
        if (post == nullptr || get == nullptr || state == nullptr)
            return fail("the client: " + errors);

        {
            // The audio thread takes the requests between its blocks, which come a millisecond apart here (a real one
            // is paced by its device: 10 ms a block of 480 at 48 kHz).
            const AudioThread audio(*synth, instance.get(), std::chrono::microseconds(1000));
            // The page, served by the library from what the host handed it, without the program.
            const int html = get(port, 0);
            const int script = get(port, 1);
            if (html != 200 * 100000 + static_cast<int>((*page)[0].bytes.size())
                || script != 200 * 100000 + static_cast<int>((*page)[1].bytes.size()))
                return fail(std::format("the page: {} and {}, not 200 with its files' sizes", html, script));
            // What no route names never reaches the program: the library answers it.
            if (get(port, 2) / 100000 != 404 || get(port, 3) / 100000 != 405)
                return fail("GET /nothing and GET /gain should be the library's 404 and 405");
            // The controls, as the page sends them.
            if (const int status = post(port, 0, 0); status != 204)
                return fail(std::format("POST /gain?value=0.5: {}, not 204", status));
            if (const int status = post(port, 1, 81); status != 204)
                return fail(std::format("POST /note/81: {}, not 204", status));
            if (const int status = post(port, 2, 0); status != 400)   // value=nan: the fallback, out of range
                return fail(std::format("POST /gain?value=nan: {}, not 400", status));
            if (const int status = post(port, 1, 128); status != 400)
                return fail(std::format("POST /note/128: {}, not 400", status));
            const float gain = state(port, 0);
            const float pitch = state(port, 1);
            const float changes = state(port, 3);
            if (gain != 0.5f || !near(pitch, 880.0f, 0.01f) || changes != 2.0f)
                return fail(std::format("GET /state: gain {}, pitch {}, {} changes; not 0.5, 880 and 2", gain, pitch,
                                        changes));
            // The meter: the level of the blocks rendered since, held.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (const float level = state(port, 2); !near(level, 0.5f, 0.01f))
                return fail(std::format("GET /state: a level of {}, not about 0.5", level));
        }
        // After them: the gain and the pitch the requests set, heard.
        const Heard after = render(*synth, instance.get());
        if (!near(after.peak, 0.5f, 0.001f) || std::abs(after.crossings - 880) > 1)
            return fail(std::format("after the requests: a peak of {} at {} Hz, not 0.5 at 880", after.peak,
                                    after.crossings));
        std::printf("heard: a peak of %.4f at %d Hz before, %.4f at %d Hz after POST /gain?value=0.5 and "
                    "POST /note/81\n",
                    static_cast<double>(before.peak), before.crossings, static_cast<double>(after.peak),
                    after.crossings);

        // The host stops the program's network: its server closes, and no one listens there now.
        synth->program->stopWeb();
        if (synth->program->webPort() != 0 || synth->port(instance.get()) != kCancelled)
            return fail("after stopWeb(), the port should be HTTP_CANCELLED and webPort() 0");
        if (const int gone = get(port, 0); gone != kTransport)
            return fail(std::format("after stopWeb(), a request gave {}, not HTTP_TRANSPORT (-1)", gone));
        std::printf("checked: the page served by the library; 404 and 405 without the program; a gain and a note sent "
                    "over HTTP heard in the output; out of range 400; stopped, nothing listens; the default policy "
                    "listens nowhere\n");
        return 0;
    }

    // Serves the page for `seconds` seconds where `listen` says, the program's blocks paced as a device would pace them.
    int serve(const std::string& path, const std::string& pageDirectory, const std::string& listen,
              const std::string& token, int seconds)
    {
        std::string errors;
        const std::optional<std::vector<fminus::Program::WebServerPolicy::File>> page = pageFiles(pageDirectory, errors);
        if (!page)
            return fail(errors);
        fminus::Program::WebServerPolicy server;
        server.listen = listen;
        server.token = token;
        server.files = *page;
        std::optional<Synth> synth = compileSynth(path, server, errors);
        if (!synth)
            return fail(errors);
        const auto instance = makeInstance(*synth->module);
        if (!instance)
            return fail("out of memory");
        synth->init(instance.get(), kRate);
        const int port = synth->port(instance.get());
        if (port <= 0)
            return fail(std::format("{}: the program does not listen (its port is {})", listen, port));
        const std::string where = listen.substr(0, listen.rfind(':'));
        const std::string fragment = token.empty() ? "" : "#k=" + token;
        if (where == "*")
            std::printf("listening on every interface, port %d: http://<this machine's address>:%d/%s\n", port, port,
                        fragment.c_str());
        else
            std::printf("open http://%s:%d/%s\n", where.c_str(), port, fragment.c_str());
        std::fflush(stdout);
        {
            const AudioThread audio(*synth, instance.get(), std::chrono::microseconds(10000));
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
        }
        std::printf("served for %d seconds\n", seconds);
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The remote example: remote.fm, a synthesiser whose gain, pitch and meter a page serves, its requests "
                 "taken on its audio thread",
                 "example-remote"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/remote.fm";
    std::string page = std::string(FMINUS_EXAMPLE_DIR) + "/page";
    std::string listen = "localhost:8080";
    std::string token;
    int seconds = 60;
    bool check = false;
    app.add_option("--module", path, "The FMinus source of the module")->capture_default_str();
    app.add_option("--page", page, "The page's directory (index.html and remote.js)")->capture_default_str();
    app.add_option("--listen", listen,
                   "Where the program may listen (Options::web.server.listen): localhost:8080, 127.0.0.1:0, ...; "
                   "an address that is not loopback needs --token")
        ->capture_default_str();
    app.add_option("--token", token, "A token every request to a route must carry (16 to 256 of A-Z a-z 0-9 - _)");
    app.add_option("--seconds", seconds, "How long to serve")->capture_default_str()->check(CLI::Range(1, 86400));
    app.add_flag("--check", check, "Check only, on 127.0.0.1: what HTTP sends is heard, and the default policy "
                                   "listens nowhere");
    CLI11_PARSE(app, argc, argv);
    if (check)
        return runCheck(path, page);
    return serve(path, page, listen, token, seconds);
}

#else

int main(int argc, char** argv)
{
    CLI::App app{"The remote example (a build with the web server and the JSON parser)", "example-remote"};
    bool check = false;
    app.add_flag("--check", check, "Check only");
    app.allow_extras();
    CLI11_PARSE(app, argc, argv);
    std::printf("example-remote: this build of FMinus leaves out the web server (FMINUS_WEB_SERVER=OFF) or the JSON "
                "parser (FMINUS_JSON=OFF): nothing to serve\n");
    return 0;
}

#endif
