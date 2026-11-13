// tests/unit/DocumentTextTest.cpp: text read from a document and a listing (src/ir/Calls.cpp's textResult): json_string
// and xml_string, each the parser's bytes or the fallback, and file_name, a listing's own name of a file. Through the
// JIT, each at its edges (an empty string, one with a 0 byte, a value of another kind, nothing there, a null document,
// an index outside the listing); their errors; and a [[clang::nonblocking]] function that calls every wait-free
// builtin of this step, the reads proved to allocate nothing (HeapCalls) and to make no system call (seccomp's strict
// mode). Everything a test reads is in a directory it makes under the build tree (beside bin/).
#include "Compile.h"
#include "fminus/Config.h"
#include "fminus/FMinus.h"
#include "HeapCalls.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if FMINUS_HAS_FILES
#include "runtime/Files.h"
#include "runtime/Runtime.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#endif

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#else
#include <process.h>
#endif

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

using namespace fminus::test;

namespace
{
#if FMINUS_HAS_JSON || FMINUS_HAS_XML
    // What a function of the program, (data, size, k, out, cap) -> its string's length, wrote into `out`.
    using Read = int(const void*, int, int, char*, int);

    std::string call(Read* read, const std::string& document, int k)
    {
        char out[64] = {};
        const int n = read(document.data(), static_cast<int>(document.size()), k, out, 64);
        return n < 0 ? "<" + std::to_string(n) + ">" : std::string(out, static_cast<std::size_t>(n < 64 ? n : 64));
    }
#endif

#if defined(__linux__)
    // `run`'s result in a child under seccomp's strict mode, where any system call but read, write and exit kills it;
    // nullopt (and a warning) where the mode is not there.
    template <typename Run>
    std::optional<int> sealed(Run run)
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
            const int result = run();
            const ssize_t written = ::write(fds[1], &result, sizeof result);
            ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
        }
        ::close(fds[1]);
        int status = 0;
        ::waitpid(child, &status, 0);
        int result = 0;
        const ssize_t got = ::read(fds[0], &result, sizeof result);
        ::close(fds[0]);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        {
            WARN("seccomp's strict mode is not available here: the system calls are not checked");
            return std::nullopt;
        }
        INFO("status " << status);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(got == static_cast<ssize_t>(sizeof result));
        return result;
    }
#endif
}   // namespace

#if FMINUS_HAS_JSON
TEST_CASE("json_string: the string at the path, unescaped, or the fallback when there is none or it is no string",
          "[codegen][json][string-type]")
{
    auto jit = compile("int name(const void* data, int size, int k, char* out, int cap)\n"
                       "{\n"
                       "    json_doc doc = json_parse(data, size);\n"
                       "    defer json_free(doc);\n"
                       "    return string_copy(out, cap, json_string(doc, \"/presets/%d/name\", k, \"(none)\"));\n"
                       "}\n"
                       "int whole(const void* data, int size, int k, char* out, int cap)\n"
                       "{\n"
                       "    json_doc doc = json_parse(data, size);\n"
                       "    defer json_free(doc);\n"
                       "    return string_copy(out, cap, json_string(doc, \"\", \"(not a string)\"));\n"
                       "}\n"
                       "int none(const void* data, int size, int k, char* out, int cap)\n"
                       "{\n"
                       "    return string_copy(out, cap, json_string(nullptr_doc(), \"/a\", \"null\"));\n"
                       "}\n"
                       "json_doc nullptr_doc() { json_doc d; return d; }\n");
    REQUIRE(jit);
    auto* name = lookup<Read>(*jit, "name");
    auto* whole = lookup<Read>(*jit, "whole");
    auto* none = lookup<Read>(*jit, "none");
    const std::string presets = "{\"presets\":[{\"name\":\"warm \\\"pad\\\" \\u00e9\"},{\"name\":5},{\"name\":\"\"},"
                                "{\"name\":\"a\\u0000b\"},{\"other\":\"x\"}]}";
    CHECK(call(name, presets, 0) == "warm \"pad\" \xc3\xa9");
    CHECK(call(name, presets, 1) == "(none)");   // a number is no string
    CHECK(call(name, presets, 2) == "");         // an empty string is one
    CHECK(call(name, presets, 3) == std::string("a\0b", 3));
    CHECK(call(name, presets, 4) == "(none)");   // no member of the name
    CHECK(call(name, presets, 5) == "(none)");   // no element
    CHECK(call(name, presets, -1) == "(none)");
    CHECK(call(name, "not json", 0) == "(none)");   // a null document is an empty one
    CHECK(call(whole, "\"abc\"", 0) == "abc");
    CHECK(call(whole, "[1]", 0) == "(not a string)");
    CHECK(call(none, "", 0) == "null");
}
#endif

