// tests/unit/HoleTest.cpp: `{name}` holes in an address, a URL or a file's name (src/ir/Strings.cpp's scanHoles,
// Calls.cpp's lowerHoles, Web.cpp's checkUrlHoles, Files.cpp; docs/web.md and docs/files.md). scanHoles itself; each
// mistake's error at its byte; the host's lists, a "*" for each hole; then through the JIT, files read, listed and
// written by names with holes of each type, and a name a string makes wrong a FILE_INVALID; and URLs with holes, a
// string percent-encoded byte by byte, a path segment it makes "." or ".." refused before anything is sent, sent by
// one program to another that serves on loopback. Everything a test reads or writes is in a directory it makes under
// the build tree (beside bin/), and nothing here binds or connects anywhere but 127.0.0.1.
#include "Compile.h"
#include "fminus/Config.h"
#include "fminus/FMinus.h"
#include "ir/Lowering.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if FMINUS_HAS_FILES
#include "runtime/Files.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#endif

#if FMINUS_HAS_FILES && !defined(_WIN32)
#include <unistd.h>
#elif FMINUS_HAS_FILES
#include <process.h>
#endif

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)
#include "web/Web.h"

#include <chrono>
#include <functional>
#include <thread>
#endif

using namespace fminus::test;

namespace
{
    // scanHoles's library text, each hole's name and place, or its first error as "offset: message".
    std::string holes(std::string_view text)
    {
        const fminus::ir::detail::HoleScan scan = fminus::ir::detail::scanHoles(text);
        if (!scan.errors.empty())
            return std::to_string(scan.errors.front().offset) + ": " + scan.errors.front().message;
        std::string out = scan.library;
        for (const fminus::ir::detail::HoleScan::Hole& hole : scan.holes)
            out += " " + hole.name + "@" + std::to_string(hole.offset) + ">" + std::to_string(hole.at);
        return out;
    }

#if FMINUS_HAS_FILES || FMINUS_HAS_WEB
    std::unique_ptr<fminus::Program> compileProgram(const std::string& source, const fminus::Program::Options& options)
    {
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
    }
#endif
}   // namespace

TEST_CASE("scanHoles: each hole a %d to the library, its braces unescaped, its percent signs as they were",
          "[codegen][holes]")
{
    CHECK(holes("https://example.com/a") == "https://example.com/a");
    CHECK(holes("p/{k}/{name}.json") == "p/%d/%d.json k@2>2 name@6>5");
    CHECK(holes("a{{b}}c{x}") == "a{b}c%d x@7>5");
    CHECK(holes("{{}}") == "{}");
    CHECK(holes("100%%/{k}") == "100%%/%d k@6>6");   // a percent sign reaches the library as itself
    CHECK(holes("a/%d/b") == "a/%d/b");
    CHECK(holes("a/%x/{k}") == "a/%x/%d k@5>5");   // scanFormat's to report
    CHECK(holes("{_x9}") == "%d _x9@0>0");
    // Each mistake at its byte.
    CHECK(holes("a/{k}/%d") == "6: a literal names its values by holes, {name}, or by %d after it, not both: this one "
                               "has a hole before");
    CHECK(holes("a/%d/{k}") == "5: a literal names its values by holes, {name}, or by %d after it, not both: this one "
                               "has a %d before");
    CHECK(holes("a}b") == "1: a '}' ends a hole, {name}, and this one ends none: a brace is written '}}'");
    CHECK(holes("a{b") == "1: a '{' begins a hole, {name}, and this one has no '}': a brace is written '{{'");
    CHECK(holes("a{}") == "2: a hole names a variable, {name}, and this one names none");
    CHECK(holes("a{1x}") == "2: a hole names a variable, {name}, and a name does not begin with a digit");
    CHECK(holes("a{x-y}") == "3: a hole names a variable, {name}: '-' is no part of a name");
    CHECK(holes("a{x y}") == "3: a hole names a variable, {name}: ' ' is no part of a name");
    CHECK(holes("{{{x}}}") == "{%d} x@2>1");
}

