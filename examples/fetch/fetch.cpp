// examples/fetch/fetch.cpp: the fetch example's harness, a host of fetch.fm through fminus::Program (docs/web.md). It
// writes the wavetable example's pack as JSON (four band-limited cycles, as examples/wavetable/bench.cpp writes it),
// serves it on 127.0.0.1 itself (or uses a server already there, --port), compiles fetch.fm allowing that server's
// /packs/ and nothing else (Options::web), lists the URLs the program names before running anything, fetches the pack
// through Download, plays it, and compares the output with the same pack handed to Wavetable::init directly: they must
// be the same, bit for bit. With --check (what ctest runs) it also fetches three packs at once in countPacks's within
// block (and a fourth the server lacks), plays Live until its when swaps the pack's first table in, which must then
// play as that table, and compiles the program with the default policy, which reaches nothing, and checks that the
// fetch is refused (HTTP_REFUSED) with no request reaching the server. No request ever leaves the loopback interface. In a build without the web library or the JSON parser (FMINUS_WEB is off by
// default), and on Windows, whose loopback server is not written, it says so and exits 0.
//
//     build-web/bin/example-fetch --check                   (its own server, on an ephemeral port)
//     build-web/bin/example-fetch --write pack.json         (the pack, for a server of one's own: README.md)
//     build-web/bin/example-fetch --port 8000 --pack 1      (fetch http://127.0.0.1:8000/packs/1.json)
#include "fminus/Config.h"
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>