#if FMINUS_HAS_XML
TEST_CASE("xml_string: an attribute's value or an element's text, trimmed, or the fallback when nothing is there",
          "[codegen][xml][string-type]")
{
    auto jit =
        compile("int text(const void* data, int size, int k, char* out, int cap)\n"
                "{\n"
                "    xml_doc doc = xml_parse(data, size);\n"
                "    defer xml_free(doc);\n"
                "    switch (k)\n"
                "    {\n"
                "    case 0: return string_copy(out, cap, xml_string(doc, \"/preset/@name\", \"-\"));\n"
                "    case 1: return string_copy(out, cap, xml_string(doc, \"/preset/osc[%d]\", 0, \"-\"));\n"
                "    case 2: return string_copy(out, cap, xml_string(doc, \"/preset/osc[%d]\", 1, \"-\"));\n"
                "    case 3: return string_copy(out, cap, xml_string(doc, \"/preset/osc[%d]/@wave\", 0, \"-\"));\n"
                "    default: return string_copy(out, cap, xml_string(doc, \"/preset/nothing\", \"-\"));\n"
                "    }\n"
                "}\n");
    REQUIRE(jit);
    auto* text = lookup<Read>(*jit, "text");
    const std::string preset =
        "<preset name=\"warm &amp; pad\"><osc wave=\"saw\">\n  bright osc \t</osc><osc/></preset>";
    CHECK(call(text, preset, 0) == "warm & pad");
    CHECK(call(text, preset, 1) == "bright osc");   // XML's white space around it left out
    CHECK(call(text, preset, 2) == "");             // an element with no text
    CHECK(call(text, preset, 3) == "saw");
    CHECK(call(text, preset, 4) == "-");
    CHECK(call(text, "<a", 0) == "-");   // no document
}
#endif

#if FMINUS_HAS_JSON && FMINUS_HAS_XML && FMINUS_HAS_WEB_SERVER && FMINUS_HAS_FILES
TEST_CASE("a [[clang::nonblocking]] function calls every wait-free builtin of a request's, a document's and a "
          "listing's text",
          "[codegen][effects][string-type]")
{
    std::string errors;
    CHECK(compile("int all(http_request q, json_doc j, xml_doc x, file_listing l) [[clang::nonblocking]]\n"
                  "{\n"
                  "    string a = http_header(q, \"x-a\");\n"
                  "    string b = http_path(q);\n"
                  "    string c = http_query_string(q, \"c\", \"\");\n"
                  "    string d = json_string(j, \"/d/%d\", 1, a);\n"
                  "    string e = xml_string(x, \"/e/@f\", b);\n"
                  "    string f = file_name(l, string_size(c));\n"
                  "    return string_size(d) + string_size(e) + string_size(f);\n"
                  "}\n",
                  &errors) != nullptr);
    CHECK(errors.empty());
}
#endif