#if FMINUS_HAS_FILES
TEST_CASE("a file's name with holes: each mistake an error at its byte", "[codegen][files][holes]")
{
    const std::string head = "enum Wave { SINE, SAW };\n"
                             "int f(string name, int k, bool on, enum Wave w, float x, int* p)\n"
                             "{\n"
                             "    file_content c = ";
    const auto errorOf = [&](const std::string& call)
    {
        return errorsOf(head + call + ";\n    return 0;\n}\n");
    };
    // A good one of each type compiles.
    std::string errors;
    CHECK(compile(head + "file_read(\"presets/{name}/{k}-{on}-{w}.json\");\n    return 0;\n}\n", &errors) != nullptr);
    CHECK(errors.empty());
    CHECK(compile(head + "file_read(\"presets/x{name}y.json\");\n    return 0;\n}\n", &errors) != nullptr);
    CHECK(errors.empty());
    CHECK(errorOf("file_read(\"{name}/a.json\")") ==
          "test.fm:4:33: error: a file's folder is its literal's own, a name its host maps: no hole in it\n");
    CHECK(errorOf("file_read(\"p{k}/a.json\")") ==
          "test.fm:4:34: error: a file's folder is its literal's own, a name its host maps: no hole in it\n");
    CHECK(errorOf("file_read(\"p/a.{name}\")") ==
          "test.fm:4:37: error: a file's extension is its type, which the literal writes: no hole in it\n");
    CHECK(errorOf("file_read(\"p/a.js{k}\")") ==
          "test.fm:4:39: error: a file's extension is its type, which the literal writes: no hole in it\n");
    CHECK(errorOf("file_read(\"p/{x}.json\")") ==
          "test.fm:4:35: error: hole {x} has type 'float': a hole takes an int, a bool, an enumeration's value or "
          "a string\n");
    CHECK(errorOf("file_read(\"p/{p}.json\")") ==
          "test.fm:4:35: error: hole {p} has type 'int *': a hole takes an int, a bool, an enumeration's value or "
          "a string\n");
    CHECK(errorOf("file_read(\"p/{nothing}.json\")") ==
          "test.fm:4:35: error: use of undeclared identifier 'nothing'\n");
    CHECK(errorOf("file_read(\"p/{k}.json\", k)") ==
          "test.fm:4:46: error: a literal with holes takes no values after it: each hole, {name}, names its own\n");
    CHECK(errorOf("file_read(\"p/{k}%d.json\", k)") ==
          "test.fm:4:38: error: a literal names its values by holes, {name}, or by %d after it, not both: this one "
          "has a hole before\n");
    CHECK(errorOf("file_read(\"p/a}.json\")") ==
          "test.fm:4:36: error: a '}' ends a hole, {name}, and this one ends none: a brace is written '}}'\n");
    CHECK(errorOf("file_list(\"p/{name}/x.json\")") ==
          "test.fm:4:42: error: a listing's pattern has one '*' in its file part, as \"kit/*.f32\"\n");
    // A device name of Windows a number's hole could write, as a %d's.
    CHECK(errorOf("file_read(\"p/COM{k}.json\")") ==
          "test.fm:4:35: error: this segment is a device name of Windows when its hole is a digit: no segment of a "
          "file's name may be one, in any letter case, with or without an extension\n");
    // A literal brace, written twice, is one.
    CHECK(compile(head + "file_read(\"p/{{a}}{k}.json\");\n    return 0;\n}\n", &errors) != nullptr);
    CHECK(errors.empty());
    // A hole reads its variable where its call is: in a parallel block, beside a statement that changes it, a race.
    CHECK(errorsOf("int f(int k)\n"
                   "{\n"
                   "    int n = 0;\n"
                   "    parallel\n"
                   "    {\n"
                   "        n = k;\n"
                   "        file_close(file_read(\"presets/{n}.json\"));\n"
                   "    }\n"
                   "    return n;\n"
                   "}\n")
          == "test.fm:7:30: error: the statements of a parallel block run at once: this one uses 'n', which another "
             "changes\n"
             "test.fm:6:9: note: 'n' is changed here\n");
}