#if FMINUS_HAS_WEB && FMINUS_HAS_JSON && !defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    // The wavetable example's pack (examples/wavetable/bench.cpp: makePack and toJson): sine, triangle, saw and square,
    // the last three as the first harmonics of their series, each sample computed in double and rounded once.
    std::vector<std::vector<float>> packTables(int size)
    {
        constexpr double kPi = 3.14159265358979323846;
        const auto series = [&](int harmonics, auto&& amplitude) {
            std::vector<float> table(static_cast<std::size_t>(size));
            for (int k = 0; k < size; ++k)
            {
                double x = 0;
                for (int h = 1; h <= harmonics; ++h)
                    x += amplitude(h) * std::sin(2 * kPi * h * k / size);
                table[static_cast<std::size_t>(k)] = static_cast<float>(x);
            }
            return table;
        };
        return {
            series(1, [](int) { return 1.0; }),
            series(31, [&](int h) { return h % 2 == 0 ? 0.0 : (h % 4 == 1 ? 8 : -8) / (kPi * kPi * h * h); }),
            series(64, [&](int h) { return (h % 2 == 1 ? 2 : -2) / (kPi * h); }),
            series(63, [&](int h) { return h % 2 == 0 ? 0.0 : 4 / (kPi * h); }),
        };
    }

    // The pack as JSON, its samples in the fewest digits that read back as the same float.
    std::string packJson(int size)
    {
        const std::vector<std::vector<float>> tables = packTables(size);
        std::string text = std::format(R"({{"name": "basic shapes", "size": {}, "gain": 0.5, )"
                                       R"("interpolation": "linear", "tables": [)",
                                       size);
        const char* names[] = {"sine", "triangle", "saw", "square"};
        for (std::size_t t = 0; t < tables.size(); ++t)
        {
            text += std::format(R"({}{{"name": "{}", "samples": [)", t == 0 ? "" : ", ", names[t]);
            for (std::size_t k = 0; k < tables[t].size(); ++k)
                text += std::format("{}{}", k == 0 ? "" : ", ", tables[t][k]);
            text += "]}";
        }
        return text + "]}";
    }

    // The pack's first table as raw little-endian float32s, as a program's own server sends a table (bytes_floats).
    std::string packF32(int size)
    {
        const std::vector<float> table = packTables(size).front();
        std::string bytes(table.size() * sizeof(float), '\0');
        std::memcpy(bytes.data(), table.data(), bytes.size());   // little-endian on every machine FMinus targets
        return bytes;
    }

    // An HTTP/1.1 server on 127.0.0.1, an ephemeral port: GET /packs/1.json, /packs/2.json and /packs/3.json are the
    // pack, /packs/1.f32 its first table, anything else 404. One connection at a time, each closed after its response.
    class Server
    {
    public:
        explicit Server(std::string pack) : pack_(std::move(pack)), table_(packF32(2048))
        {
            listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in where{};
            where.sin_family = AF_INET;
            ::inet_pton(AF_INET, "127.0.0.1", &where.sin_addr);
            socklen_t length = sizeof where;
            if (listener_ < 0 || ::bind(listener_, reinterpret_cast<sockaddr*>(&where), sizeof where) != 0
                || ::listen(listener_, 16) != 0
                || ::getsockname(listener_, reinterpret_cast<sockaddr*>(&where), &length) != 0)
                return;
            port_ = ntohs(where.sin_port);
            thread_ = std::thread([this] { run(); });
        }

        ~Server()
        {
            stop_ = true;
            if (thread_.joinable())
                thread_.join();
            if (listener_ >= 0)
                ::close(listener_);
        }

        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;

        int port() const
        {
            return port_;
        }

        int requests() const
        {
            return requests_.load();
        }

    private:
        void run()
        {
            while (!stop_)
            {
                pollfd one{listener_, POLLIN, 0};
                if (::poll(&one, 1, 20) <= 0)
                    continue;
                const int fd = ::accept(listener_, nullptr, nullptr);
                if (fd < 0)
                    continue;
                ++requests_;
                std::string request;
                char buffer[4096];
                while (request.find("\r\n\r\n") == std::string::npos)
                {
                    const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
                    if (got <= 0)
                        break;
                    request.append(buffer, static_cast<std::size_t>(got));
                }
                const std::size_t start = request.find(' ') + 1;
                const std::string path = request.substr(start, request.find(' ', start) - start);
                const bool json = path == "/packs/1.json" || path == "/packs/2.json" || path == "/packs/3.json";
                const bool table = path == "/packs/1.f32";
                const bool found = request.starts_with("GET ") && (json || table);
                const std::string body = !found ? "no such pack" : json ? pack_ : table_;
                const std::string response =
                    std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                                found ? "200 OK" : "404 Not Found",
                                table ? "application/octet-stream" : "application/json", body.size())
                    + body;
                for (std::size_t sent = 0; sent < response.size();)
                {
                    const ssize_t put = ::send(fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                    if (put <= 0)
                        break;
                    sent += static_cast<std::size_t>(put);
                }
                ::close(fd);
            }
        }

        std::string pack_;
        std::string table_;
        int listener_ = -1;
        int port_ = 0;
        std::atomic<bool> stop_{false};
        std::atomic<int> requests_{0};
        std::thread thread_;
    };

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

    // fetch.fm, compiled with `allow` as its web policy, and its methods.
    struct Fetcher
    {
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* wavetable = nullptr;
        const fminus::Program::Module* download = nullptr;
        bool (*init)(void*, const void*, int, float) = nullptr;
        void (*process)(void*, float*, int) = nullptr;
        void (*end)(void*) = nullptr;
        void (*start)(void*, int, int) = nullptr;
        int (*received)(void*) = nullptr;
        int (*finish)(void*, void*, float) = nullptr;
        int (*countPacks)(int, int) = nullptr;
        const fminus::Program::Module* live = nullptr;
        void (*liveStart)(void*, int, int, float) = nullptr;
        void (*liveProcess)(void*, float*, int) = nullptr;
        int (*liveLoaded)(void*) = nullptr;
        void (*liveEnd)(void*) = nullptr;
    };

    std::optional<Fetcher> compileFetcher(const std::string& path, std::vector<std::string> allow, std::string& errors)
    {
        std::ifstream file(path);
        if (!file)
        {
            errors = "cannot read " + path;
            return std::nullopt;
        }
        const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        fminus::Program::Options options;
        options.fileName = path;
        options.web.allow = std::move(allow);
        options.web.timeoutMs = 10000;
        // #include "../wavetable/wavetable.fm", beside the including file, as fminus finds it.
        options.include = [](std::string_view name, std::string_view from) -> std::optional<fminus::Program::IncludedFile> {
            const std::string where = std::string(from.substr(0, from.find_last_of('/') + 1)) + std::string(name);
            std::ifstream included(where);
            if (!included)
                return std::nullopt;
            return fminus::Program::IncludedFile{
                where, std::string{std::istreambuf_iterator<char>(included), std::istreambuf_iterator<char>()}};
        };
        Fetcher fetcher;
        fetcher.program = fminus::Program::compile(source, errors, options);
        if (!fetcher.program)
            return std::nullopt;
        fminus::Program& program = *fetcher.program;
        fetcher.wavetable = program.module("Wavetable");
        fetcher.download = program.module("Download");
        fetcher.init = program.function<bool(void*, const void*, int, float)>("Wavetable::init", errors);
        fetcher.process = program.function<void(void*, float*, int)>("Wavetable::process", errors);
        fetcher.end = program.function<void(void*)>("Wavetable::end", errors);
        fetcher.start = program.function<void(void*, int, int)>("Download::start", errors);
        fetcher.received = program.function<int(void*)>("Download::received", errors);
        fetcher.finish = program.function<int(void*, void*, float)>("Download::finish", errors);
        fetcher.countPacks = program.function<int(int, int)>("countPacks", errors);
        fetcher.live = program.module("Live");
        fetcher.liveStart = program.function<void(void*, int, int, float)>("Live::start", errors);
        fetcher.liveProcess = program.function<void(void*, float*, int)>("Live::process", errors);
        fetcher.liveLoaded = program.function<int(void*)>("Live::loaded", errors);
        fetcher.liveEnd = program.function<void(void*)>("Live::end", errors);
        if (fetcher.wavetable == nullptr || fetcher.download == nullptr || fetcher.finish == nullptr
            || fetcher.init == nullptr || fetcher.process == nullptr || fetcher.end == nullptr
            || fetcher.start == nullptr || fetcher.received == nullptr || fetcher.countPacks == nullptr
            || fetcher.live == nullptr || fetcher.liveStart == nullptr || fetcher.liveProcess == nullptr
            || fetcher.liveLoaded == nullptr || fetcher.liveEnd == nullptr)
        {
            errors = path + " has no modules Wavetable, Download and Live, and countPacks, with their methods: " + errors;
            return std::nullopt;
        }
        return fetcher;
    }

    // Fetches pack `pack` from 127.0.0.1:`port` into a new oscillator and plays `samples` samples of it into `output`:
    // the status Download::finish gave.
    int fetchAndPlay(const Fetcher& fetcher, int port, int pack, std::vector<float>& output)
    {
        const auto download = makeInstance(*fetcher.download);
        const auto wavetable = makeInstance(*fetcher.wavetable);
        if (!download || !wavetable)
            return -1000;
        fetcher.start(download.get(), port, pack);
        const int status = fetcher.finish(download.get(), wavetable.get(), 48000.0f);
        if (status == 200)
        {
            fetcher.process(wavetable.get(), output.data(), static_cast<int>(output.size()));
            fetcher.end(wavetable.get());
        }
        return status;
    }

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-fetch: %s\n", message.c_str());
        return 1;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The fetch example: fetch.fm fetches the wavetable example's JSON pack from a loopback server its "
                 "host allows, and plays it",
                 "example-fetch"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/fetch.fm";
    int port = 0;
    int pack = 1;
    std::string write;
    bool check = false;
    app.add_option("--module", path, "The FMinus source of the modules")->capture_default_str();
    app.add_option("--port", port, "Fetch from a server already running on 127.0.0.1 at this port (0: start one)")
        ->check(CLI::Range(0, 65535));
    app.add_option("--pack", pack, "The pack's number, in its URL: /packs/<n>.json")->capture_default_str();
    app.add_option("--write", write, "Write the pack's JSON to this file, and do nothing else");
    app.add_flag("--check", check, "Check only: the fetched pack plays as the same pack loaded directly, and the "
                                   "default policy reaches nothing");
    CLI11_PARSE(app, argc, argv);

    const std::string json = packJson(2048);
    if (!write.empty())
    {
        std::ofstream out(write, std::ios::binary);
        out << json;
        if (!out)
            return fail("cannot write " + write);
        std::printf("wrote the pack, %zu bytes, to %s\n", json.size(), write.c_str());
        return 0;
    }

    std::optional<Server> server;
    if (port == 0)
    {
        server.emplace(json);
        port = server->port();
        if (port == 0)
            return fail("cannot listen on 127.0.0.1");
    }
    std::string errors;
    std::optional<Fetcher> fetcher =
        compileFetcher(path, {"http://127.0.0.1:" + std::to_string(port) + "/packs/*"}, errors);
    if (!fetcher)
        return fail(errors);
    // What a host shows, or checks against what it allows, before it runs anything.
    for (const fminus::Program::WebRequest& request : fetcher->program->webRequests())
        std::printf("%s:%d: %s %s\n", request.file.c_str(), request.line, request.method.c_str(),
                    request.pattern.c_str());

    std::vector<float> fetched(48000);
    const int status = fetchAndPlay(*fetcher, port, pack, fetched);
    if (status != 200)
        return fail(std::format("fetching http://127.0.0.1:{}/packs/{}.json: status {}", port, pack, status));
    std::printf("fetched pack %d from 127.0.0.1:%d and played %zu samples\n", pack, port, fetched.size());
    if (!check)
        return 0;

    // The same pack handed to the oscillator directly: the same samples, bit for bit.
    std::vector<float> local(fetched.size());
    {
        const auto wavetable = makeInstance(*fetcher->wavetable);
        if (!wavetable || !fetcher->init(wavetable.get(), json.data(), static_cast<int>(json.size()), 48000.0f))
            return fail("Wavetable::init refused the pack");
        fetcher->process(wavetable.get(), local.data(), static_cast<int>(local.size()));
        fetcher->end(wavetable.get());
    }
    if (std::memcmp(local.data(), fetched.data(), local.size() * sizeof(float)) != 0)
        return fail("the fetched pack plays otherwise than the same pack loaded directly");
    // A pack the server does not have: its status, which the program reads as any other.
    std::vector<float> none(16);
    if (const int missing = fetchAndPlay(*fetcher, port, 9, none); server && missing != 404)
        return fail(std::format("a pack the server lacks: status {}, not 404", missing));
    // Four at once in one within: the server has three of them, and every request has ended where the block ends.
    if (const int packs = fetcher->countPacks(port, 4); server && packs != 3)
        return fail(std::format("four packs fetched at once, three on the server: {} found, not 3", packs));
    // Live: silence until the table arrives, on whichever block it does, then the table, from its first sample, at
    // the default 220 Hz; its when fires once. (A server of one's own may have no /packs/1.f32.)
    if (server)
    {
        const auto live = makeInstance(*fetcher->live);
        if (!live)
            return fail("no memory for Live");
        fetcher->liveStart(live.get(), port, 1, 48000.0f);
        std::vector<float> block(256);
        int silent = 0;
        for (; silent < 5000 && fetcher->liveLoaded(live.get()) == 0; ++silent)
        {
            fetcher->liveProcess(live.get(), block.data(), static_cast<int>(block.size()));
            if (fetcher->liveLoaded(live.get()) == 0)
            {
                if (std::any_of(block.begin(), block.end(), [](float x) { return x != 0.0f; }))
                    return fail("Live sounded before its table arrived");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (fetcher->liveLoaded(live.get()) != 1)
            return fail("Live's table never arrived");
        const std::vector<float> table = packTables(2048).front();
        float phase = 0.0f;
        for (int b = 0; b < 8; ++b)
        {
            if (b > 0)
                fetcher->liveProcess(live.get(), block.data(), static_cast<int>(block.size()));
            for (const float sample : block)
            {
                if (sample != table[static_cast<std::size_t>(static_cast<int>(phase))])
                    return fail("Live plays otherwise than its table from the block it arrived in");
                phase += 220.0f * 2048 / 48000.0f;
                if (phase >= 2048)
                    phase -= 2048;
            }
        }
        if (fetcher->liveLoaded(live.get()) != 1)
            return fail("Live's when fired more than once");
        fetcher->liveEnd(live.get());
    }
    // The default policy reaches nothing: refused, before anything is sent.
    std::optional<Fetcher> closed = compileFetcher(path, {}, errors);
    if (!closed)
        return fail(errors);
    const int before = server ? server->requests() : 0;
    constexpr int kRefused = -4;   // HTTP_REFUSED
    if (const int refused = fetchAndPlay(*closed, port, pack, none); refused != kRefused)
        return fail(std::format("with no URL allowed, the fetch gave {}, not HTTP_REFUSED (-4)", refused));
    if (server && server->requests() != before)
        return fail("with no URL allowed, a request reached the server");
    std::printf("checked: the same samples as the pack loaded directly; a missing pack is 404; three packs of four at "
                "once in a within; Live's table swapped in once; with no URL allowed, HTTP_REFUSED and no request\n");
    return 0;
}

#else

int main(int argc, char** argv)
{
    CLI::App app{"The fetch example (a build with the web library and the JSON parser)", "example-fetch"};
    bool check = false;
    app.add_flag("--check", check, "Check only");
    app.allow_extras();
    CLI11_PARSE(app, argc, argv);
#if defined(_WIN32)
    std::printf("example-fetch: its loopback server is not written for Windows: nothing to fetch\n");
#else
    std::printf("example-fetch: this build of FMinus leaves out the web library (FMINUS_WEB=OFF) or the JSON parser "
                "(FMINUS_JSON=OFF): nothing to fetch\n");
#endif
    return 0;
}

#endif
