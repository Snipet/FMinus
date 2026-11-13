// examples/presets/presets.cpp: the presets example's harness, a host of presets.fm through fminus::Program
// (docs/web.md, docs/files.md). It maps the program's one folder, presets, to a directory for writing
// (Options::files), lets it listen where it is told (Options::web.server), lists its routes and its files before
// running anything, and runs its run() on a thread of its own, a loader's: the program parses and writes there.
//
//     build-web/bin/example-presets --check                                   (what ctest runs)
//     build-web/bin/example-presets --listen localhost:8080 --folder ./mine   (serve for a minute, for curl)
//
// --check makes a directory of its own beside bin/ (the build tree), maps it, listens on 127.0.0.1 on a port the
// system picks, and drives the program with the library's own client, a second Program in the same process, whose
// URLs have holes: presets stored by name, each the file of its preset written back as JSON, and read back; a body out
// of range, without a field or with a wave no constant names, a name no file may have and a body of another type
// refused; the listing's count and names; then the host closes the
// program's network, and run() returns. It removes its directory. Then it compiles the program again with the default
// policy, under which it listens nowhere. Nothing binds anything but 127.0.0.1, no request leaves the machine, and
// nothing is written outside the directory it made. In a build without the web server, the JSON parser or file
// writing (FMINUS_WEB_SERVER and FMINUS_FILES_WRITE are off by default), it says so and exits 0.
#include "fminus/Config.h"
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>

#if FMINUS_HAS_WEB_SERVER && FMINUS_HAS_JSON && FMINUS_HAS_FILES_WRITE

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    constexpr int kRefused = -4;   // HTTP_REFUSED

    // The client, a second program: each function makes one request of the store at `port`, its name one of NAMES,
    // filled into the URL by a hole, and gives its status, or what it read of the body.
    constexpr const char* kClient = R"(
const string NAMES[6] = {"warm-pad", "bright.2", "missing", ".hidden", "bad name", "con"};
const string BODIES[5] = {"{\"gain\": 0.5, \"cutoff\": 1200, \"wave\": \"SAW\", \"note\": \"not a field\"}",
                          "{\"wave\": 2, \"gain\": 0.75, \"cutoff\": 2e4}",
                          "{\"gain\": 2, \"cutoff\": 1200, \"wave\": \"SAW\"}",
                          "{\"gain\": 0.5, \"cutoff\": 1200}",
                          "{\"gain\": 0.5, \"cutoff\": 1200, \"wave\": \"TRIANGLE\"}"};

int finish(http_response r)
{
    defer http_close(r);
    return http_wait(r, 10000);
}

int put(int port, int which, int body, bool typed)
{
    const string name = NAMES[which];
    const string text = BODIES[body];
    if (typed)
        return finish(http_post("http://127.0.0.1:{port}/presets/{name}", string_data(text), string_size(text),
                                "application/json"));
    return finish(http_post("http://127.0.0.1:{port}/presets/{name}", string_data(text), string_size(text),
                            "text/plain"));
}

// The stored preset's gain, read back; or its status.
float gain(int port, int which)
{
    const string name = NAMES[which];
    http_response r = http_get("http://127.0.0.1:{port}/presets/{name}");
    defer http_close(r);
    const int status = http_wait(r, 10000);
    if (status != 200)
        return (float)status;
    json_doc doc = json_parse(http_body(r), http_size(r));
    defer json_free(doc);
    return json_float(doc, "/gain", -1.0f);
}

// How many presets the store lists, and the first one's file name's length times 1000.
int count(int port)
{
    http_response r = http_get("http://127.0.0.1:{port}/presets");
    defer http_close(r);
    if (http_wait(r, 10000) != 200)
        return -1;
    json_doc doc = json_parse(http_body(r), http_size(r));
    defer json_free(doc);
    return json_int(doc, "/count", -1) + 1000 * string_size(json_string(doc, "/first", "?"));
}