TEST_CASE("files() lists a name with holes as a pattern, a '*' for each hole", "[library][files][holes]")
{
    std::string source = "file_content load(string name, int k) { return file_read(\"presets/{name}/v{k}.json\"); }\n"
                         "file_listing list(string bank) { return file_list(\"kit/{bank}-{{x}}/*.f32\"); }\n";
    if (FMINUS_HAS_FILES_WRITE != 0)
        source += "int save(string name, const void* d, int n) { return file_write(\"user/{name}.f32\", d, n); }\n";
    auto program = compileProgram(source, {});
    const std::vector<fminus::Program::FileUse>& uses = program->files();
    REQUIRE(uses.size() == (FMINUS_HAS_FILES_WRITE != 0 ? 3u : 2u));
    CHECK(uses[0].operation == "read");
    CHECK(uses[0].pattern == "presets/*/v*.json");
    CHECK(uses[0].line == 1);
    CHECK(uses[0].column == 48);
    CHECK(uses[1].operation == "list");
    CHECK(uses[1].pattern == "kit/*-{x}/*.f32");
    if (FMINUS_HAS_FILES_WRITE != 0)
    {
        CHECK(uses[2].operation == "write");
        CHECK(uses[2].pattern == "user/*.f32");
    }
}

namespace
{
    namespace fs = std::filesystem;

    // A directory of the test's own beside bin/ (the build tree), removed when it goes.
    class Scratch
    {
    public:
        Scratch()
        {
            static std::atomic<int> made{0};
            fs::path tree;
#if defined(__linux__)
            std::error_code error;
            const fs::path self = fs::read_symlink("/proc/self/exe", error);
            if (!error)
                tree = self.parent_path().parent_path();
#endif
            if (tree.empty())
                tree = fs::current_path();
#if defined(_WIN32)
            const std::string process = std::to_string(::_getpid());
#else
            const std::string process = std::to_string(::getpid());
#endif
            path_ = tree / "hole-test" / ("t" + process + "-" + std::to_string(made++));
            fs::remove_all(path_);
            for (int attempt = 0;; ++attempt)
            {
                std::error_code failed;
                fs::create_directories(path_, failed);
                if (!failed)
                    break;
                if (failed != std::errc::no_such_file_or_directory || attempt == 100)
                    fs::create_directories(path_);
            }
            path_ = fs::canonical(path_);
        }
        ~Scratch()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
            fs::remove(path_.parent_path(), ignored);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        fs::path operator/(const std::string& name) const
        {
            return path_ / name;
        }

    private:
        fs::path path_;
    };