TEST_CASE("text's builtins' errors, each once, at its column", "[codegen][string-type][errors]")
{
#if FMINUS_HAS_JSON && FMINUS_HAS_XML && FMINUS_HAS_FILES
    CHECK(errorsOf("void f(json_doc j, xml_doc x, file_listing l, string s)\n"
                   "{\n"
                   "    string a = json_string(j, \"/a\");\n"
                   "    string b = json_string(j, s, \"x\");\n"
                   "    string c = xml_string(x, \"/a\", 3);\n"
                   "    string d = file_name(l);\n"
                   "    int e = json_string(j, \"/a\", \"x\");\n"
                   "    string g = file_name(j, 0);\n"
                   "}\n") == "test.fm:3:16: error: too few arguments to function call, expected at least 3, have 2\n"
                             "test.fm:4:31: error: argument to 'json_string' must be a string literal\n"
                             "test.fm:5:16: error: passing 'int' to parameter of incompatible type 'string'\n"
                             "test.fm:6:16: error: too few arguments to function call, expected 2, have 1\n"
                             "test.fm:7:9: error: initializing 'int' with an expression of incompatible type 'string'\n"
                             "test.fm:8:16: error: incompatible pointer types passing 'json_doc' to parameter of type "
                             "'file_listing'\n");
#endif
#if !FMINUS_HAS_JSON
    CHECK(errorsOf("int f(json_doc d) { return string_size(json_string(d, \"/a\", \"\")); }") ==
          "test.fm:1:40: error: 'json_string' needs the JSON parser, which this build of FMinus leaves out "
          "(FMINUS_JSON=OFF)\n");
#endif
#if !FMINUS_HAS_WEB_SERVER
    CHECK(errorsOf("int f(http_request q) { return string_size(http_path(q)); }") ==
          "test.fm:1:44: error: 'http_path' needs the web server, which this build of FMinus leaves out "
          "(FMINUS_WEB_SERVER=OFF)\n");
#endif
}

#if FMINUS_HAS_FILES && FMINUS_HAS_JSON && FMINUS_HAS_XML
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
            path_ = tree / "document-text-test" / ("t" + process + "-" + std::to_string(made++));
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
}   // namespace

TEST_CASE("file_name: a listing's own name of each file, in its order; empty outside it; a hole of a later read",
          "[library][files][string-type]")
{
    const Scratch scratch;
    put(scratch / "presets/warm.json", "{\"gain\": 0.5}");
    put(scratch / "presets/bright pad.json", "{\"gain\": 0.75}");
    put(scratch / "presets/a.json", "{\"gain\": 0.25}");
    put(scratch / "presets/notes.txt", "not listed");
    fminus::Program::Options options;
    options.files.folders = {{"presets", (scratch / "presets").string(), false, {}}};
    std::string errors;
    auto program = fminus::Program::compile(
        "file_listing list() { return file_list(\"presets/*.json\"); }\n"
        "int name(file_listing l, int i, char* out, int cap) { return string_copy(out, cap, file_name(l, i)); }\n"
        "// The i-th preset's gain, read again by its name: a listing's name goes into a hole.\n"
        "float gain(file_listing l, int i)\n"
        "{\n"
        "    const string name = file_name(l, i);\n"
        "    file_content c = file_read(\"presets/{name}\");\n"
        "    defer file_close(c);\n"
        "    json_doc doc = json_parse(file_data(c), file_size(c));\n"
        "    defer json_free(doc);\n"
        "    return file_status(c) == FILE_OK ? json_float(doc, \"/gain\", -1.0f) : (float)file_status(c);\n"
        "}\n"
        "int sizes(file_listing l, int n) [[clang::nonblocking]]\n"
        "{\n"
        "    int sum = 0;\n"
        "    for (int i = -1; i <= n; ++i)\n"
        "        sum = sum * 10 + string_size(file_name(l, i)) % 10;\n"
        "    return sum;\n"
        "}\n",
        errors, options);
    INFO(errors);
    // file_read("presets/{name}") has no extension of its own: the literal must write one.
    CHECK(program == nullptr);
    CHECK(errors.find("a file's name ends with its type, an extension") != std::string::npos);
    program = fminus::Program::compile(
        "file_listing list() { return file_list(\"presets/*.json\"); }\n"
        "int name(file_listing l, int i, char* out, int cap) { return string_copy(out, cap, file_name(l, i)); }\n"
        "float gain(file_listing l, int i)\n"
        "{\n"
        "    const string stem = string_slice(file_name(l, i), 0, string_size(file_name(l, i)) - 5);\n"
        "    file_content c = file_read(\"presets/{stem}.json\");\n"
        "    defer file_close(c);\n"
        "    json_doc doc = json_parse(file_data(c), file_size(c));\n"
        "    defer json_free(doc);\n"
        "    return file_status(c) == FILE_OK ? json_float(doc, \"/gain\", -1.0f) : (float)file_status(c);\n"
        "}\n"
        "int sizes(file_listing l, int n) [[clang::nonblocking]]\n"
        "{\n"
        "    int sum = 0;\n"
        "    for (int i = -1; i <= n; ++i)\n"
        "        sum = sum * 10 + string_size(file_name(l, i)) % 10;\n"
        "    return sum;\n"
        "}\n",
        errors, options);
    INFO(errors);
    REQUIRE(program);
    auto* list = program->function<void*()>("list", errors);
    auto* name = program->function<int(void*, int, char*, int)>("name", errors);
    auto* gain = program->function<float(void*, int)>("gain", errors);
    auto* sizes = program->function<int(void*, int)>("sizes", errors);
    REQUIRE(list);
    REQUIRE(name);
    REQUIRE(gain);
    REQUIRE(sizes);
    void* listing = list();
    REQUIRE(fminus_rt_file_count(listing) == 3);
    const auto named = [&](int i)
    {
        char out[64] = {};
        const int n = name(listing, i, out, 64);
        return std::string(out, static_cast<std::size_t>(n));
    };
    CHECK(named(0) == "a.json");   // in their names' bytes' order
    CHECK(named(1) == "bright pad.json");
    CHECK(named(2) == "warm.json");
    CHECK(named(3).empty());
    CHECK(named(-1).empty());
    CHECK(name(nullptr, 0, nullptr, 0) == 0);
    CHECK(gain(listing, 0) == 0.25f);
    CHECK(gain(listing, 1) == 0.75f);
    CHECK(gain(listing, 2) == 0.5f);
    CHECK(gain(listing, 3) == static_cast<float>(FMINUS_FILE_INVALID));   // ".json": a leading '.'
    // The reads allocate nothing and make no system call: the names' sizes, each % 10, -1 and 3 outside it (0).
    CHECK(sizes(listing, 3) == 6590);
    if (HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                same = same && sizes(listing, 3) == 6590;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
    }
#if defined(__linux__)
    if (const std::optional<int> sealedResult = sealed([&] { return sizes(listing, 3); }))
        CHECK(*sealedResult == 6590);
#endif
    fminus_rt_file_close(listing);
}