// The i-th preset's name, into `out` (at most `cap` bytes): its length.
int name(int port, int i, char* out, int cap)
{
    http_response r = http_get("http://127.0.0.1:{port}/names/{i}");
    defer http_close(r);
    if (http_wait(r, 10000) != 200)
        return -1;
    return string_copy(out, cap, string_of(http_body(r), http_size(r)));
}
)";

    std::optional<std::string> readFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-presets: %s\n", message.c_str());
        return 1;
    }

    // presets.fm compiled with `folder` mapped as presets (writable) and `listen` (none: the default policy).
    std::unique_ptr<fminus::Program> compileStore(const std::string& path, const std::string& folder,
                                                  const std::string& listen, std::string& errors)
    {
        const std::optional<std::string> source = readFile(path);
        if (!source)
        {
            errors = "cannot read " + path;
            return nullptr;
        }
        fminus::Program::Options options;
        options.fileName = path;
        options.web.server.listen = listen;
        if (!folder.empty())
            options.files.folders = {{"presets", folder, true, {}}};
        return fminus::Program::compile(*source, errors, options);
    }

    // The store's run() on a thread of its own, a loader's, until the host closes the program's network.
    class Loader
    {
    public:
        Loader(int (*run)(float), float seconds)
            : thread_([this, run, seconds] { stored_ = run(seconds); })
        {
        }
        ~Loader()
        {
            if (thread_.joinable())
                thread_.join();
        }
        Loader(const Loader&) = delete;
        Loader& operator=(const Loader&) = delete;

        int join()
        {
            thread_.join();
            return stored_;
        }

    private:
        int stored_ = 0;
        std::thread thread_;
    };

    // A directory of its own beside bin/ (the build tree), removed when it goes.
    class Scratch
    {
    public:
        explicit Scratch(const fs::path& beside)
        {
#if defined(_WIN32)
            const std::string process = std::to_string(::_getpid());
#else
            const std::string process = std::to_string(::getpid());
#endif
            path_ = beside / ("presets-check-" + process);
            fs::remove_all(path_);
            fs::create_directories(path_);
            path_ = fs::canonical(path_);
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        const fs::path& path() const
        {
            return path_;
        }

    private:
        fs::path path_;
    };

    int runCheck(const std::string& path, const fs::path& beside)
    {
        std::string errors;
        const Scratch scratch(beside);
        auto store = compileStore(path, scratch.path().string(), "127.0.0.1:0", errors);
        if (!store)
            return fail(errors);
        // What a host shows, or checks, before it runs anything: its routes, a string parameter's among them, and its
        // files, each hole a "*".
        for (const fminus::Program::WebRoute& route : store->webRoutes())
        {
            std::printf("%s:%d: %s %s", route.file.c_str(), route.line, route.method.c_str(), route.pattern.c_str());
            for (const fminus::Program::RouteParameter& parameter : route.parameters)
                std::printf(" (%s %s, from the %s)", parameter.type.c_str(), parameter.name.c_str(),
                            parameter.source.c_str());
            std::printf("\n");
        }
        for (const fminus::Program::FileUse& use : store->files())
            std::printf("%s:%d: %s %s\n", use.file.c_str(), use.line, use.operation.c_str(), use.pattern.c_str());
        if (store->webRoutes().size() != 5 || store->webRoutes()[0].parameters.size() != 2 ||
            store->webRoutes()[0].parameters[0].type != "string")
            return fail("presets.fm should name 5 routes, the first with a string parameter");
        auto* run = store->function<int(float)>("run", errors);
        if (run == nullptr)
            return fail(errors);

        Loader loader(run, 60.0f);
        int port = 0;
        for (int wait = 0; wait < 500 && port == 0; ++wait)   // until it listens: at most 5 s
        {
            port = store->webPort();
            if (port == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (port <= 0)
        {
            store->stopWeb();
            return fail("the store does not listen on 127.0.0.1");
        }
        std::printf("listening on 127.0.0.1:%d, presets in %s\n", port, scratch.path().string().c_str());

        fminus::Program::Options clientOptions;
        clientOptions.web.allow = {"http://127.0.0.1:" + std::to_string(port) + "/*"};
        auto client = fminus::Program::compile(kClient, errors, clientOptions);
        if (!client)
        {
            store->stopWeb();
            return fail("the client: " + errors);
        }
        auto* put = client->function<int(int, int, int, bool)>("put", errors);
        auto* gain = client->function<float(int, int)>("gain", errors);
        auto* count = client->function<int(int)>("count", errors);
        auto* name = client->function<int(int, int, char*, int)>("name", errors);
        if (put == nullptr || gain == nullptr || count == nullptr || name == nullptr)
        {
            store->stopWeb();
            return fail("the client: " + errors);
        }
        const auto nth = [&](int i)
        {
            char out[64] = {};
            const int n = name(port, i, out, 64);
            return n < 0 ? std::string("<error>") : std::string(out, static_cast<std::size_t>(n < 64 ? n : 64));
        };
        const std::string check = [&]() -> std::string
        {
            // Two presets stored by name, each the file presets/<name>.json, its body as it came, and read back.
            if (const int status = put(port, 0, 0, true); status != 204)
                return std::format("storing warm-pad: {}, not 204", status);
            if (const int status = put(port, 1, 1, true); status != 204)
                return std::format("storing bright.2: {}, not 204", status);
            // What is stored is the preset, written back as JSON (json (preset)): its fields in their order, an
            // enumeration by its constant's name, and nothing the module has no field for.
            if (const auto stored = readFile(scratch.path() / "warm-pad.json");
                stored != "{\"gain\":0.5,\"cutoff\":1200,\"wave\":\"SAW\"}")
                return "warm-pad.json is not the preset: " + stored.value_or("<none>");
            if (const auto stored = readFile(scratch.path() / "bright.2.json");
                stored != "{\"gain\":0.75,\"cutoff\":20000,\"wave\":\"SQUARE\"}")
                return "bright.2.json is not the preset: " + stored.value_or("<none>");
            if (gain(port, 0) != 0.5f || gain(port, 1) != 0.75f)
                return std::format("read back: {} and {}, not 0.5 and 0.75", gain(port, 0), gain(port, 1));
            // Refused: a gain out of its range, a wave missing, a wave no constant names (and warm-pad is as it
            // was), a name no file may have (a leading '.', a device's), a name the server holds no path of (a
            // space), a body that says it is no JSON; none there.
            if (const int status = put(port, 0, 2, true); status != 400)
                return std::format("a gain of 2: {}, not 400", status);
            if (const int status = put(port, 0, 3, true); status != 400)
                return std::format("no wave: {}, not 400", status);
            if (const int status = put(port, 0, 4, true); status != 400)
                return std::format("a wave TRIANGLE: {}, not 400", status);
            if (gain(port, 0) != 0.5f)
                return "a refused body changed warm-pad";
            if (const int status = put(port, 3, 0, true); status != 400)
                return std::format("the name .hidden: {}, not 400", status);
            if (const int status = put(port, 5, 0, true); status != 400)
                return std::format("the name con: {}, not 400", status);
            if (const int status = put(port, 4, 0, true); status != 400)
                return std::format("the name 'bad name': {}, not 400", status);
            if (const int status = put(port, 0, 0, false); status != 415)
                return std::format("a body of text/plain: {}, not 415", status);
            if (const float missing = gain(port, 2); missing != 404.0f)
                return std::format("a preset no one stored: {}, not 404", missing);
            if (fs::exists(scratch.path() / ".hidden.json") || fs::exists(scratch.path() / "con.json"))
                return "a refused name was written";
            // The listing: two, in their names' order, and nothing past the last.
            if (const int listed = count(port); listed != 2 + 1000 * 13)   // "bright.2.json" first
                return std::format("GET /presets: {}, not 2 presets, the first bright.2.json", listed);
            if (nth(0) != "bright.2" || nth(1) != "warm-pad" || !nth(2).empty())
                return std::format("the names: '{}', '{}', '{}'", nth(0), nth(1), nth(2));
            return {};
        }();
        // The host closes the program's network: run() sees its server closed, and returns.
        store->stopWeb();
        const int stored = loader.join();
        if (!check.empty())
            return fail(check);
        if (stored != 2)
            return fail(std::format("run() stored {}, not 2", stored));
        std::printf("stored warm-pad and bright.2 as presets, read back; refused a gain of 2, no wave, a wave "
                    "TRIANGLE, .hidden, con, a space and text/plain; listed 2\n");

        // The default policy: it listens nowhere, and reaches no file.
        auto closed = compileStore(path, "", "", errors);
        if (!closed)
            return fail(errors);
        auto* closedRun = closed->function<int(float)>("run", errors);
        if (closedRun == nullptr)
            return fail(errors);
        if (const int refused = closedRun(1.0f); refused != kRefused)
            return fail(std::format("with the default policy, run() gave {}, not HTTP_REFUSED (-4)", refused));
        std::printf("checked: the default policy listens nowhere\n");
        return 0;
    }

    // Serves for `seconds` seconds where `listen` says, the presets in `folder`.
    int serve(const std::string& path, const std::string& folder, const std::string& listen, int seconds)
    {
        std::string errors;
        std::error_code made;
        fs::create_directories(folder, made);
        auto store = compileStore(path, fs::absolute(folder).string(), listen, errors);
        if (!store)
            return fail(errors);
        auto* run = store->function<int(float)>("run", errors);
        if (run == nullptr)
            return fail(errors);
        std::printf("serving where %s says for %d seconds, the presets in %s\n", listen.c_str(), seconds,
                    folder.c_str());
        std::fflush(stdout);
        const int stored = run(static_cast<float>(seconds));
        if (stored < 0)
            return fail(std::format("{}: the program does not listen ({})", listen, stored));
        std::printf("stored %d presets\n", stored);
        return 0;
    }
}   // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The presets example: presets.fm, a store of named presets kept over HTTP as files",
                 "example-presets"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/presets.fm";
    std::string listen = "localhost:8080";
    std::string folder = "presets";
    int seconds = 60;
    bool check = false;
    app.add_option("--module", path, "The FMinus source of the store")->capture_default_str();
    app.add_option("--listen", listen, "Where the program may listen (Options::web.server.listen)")
        ->capture_default_str();
    app.add_option("--folder", folder, "The directory the program's folder presets is (made if it is not there)")
        ->capture_default_str();
    app.add_option("--seconds", seconds, "How long to serve")->capture_default_str()->check(CLI::Range(1, 86400));
    app.add_flag("--check", check, "Check only, on 127.0.0.1, in a directory it makes beside bin/ and removes");
    CLI11_PARSE(app, argc, argv);
    if (check)
    {
        std::error_code error;
        fs::path beside = fs::absolute(argv[0], error).parent_path().parent_path();   // the build tree
        if (error || beside.empty())
            beside = fs::current_path();
        return runCheck(path, beside);
    }
    return serve(path, folder, listen, seconds);
}

#else

int main(int argc, char** argv)
{
    CLI::App app{"The presets example (a build with the web server, the JSON parser and file writing)",
                 "example-presets"};
    bool check = false;
    app.add_flag("--check", check, "Check only");
    app.allow_extras();
    CLI11_PARSE(app, argc, argv);
    std::printf("example-presets: this build of FMinus leaves out the web server (FMINUS_WEB_SERVER=OFF), the JSON "
                "parser (FMINUS_JSON=OFF) or file writing (FMINUS_FILES_WRITE=OFF): nothing to serve\n");
    return 0;
}

#endif