    void put(const fs::path& file, const std::string& bytes)
    {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    std::string contents(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
}   // namespace

TEST_CASE("a program reads, lists and writes files by names with holes; a name a string makes wrong is FILE_INVALID",
          "[library][files][holes]")
{
    const Scratch scratch;
    put(scratch / "presets/warm/3-1-1.txt", "warm three");
    put(scratch / "presets/warm/3-0-0.txt", "warm zero");
    put(scratch / "presets/cold/x.txt", "cold x");
    put(scratch / "presets/cold/y.txt", "cold y");
    put(scratch / "presets/cold/z.bin", "not listed");
    put(scratch / "presets/{x}.txt", "braces");
    fminus::Program::Options options;
    options.files.folders = {{"presets", (scratch / "presets").string(), false, {}}};
    if (FMINUS_HAS_FILES_WRITE != 0)
    {
        fs::create_directories(scratch / "takes");
        options.files.folders.push_back({"user", (scratch / "takes").string(), true, {}});
    }
    // The name a string comes from: bytes the host writes into the module's char array.
    std::string source = "enum Wave { SINE, SAW };\n"
                         "module Loader\n"
                         "{\n"
                         "    char text[64];\n"
                         "    int size;\n"
                         "    char got[64];\n"
                         "    int length;\n"
                         "    // The file presets/<text>/<k>-<on>-<wave>.txt: its status, its bytes into got.\n"
                         "    int load(int k, bool on)\n"
                         "    {\n"
                         "        const string name = string_of(text, size);\n"
                         "        enum Wave wave = on ? SAW : SINE;\n"
                         "        file_content c = file_read(\"presets/{name}/{k}-{on}-{wave}.txt\");\n"
                         "        defer file_close(c);\n"
                         "        length = string_copy(got, 64, string_of(file_data(c), file_size(c)));\n"
                         "        return file_status(c);\n"
                         "    }\n"
                         "    // The files presets/<text>/*.txt lists: its status, or how many.\n"
                         "    int list()\n"
                         "    {\n"
                         "        const string name = string_of(text, size);\n"
                         "        file_listing l = file_list(\"presets/{name}/*.txt\");\n"
                         "        defer file_close(l);\n"
                         "        return file_status(l) != FILE_OK ? file_status(l) : file_count(l);\n"
                         "    }\n"
                         "    int braces()\n"
                         "    {\n"
                         "        file_content c = file_read(\"presets/{{x}}.txt\");\n"
                         "        defer file_close(c);\n"
                         "        return file_size(c);\n"
                         "    }\n";
    if (FMINUS_HAS_FILES_WRITE != 0)
        source +=
            "    int save(int k)\n"
            "    {\n"
            "        const string name = string_of(text, size);\n"
            "        return file_write(\"user/{name}-{k}.txt\", text, size) + 10 * file_append(\"user/log{k}.txt\","
            " text, size);\n"
            "    }\n";
    source += "};\n";
    auto program = compileProgram(source, options);
    std::string errors;
    auto* load = program->function<int(void*, int, bool)>("Loader::load", errors);
    auto* list = program->function<int(void*)>("Loader::list", errors);
    auto* braces = program->function<int(void*)>("Loader::braces", errors);
    INFO(errors);
    REQUIRE(load);
    REQUIRE(list);
    REQUIRE(braces);
    const fminus::Program::Module* layout = program->module("Loader");
    REQUIRE(layout != nullptr);
    std::vector<std::max_align_t> storage(layout->size / sizeof(std::max_align_t) + 1);
    void* self = storage.data();
    const auto name = [&](const std::string& text)
    {
        REQUIRE(text.size() <= 64);
        std::memcpy(static_cast<char*>(self), text.data(), text.size());   // text is the first field
        const int size = static_cast<int>(text.size());
        std::memcpy(static_cast<char*>(self) + 64, &size, sizeof size);   // and size the second
    };
    const auto got = [&]
    {
        int n = 0;
        std::memcpy(&n, static_cast<char*>(self) + 132, sizeof n);
        return std::string(static_cast<char*>(self) + 68, static_cast<std::size_t>(n));
    };
    REQUIRE(layout->fields.size() == 4);
    REQUIRE(layout->fields[2].offset == 68);
    REQUIRE(layout->fields[3].offset == 132);

    // A string joins its segment, an int, a bool and an enumeration's value are written in decimal.
    name("warm");
    CHECK(load(self, 3, true) == FMINUS_FILE_OK);
    CHECK(got() == "warm three");
    CHECK(load(self, 3, false) == FMINUS_FILE_OK);
    CHECK(got() == "warm zero");
    CHECK(load(self, 4, false) == FMINUS_FILE_MISSING);
    CHECK(list(self) == 2);
    name("cold");
    CHECK(list(self) == 2);
    name("nowhere");
    CHECK(list(self) == FMINUS_FILE_MISSING);
    CHECK(braces(self) == 6);
    // What a string cannot make: another segment, a hidden name, "." or "..", an empty segment, a device name: a status
    // the program reads, never a trap, and nothing opened.
    for (const char* wrong : {"warm/..", "..", ".", "", ".hidden", "a/b", "a\\b", "CON", "nul", "a:b", "a*b", "x\x01",
                              "trailing.", "trailing "})
    {
        INFO(wrong);
        name(wrong);
        CHECK(load(self, 3, true) == FMINUS_FILE_INVALID);
        CHECK(list(self) == FMINUS_FILE_INVALID);
    }
    if (FMINUS_HAS_FILES_WRITE != 0)
    {
        auto* save = program->function<int(void*, int)>("Loader::save", errors);
        REQUIRE(save);
        name("take one");
        CHECK(save(self, 2) == 0);
        CHECK(contents(scratch / "takes/take one-2.txt") == "take one");
        CHECK(contents(scratch / "takes/log2.txt") == "take one");
        name("../escape");
        CHECK(save(self, 3) == FMINUS_FILE_INVALID + 10 * FMINUS_FILE_OK);   // the append's name has no string
        CHECK(!fs::exists(scratch / "escape-3.txt"));
    }
}
#endif

#if FMINUS_HAS_WEB
TEST_CASE("a URL with holes: each mistake an error at its byte, and webRequests() a '*' for each hole",
          "[codegen][web][holes]")
{
    const std::string head = "enum Wave { SINE, SAW };\n"
                             "void f(string name, int k, bool on, enum Wave w, float x)\n"
                             "{\n"
                             "    http_response r = ";
    const auto errorOf = [&](const std::string& call)
    {
        return errorsOf(head + call + ";\n}\n");
    };
    std::string errors;
    CHECK(compile(head + "http_get(\"https://example.com:{k}/a/{name}/{k}?on={on}&w={w}#{name}\");\n}\n", &errors) !=
          nullptr);
    CHECK(errors.empty());
    CHECK(errorOf("http_get(\"https://{name}.example.com/\")") ==
          "test.fm:4:41: error: a URL's scheme and host are its literal's own: a hole may be in its port, its path, "
          "its query or its fragment\n");
    CHECK(errorOf("http_get(\"https://example{k}.com/\")") ==
          "test.fm:4:48: error: a URL's scheme and host are its literal's own: a hole may be in its port, its path, "
          "its query or its fragment\n");
    CHECK(errorOf("http_get(\"https://[::1]:{name}/\")") ==
          "test.fm:4:47: error: a URL's port is a number: its hole is an int, not a string\n");
    CHECK(errorOf("http_get(\"{name}://example.com/\")") ==
          "test.fm:4:33: error: a URL's scheme is http or https, as 'https://example.com/a.json'\n");
    CHECK(errorOf("http_get(\"https://example.com/{x}\")") ==
          "test.fm:4:53: error: hole {x} has type 'float': a hole takes an int, a bool, an enumeration's value or "
          "a string\n");
    CHECK(errorOf("http_get(\"https://example.com/{k\")") ==
          "test.fm:4:53: error: a '{' begins a hole, {name}, and this one has no '}': a brace is written '{{'\n");
    CHECK(errorOf("http_get(\"https://example.com/%d/{k}\", k)") ==
          "test.fm:4:56: error: a literal names its values by holes, {name}, or by %d after it, not both: this one "
          "has a %d before\n");
    CHECK(errorOf("http_post(\"https://example.com/{k}\", &x, 4, \"\", k)") ==
          "test.fm:4:71: error: a literal with holes takes no values after it: each hole, {name}, names its own\n");
    CHECK(errorOf("http_get(\"https://example.com/{k}/%y\")") ==
          "test.fm:4:58: error: invalid conversion specifier 'y'\n"
          "test.fm:4:58: note: a format here takes %d, an int, and %%, a percent sign; and holes, {name}\n");

    const std::string source = "http_response a(string name, int k) { return http_get(\"https://example.com/p/{name}/"
                               "{k}.json?v={k}&w=100%%\"); }\n"
                               "http_response b(string name, const void* d) { return http_post(\"http://127.0.0.1:"
                               "{port}/x/{name}\", d, 4, \"\"); }\n";
    CHECK(errorsOf(source) == "test.fm:2:82: error: use of undeclared identifier 'port'\n");
    auto program = compileProgram("http_response a(string name, int k) { return http_get(\"https://example.com/p/{name}"
                                  "/{k}.json?v={k}&w=100%%\"); }\n"
                                  "http_response b(string name, int port, const void* d)\n"
                                  "{\n"
                                  "    return http_post(\"http://127.0.0.1:{port}/x/{name}?{{\", d, 4, \"\");\n"
                                  "}\n",
                                  {});
    const std::vector<fminus::Program::WebRequest>& requests = program->webRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].pattern == "https://example.com/p/*/*.json?v=*&w=100%");
    CHECK(requests[0].line == 1);
    CHECK(requests[0].column == 46);
    CHECK(requests[1].method == "POST");
    CHECK(requests[1].pattern == "http://127.0.0.1:*/x/*?{");
    CHECK(requests[1].line == 4);
}
#endif