TEST_CASE("json_string and xml_string through the JIT allocate nothing and make no system call",
          "[library][json][xml][string-type]")
{
    std::string errors;
    auto program = fminus::Program::compile(
        "json_doc parse_json(const void* data, int size) { return json_parse(data, size); }\n"
        "xml_doc parse_xml(const void* data, int size) { return xml_parse(data, size); }\n"
        "int read(json_doc j, xml_doc x) [[clang::nonblocking]]\n"
        "{\n"
        "    int sum = 0;\n"
        "    for (int i = 0; i < 3; ++i)\n"
        "        sum = sum * 100 + string_size(json_string(j, \"/a/%d\", i, \"fallback\")) * 10\n"
        "              + string_size(xml_string(x, \"/a/b[%d]\", i, \"no\"));\n"
        "    return sum;\n"
        "}\n"
        "void release(json_doc j, xml_doc x) { json_free(j); xml_free(x); }\n",
        errors);
    INFO(errors);
    REQUIRE(program);
    auto* json = program->function<void*(const void*, int)>("parse_json", errors);
    auto* xml = program->function<void*(const void*, int)>("parse_xml", errors);
    auto* read = program->function<int(void*, void*)>("read", errors);
    REQUIRE(json);
    REQUIRE(xml);
    REQUIRE(read);
    const std::string jsonText = "{\"a\":[\"abc\",1,\"\"]}";
    const std::string xmlText = "<a><b>x</b><b> yz </b></a>";
    void* j = json(jsonText.data(), static_cast<int>(jsonText.size()));
    void* x = xml(xmlText.data(), static_cast<int>(xmlText.size()));
    REQUIRE(j != nullptr);
    REQUIRE(x != nullptr);
    const int expected = (3 * 10 + 1) * 10000 + (8 * 10 + 2) * 100 + (0 * 10 + 2);
    CHECK(read(j, x) == expected);
    if (HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                same = same && read(j, x) == expected;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
    }
#if defined(__linux__)
    if (const std::optional<int> sealedResult = sealed([&] { return read(j, x); }))
        CHECK(*sealedResult == expected);
#endif
    auto* release = program->function<void(void*, void*)>("release", errors);
    REQUIRE(release);
    release(j, x);
}
#endif