#if FMINUS_HAS_WEB_SERVER && !defined(_WIN32)
namespace
{
    // The server: what it was asked, as the library holds it: a query's field's bytes as sent, and a path's segment.
    constexpr const char* kEcho = "module Echo\n"
                                  "{\n"
                                  "    http_server server;\n"
                                  "    void init() { server = http_listen(); }\n"
                                  "    int port() { return http_port(server); }\n"
                                  "    void process() [[clang::nonblocking]]\n"
                                  "    {\n"
                                  "        serve (server)\n"
                                  "        {\n"
                                  "            route \"GET /query/{k}\" (int k, string q)\n"
                                  "            {\n"
                                  "                reply k, string_data(q), string_size(q), \"text/plain\";\n"
                                  "            }\n"
                                  "            route \"POST /path/{name}/{k}\" (string name, int k)\n"
                                  "            {\n"
                                  "                reply k, string_data(name), string_size(name), \"text/plain\";\n"
                                  "            }\n"
                                  "        }\n"
                                  "    }\n"
                                  "};\n";

    // The client: a URL with holes, a string from the host's bytes (text, size), the body it got into `got`.
    constexpr const char* kAsk =
        "module Ask\n"
        "{\n"
        "    char text[256];\n"
        "    int size;\n"
        "    char got[1024];\n"
        "    int length;\n"
        "    int finish(http_response r)\n"
        "    {\n"
        "        defer http_close(r);\n"
        "        const int status = http_wait(r, 10000);\n"
        "        length = string_copy(got, 1024, string_of(http_body(r), http_size(r)));\n"
        "        return status;\n"
        "    }\n"
        "    int query(int port, int k)\n"
        "    {\n"
        "        const string name = string_of(text, size);\n"
        "        return finish(http_get(\"http://127.0.0.1:{port}/query/{k}?q={name}&z=1\"));\n"
        "    }\n"
        "    int path(int port, int k)\n"
        "    {\n"
        "        const string name = string_of(text, size);\n"
        "        float none[1];\n"
        "        return finish(http_post(\"http://127.0.0.1:{port}/path/{name}/{k}\", none, 0, "
        "\"\"));\n"
        "    }\n"
        "};\n";

    struct Echo
    {
        std::unique_ptr<fminus::Program> program;
        std::vector<std::max_align_t> storage;
        void (*process)(void*) = nullptr;
        int port = 0;
    };
}   // namespace

TEST_CASE("a URL's string holes are percent-encoded byte by byte, and one that makes a path segment '.' or '..' is "
          "refused before anything is sent",
          "[library][web][server][holes]")
{
    fminus::Program::Options serverOptions;
    serverOptions.web.server.listen = "127.0.0.1:0";
    Echo echo;
    echo.program = compileProgram(kEcho, serverOptions);
    std::string errors;
    auto* init = echo.program->function<void(void*)>("Echo::init", errors);
    auto* port = echo.program->function<int(void*)>("Echo::port", errors);
    echo.process = echo.program->function<void(void*)>("Echo::process", errors);
    INFO(errors);
    REQUIRE(init);
    REQUIRE(port);
    REQUIRE(echo.process);
    echo.storage.resize(echo.program->module("Echo")->size / sizeof(std::max_align_t) + 1);
    init(echo.storage.data());
    echo.port = port(echo.storage.data());
    REQUIRE(echo.port > 0);
    std::atomic<bool> stop{false};
    std::thread audio(
        [&]
        {
            while (!stop.load())
            {
                echo.process(echo.storage.data());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    fminus::Program::Options clientOptions;
    clientOptions.web.allow = {"http://127.0.0.1:" + std::to_string(echo.port) + "/*"};
    auto client = compileProgram(kAsk, clientOptions);
    auto* query = client->function<int(void*, int, int)>("Ask::query", errors);
    auto* path = client->function<int(void*, int, int)>("Ask::path", errors);
    REQUIRE(query);
    REQUIRE(path);
    const fminus::Program::Module* layout = client->module("Ask");
    REQUIRE(layout != nullptr);
    REQUIRE(layout->fields[2].offset == 260);
    REQUIRE(layout->fields[3].offset == 1284);
    std::vector<std::max_align_t> storage(layout->size / sizeof(std::max_align_t) + 1);
    void* self = storage.data();
    const auto name = [&](const std::string& text)
    {
        std::memcpy(static_cast<char*>(self), text.data(), text.size());
        const int size = static_cast<int>(text.size());
        std::memcpy(static_cast<char*>(self) + 256, &size, sizeof size);
    };
    const auto got = [&]
    {
        int n = 0;
        std::memcpy(&n, static_cast<char*>(self) + 1284, sizeof n);
        return std::string(static_cast<char*>(self) + 260, static_cast<std::size_t>(n));
    };

    // Every byte: the unreserved ones as they are, every other one %XX in upper case.
    std::string all;
    std::string encoded;
    for (int byte = 0; byte < 256; ++byte)
    {
        const char c = static_cast<char>(byte);
        all += c;
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';
        constexpr const char* kHex = "0123456789ABCDEF";
        encoded += unreserved ? std::string(1, c) : std::string{'%', kHex[byte >> 4], kHex[byte & 15]};
    }
    name(all);
    CHECK(query(self, echo.port, 200) == 200);
    CHECK(got() == encoded);
    name("a b&c=d?e#f/g");
    CHECK(query(self, echo.port, 201) == 201);
    CHECK(got() == "a%20b%26c%3Dd%3Fe%23f%2Fg");
    name("");
    CHECK(query(self, echo.port, 202) == 202);
    CHECK(got() == "");
    // In the path: a segment of the server's bytes reaches its route's string parameter as it is.
    name("warm-1.0_x~");
    CHECK(path(self, echo.port, 203) == 203);
    CHECK(got() == "warm-1.0_x~");
    // A segment the string makes ".", ".." or empty: HTTP_INVALID, before anything is sent; a "/" the library's path
    // check refuses, encoded (HTTP_REFUSED); a byte the server holds no path to is a request it refuses (400).
    for (const char* wrong : {"..", ".", ""})
    {
        INFO(wrong);
        name(wrong);
        CHECK(path(self, echo.port, 204) == FMINUS_WEB_INVALID);
    }
    name("a/b");
    CHECK(path(self, echo.port, 204) == FMINUS_WEB_REFUSED);
    name("a b");
    CHECK(path(self, echo.port, 204) == 400);

    stop.store(true);
    audio.join();
}
#endif
