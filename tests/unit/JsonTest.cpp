// tests/unit/JsonTest.cpp: the JSON parser and its builtins (extending.md, recipe 39): RFC 8259's JSON and nothing
// else, within the size and depth limits; each read on each kind of value, its fallback when the value is not of its
// kind; a float correctly rounded from its digits in any locale, an int only when the number is one; a path prepared
// when the program is compiled (a JSON Pointer with places for ints), its errors at their columns; the effects,
// parsing off the audio thread and every read on it, and the runtime's promise behind them: a read allocates nothing
// and makes no system call. The runtime's functions are called directly, with paths prepared by the compiler's own
// scanPath, and through programs the JIT runs. In a build without FMINUS_JSON, every json_ builtin is the error that
// names the option.
#include "Compile.h"
#include "HeapCalls.h"
#include "fminus/Config.h"
#include "ir/Lowering.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace fminus::test;

namespace
{
    // Every json_ builtin, and a call of each a program could make.
    constexpr std::string_view kCalls[][2] = {
        {"json_parse", "void* a(const void* d, int n) { return json_parse(d, n); }\n"},
        {"json_free", "void b(void* d) { json_free(d); }\n"},
        {"json_float", "float c(const void* d) { return json_float(d, \"/a\", 0.0f); }\n"},
        {"json_double", "double e(const void* d) { return json_double(d, \"/a\", 0.0); }\n"},
        {"json_int", "int f(const void* d) { return json_int(d, \"/a\", 0); }\n"},
        {"json_bool", "bool g(const void* d) { return json_bool(d, \"/a\", false); }\n"},
        {"json_count", "int h(const void* d) { return json_count(d, \"/a\"); }\n"},
        {"json_floats", "int i(const void* d, float* o) { return json_floats(d, \"/a\", o, 4); }\n"},
        {"json_choice", "int j(const void* d) { return json_choice(d, \"/a\", \"x\", \"y\"); }\n"},
        {"json_base64_floats", "int k(const void* d, float* o) { return json_base64_floats(d, \"/a\", o, 4); }\n"},
    };
} // namespace

#if FMINUS_HAS_JSON
namespace
{
    // A document the runtime parsed (null when it is no JSON), freed at the end.
    struct Document
    {
        void* doc;

        explicit Document(std::string_view text)
            : doc(fminus_rt_json_parse(text.data(), static_cast<std::int32_t>(text.size())))
        {
        }
        ~Document()
        {
            fminus_rt_json_free(doc);
        }
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;
    };

    bool parses(std::string_view text)
    {
        return Document(text).doc != nullptr;
    }

    // A JSON Pointer prepared as the compiler prepares one (ir/Paths.cpp, scanPath), laid out as the program's
    // constant is (runtime/Runtime.h): its counts, its code, then its bytes.
    std::vector<std::int32_t> prepared(std::string_view pointer)
    {
        const fminus::ir::detail::PathScan scan = fminus::ir::detail::scanPath(pointer);
        REQUIRE(scan.errors.empty());
        std::vector<std::int32_t> table{static_cast<std::int32_t>(scan.code.size()),
                                        static_cast<std::int32_t>(scan.bytes.size())};
        table.insert(table.end(), scan.code.begin(), scan.code.end());
        const std::size_t at = table.size();
        table.resize(at + (scan.bytes.size() + 3) / 4);
        std::memcpy(table.data() + at, scan.bytes.data(), scan.bytes.size());
        return table;
    }

    // The reads, through the runtime's functions, at a pointer with its ints.
    float jsonFloat(const Document& d, std::string_view pointer, float fallback,
                    const std::vector<std::int32_t>& ints = {})
    {
        return fminus_rt_json_float(d.doc, prepared(pointer).data(), ints.data(),
                                    static_cast<std::int32_t>(ints.size()), fallback);
    }

    double jsonDouble(const Document& d, std::string_view pointer, double fallback)
    {
        return fminus_rt_json_double(d.doc, prepared(pointer).data(), nullptr, 0, fallback);
    }

    std::int32_t jsonInt(const Document& d, std::string_view pointer, std::int32_t fallback,
                         const std::vector<std::int32_t>& ints = {})
    {
        return fminus_rt_json_int(d.doc, prepared(pointer).data(), ints.data(), static_cast<std::int32_t>(ints.size()),
                                  fallback);
    }

    std::int32_t jsonBool(const Document& d, std::string_view pointer, std::int32_t fallback)
    {
        return fminus_rt_json_bool(d.doc, prepared(pointer).data(), nullptr, 0, fallback);
    }

    std::int32_t jsonCount(const Document& d, std::string_view pointer, const std::vector<std::int32_t>& ints = {})
    {
        return fminus_rt_json_count(d.doc, prepared(pointer).data(), ints.data(),
                                    static_cast<std::int32_t>(ints.size()));
    }

    std::int32_t jsonFloats(const Document& d, std::string_view pointer, float* out, std::int32_t cap)
    {
        return fminus_rt_json_floats(d.doc, prepared(pointer).data(), nullptr, 0, out, cap);
    }

    std::int32_t jsonBase64(const Document& d, std::string_view pointer, float* out, std::int32_t cap)
    {
        return fminus_rt_json_base64_floats(d.doc, prepared(pointer).data(), nullptr, 0, out, cap);
    }

    std::int32_t jsonChoice(const Document& d, std::string_view pointer, const std::vector<std::string_view>& choices)
    {
        std::vector<std::int32_t> table{static_cast<std::int32_t>(choices.size()), 0};
        std::string bytes;
        for (std::string_view choice : choices)
        {
            table.push_back(static_cast<std::int32_t>(choice.size()));
            bytes += choice;
        }
        table[1] = static_cast<std::int32_t>(bytes.size());
        const std::size_t at = table.size();
        table.resize(at + (bytes.size() + 3) / 4);
        std::memcpy(table.data() + at, bytes.data(), bytes.size());
        return fminus_rt_json_choice(d.doc, prepared(pointer).data(), nullptr, 0, table.data());
    }

    std::string nested(int depth, std::string_view open, std::string_view inner, std::string_view close)
    {
        std::string text;
        for (int i = 0; i < depth; ++i)
            text += open;
        text += inner;
        for (int i = 0; i < depth; ++i)
            text += close;
        return text;
    }

    bool sameBits(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    // A preset, as a host would hand one to a program, and the program's reads of it.
    constexpr std::string_view kPreset = R"({
        "filter": {"cutoff": 2500.5, "q": 0.7071},
        "gain": 0.1,
        "master": {"voices": 6, "tuning": 440.0},
        "lfo": [{"sync": true}, {"sync": false}],
        "osc": [{"detune": 0.25, "wave": "saw"}, {"detune": -3, "wave": "square"}, {"detune": 1e-3, "wave": "tri"}],
        "tables": [[1, 2, 3], [4.5, 5.5]],
        "packed": "AACAPwAAAL8AAFBA",
        "settings": {"osc_1_level": 0.75, "osc_-1_level": -0.75},
        "a/b": {"m~n": {"%": 42}}
    })";

    constexpr std::string_view kReads = R"(
        void* parse(const void* data, int bytes) { return json_parse(data, bytes); }
        void release(void* doc) { json_free(doc); }
        float cutoff(const void* doc) [[clang::nonblocking]] { return json_float(doc, "/filter/cutoff", 1000.0f); }
        double gain(const void* doc) [[clang::nonblocking]] { return json_double(doc, "/gain", -1.0); }
        int voices(const void* doc) [[clang::nonblocking]] { return json_int(doc, "/master/voices", 8); }
        bool sync(const void* doc, int k) [[clang::nonblocking]] { return json_bool(doc, "/lfo/%d/sync", k, true); }
        int oscillators(const void* doc) [[clang::nonblocking]] { return json_count(doc, "/osc"); }
        float detune(const void* doc, int k) [[clang::nonblocking]]
        {
            return json_float(doc, "/osc/%d/detune", k, 100.0f);
        }
        enum Wave { Sine, Saw, Square };
        int wave(const void* doc, int k) [[clang::nonblocking]]
        {
            return json_choice(doc, "/osc/%d/wave", k, "sine", "saw", "square");
        }
        int table(const void* doc, int t, float* row, int cap) [[clang::nonblocking]]
        {
            return json_floats(doc, "/tables/%d", t, row, cap);
        }
        int tables(const void* doc, float* out, int cap) [[clang::nonblocking]]
        {
            return json_floats(doc, "/tables", out, cap);
        }
        int packed(const void* doc, float* out, int cap) [[clang::nonblocking]]
        {
            return json_base64_floats(doc, "/packed", out, cap);
        }
        float level(const void* doc, int k) [[clang::nonblocking]]
        {
            return json_float(doc, "/settings/osc_%d_level", k, 9.0f);
        }
        int escaped(const void* doc) [[clang::nonblocking]] { return json_int(doc, "/a~1b/m~0n/%%", 0); }
        int whole(const void* doc) [[clang::nonblocking]] { return json_count(doc, ""); }
        bool mode(const void* doc, enum Wave w) [[clang::nonblocking]] { return json_bool(doc, "/lfo/%d/sync", w, 0); }
    )";

    struct Reads
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        void* (*parse)(const void*, int) = nullptr;
        void (*release)(void*) = nullptr;
        float (*cutoff)(const void*) = nullptr;
        double (*gain)(const void*) = nullptr;
        int (*voices)(const void*) = nullptr;
        bool (*sync)(const void*, int) = nullptr;
        int (*oscillators)(const void*) = nullptr;
        float (*detune)(const void*, int) = nullptr;
        int (*wave)(const void*, int) = nullptr;
        int (*table)(const void*, int, float*, int) = nullptr;
        int (*tables)(const void*, float*, int) = nullptr;
        int (*packed)(const void*, float*, int) = nullptr;
        float (*level)(const void*, int) = nullptr;
        int (*escaped)(const void*) = nullptr;
        int (*whole)(const void*) = nullptr;

        Reads()
        {
            std::string errors;
            jit = compile(kReads, &errors);
            INFO(errors);
            REQUIRE(jit);
            parse = lookup<void*(const void*, int)>(*jit, "parse");
            release = lookup<void(void*)>(*jit, "release");
            cutoff = lookup<float(const void*)>(*jit, "cutoff");
            gain = lookup<double(const void*)>(*jit, "gain");
            voices = lookup<int(const void*)>(*jit, "voices");
            sync = lookup<bool(const void*, int)>(*jit, "sync");
            oscillators = lookup<int(const void*)>(*jit, "oscillators");
            detune = lookup<float(const void*, int)>(*jit, "detune");
            wave = lookup<int(const void*, int)>(*jit, "wave");
            table = lookup<int(const void*, int, float*, int)>(*jit, "table");
            tables = lookup<int(const void*, float*, int)>(*jit, "tables");
            packed = lookup<int(const void*, float*, int)>(*jit, "packed");
            level = lookup<float(const void*, int)>(*jit, "level");
            escaped = lookup<int(const void*)>(*jit, "escaped");
            whole = lookup<int(const void*)>(*jit, "whole");
        }
    };
} // namespace

TEST_CASE("json_parse takes RFC 8259's JSON, and nothing else", "[runtime][json]")
{
    // Valid: any value at the top, whitespace around it, every escape, any number JSON writes, duplicate names.
    for (std::string_view valid : {
             "[]", "{}", "0", "-0", "\"\"", "\"x\"", "true", "false", "null", " \t\r\n[1]\n ",
             "[1e1, 1E+1, 1e-1, -1.5e+2]",
             "{\"a\": 1, \"a\": 2}", "[\"\\u0000\"]", "[\"\\ud83d\\ude00\"]", "[\"\\/\\b\\f\\n\\r\\t\\\\\\\"\"]",
             "[\"\xe2\x82\xac\xf0\x9d\x84\x9e\"]", "[123456789012345678901234567890]", "[1E400, -1e-400]",
             "{\"\": {\"\": []}}", "[[[]], {}, [{}]]",
         })
    {
        INFO(valid);
        CHECK(parses(valid));
    }
    // Invalid: nothing, a truncated document, anything JSON5 or C would take, a control character or a NUL in a
    // string, a byte after the value (a NUL among them), a byte order mark, and UTF-8 that is not: a byte that never
    // starts a character, an overlong form, a surrogate's, a truncated sequence.
    for (std::string_view invalid : {
             "", " ", "\n", "[", "[1,", "[1, 2", "{\"a\":", "{\"a\": 1", "\"abc", "tru", "nul", "[1,]", "[1 2]",
             "{\"a\"}", "{\"a\":}", "{a: 1}", "['a']", "[01]", "[+1]", "[.5]", "[1.]", "[1e]", "[0x1]", "[NaN]",
             "[Infinity]", "[-]", "[1]x", "[1]]", "\"\\x\"", "\"\\u12\"", "\"a\tb\"", "/* c */ 1", "1 // c",
             "\xef\xbb\xbf[]", "[\"\xff\"]", "[\"\xc0\xaf\"]", "[\"\xed\xa0\x80\"]", "[\"\xe2\x82\"]", "[\"\x80\"]",
         })
    {
        INFO(invalid);
        CHECK(!parses(invalid));
    }
    CHECK(!parses(std::string_view("\"a\0b\"", 5)));   // a NUL in a string
    CHECK(!parses(std::string_view("[1]\0", 4)));      // a NUL after the value
    CHECK(!parses(std::string_view("\0", 1)));
    CHECK(parses(std::string_view("[1]\n", 4)));
    // No data, or a size that is not positive, is no document.
    CHECK(fminus_rt_json_parse(nullptr, 2) == nullptr);
    CHECK(fminus_rt_json_parse("[]", 0) == nullptr);
    CHECK(fminus_rt_json_parse("[]", -2) == nullptr);
    // The document holds what it needs: the text may change, or go, once it is parsed.
    std::string text = "{\"a\": [1.5, \"b\"]}";
    const Document d(text);
    REQUIRE(d.doc != nullptr);
    std::fill(text.begin(), text.end(), 'x');
    CHECK(jsonFloat(d, "/a/0", 0.0f) == 1.5f);
    CHECK(jsonChoice(d, "/a/1", {"a", "b"}) == 1);
    fminus_rt_json_free(nullptr);   // nothing
}

TEST_CASE("json_parse refuses a document nested past its depth or larger than its size", "[runtime][json]")
{
    constexpr int kDepth = FMINUS_JSON_MAX_DEPTH;
    CHECK(parses(nested(kDepth, "[", "", "]")));
    CHECK(!parses(nested(kDepth + 1, "[", "", "]")));
    CHECK(parses(nested(kDepth / 2, "{\"a\":[", "1", "]}")));   // two levels each
    CHECK(!parses(nested(kDepth / 2 + 1, "{\"a\":[", "1", "]}")));
    // Far past it, opened and never closed, or closed: refused, and without recursion (no stack to run out of).
    CHECK(!parses(std::string(1000000, '[')));
    CHECK(!parses(nested(100000, "[", "", "]")));
    // At the depth, every level is there to read.
    const Document deep(nested(kDepth - 1, "[", "7", "]"));
    REQUIRE(deep.doc != nullptr);
    std::string pointer;
    for (int i = 0; i < kDepth - 1; ++i)
        pointer += "/0";
    CHECK(jsonInt(deep, pointer, -1) == 7);

    // The size: FMINUS_JSON_MAX_BYTES at most, whatever the bytes are.
    std::string large(FMINUS_JSON_MAX_BYTES, ' ');
    large.back() = '0';
    CHECK(parses(large));
    large.push_back(' ');
    CHECK(!parses(large));
}

TEST_CASE("a 10 MB array of numbers parses, and json_floats reads every one of them", "[runtime][json]")
{
    std::string text = "[";
    std::int64_t expected = 0;
    int n = 0;
    while (text.size() < 10 * 1000 * 1000)
    {
        const int value = static_cast<int>(std::int64_t{n} * 7919 % 20001) - 10000;   // -10000 to 10000
        text += std::to_string(value) + ".25,";   // exactly a float: value + 0.25, or value - 0.25 below 0
        expected += 4 * value + (value < 0 ? -1 : 1);
        ++n;
    }
    text.back() = ']';
    const Document d(text);
    REQUIRE(d.doc != nullptr);
    CHECK(jsonCount(d, "") == n);
    std::vector<float> out(static_cast<std::size_t>(n));
    CHECK(jsonFloats(d, "", out.data(), n) == n);
    std::int64_t sum = 0;
    for (float x : out)
        sum += static_cast<std::int64_t>(4 * x);
    CHECK(sum == expected);
    CHECK(jsonFloat(d, "/%d", 0.0f, {n - 1}) == out.back());
}

TEST_CASE("json_float is the nearest float to the number's digits, never a double narrowed", "[runtime][json]")
{
    const Document d(R"({"zero": 0, "minus zero": -0, "tenth": 0.1, "exp": [1E+2, 1e-2, 2.5e0, -1E-0],
                         "rounding": [1.00000005960464477550, 16777217.000000001, 1.000000059604644775390625],
                         "huge": 1e39, "tiny": 1e-46, "least": 1.4e-45, "max": 3.4028235e38, "big": 1e300,
                         "integer": 123456789, "long": 0.1000000000000000000000000000000000000000000000000000001,
                         "string": "0.5", "true": true, "null": null, "array": [0.5], "object": {"a": 0.5}})");
    REQUIRE(d.doc != nullptr);
    CHECK(sameBits(jsonFloat(d, "/zero", 9.0f), 0.0f));
    CHECK(sameBits(jsonFloat(d, "/minus zero", 9.0f), -0.0f));
    CHECK(jsonFloat(d, "/tenth", 9.0f) == 0.1f);
    CHECK(jsonFloat(d, "/exp/0", 9.0f) == 100.0f);
    CHECK(jsonFloat(d, "/exp/1", 9.0f) == 0.01f);
    CHECK(jsonFloat(d, "/exp/2", 9.0f) == 2.5f);
    CHECK(jsonFloat(d, "/exp/3", 9.0f) == -1.0f);
    // Past halfway between two floats by less than a double can see: the double is halfway, and narrowing it rounds
    // to even, the wrong way. Read straight, each is the float above. Exactly halfway, it is the even one.
    CHECK(jsonFloat(d, "/rounding/0", 9.0f) == std::nextafter(1.0f, 2.0f));
    CHECK(static_cast<float>(1.00000005960464477550) == 1.0f);   // what a double narrowed gives
    CHECK(jsonFloat(d, "/rounding/1", 9.0f) == 16777218.0f);
    CHECK(static_cast<float>(16777217.000000001) == 16777216.0f);
    CHECK(jsonFloat(d, "/rounding/2", 9.0f) == 1.0f);
    // Past a float's range, an infinity; nearer zero than its least subnormal, a zero; the least subnormal; the
    // largest.
    CHECK(jsonFloat(d, "/huge", 9.0f) == std::numeric_limits<float>::infinity());
    CHECK(jsonFloat(d, "/big", 9.0f) == std::numeric_limits<float>::infinity());
    CHECK(sameBits(jsonFloat(d, "/tiny", 9.0f), 0.0f));
    CHECK(jsonFloat(d, "/least", 9.0f) == std::numeric_limits<float>::denorm_min());
    CHECK(jsonFloat(d, "/max", 9.0f) == std::numeric_limits<float>::max());
    CHECK(jsonFloat(d, "/integer", 9.0f) == 123456792.0f);
    CHECK(jsonFloat(d, "/long", 9.0f) == 0.1f);
    // Not a number: the fallback, whatever the value is.
    for (std::string_view other : {"/string", "/true", "/null", "/array", "/object", "/missing", "/tenth/0"})
    {
        INFO(other);
        CHECK(jsonFloat(d, other, 9.0f) == 9.0f);
    }
    // json_double: the nearest double, which is not the float widened.
    CHECK(jsonDouble(d, "/tenth", 9.0) == 0.1);
    CHECK(jsonDouble(d, "/big", 9.0) == 1e300);
    CHECK(jsonDouble(d, "/huge", 9.0) == 1e39);
    CHECK(jsonDouble(d, "/rounding/1", 9.0) == 16777217.0);
    CHECK(jsonDouble(d, "/string", 9.0) == 9.0);
}

TEST_CASE("json_float reads the same digits the same way in a locale that writes a decimal comma", "[runtime][json]")
{
    // A German or French locale, where C's strtod reads "0.5" as 0: if this machine has one (Linux has one when it is
    // generated, localedef -i de_DE -f UTF-8, into the system's locales or a directory LOCPATH names).
    const std::string before = std::setlocale(LC_ALL, nullptr);
    const char* found = nullptr;
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8", "German_Germany.1252"})
    {
        if ((found = std::setlocale(LC_ALL, name)) != nullptr)
            break;
    }
    if (found == nullptr)
        SKIP("no locale with a decimal comma on this machine");
    const double cLibrary = std::strtod("0.5", nullptr);
    const Document d(R"({"half": 0.5, "pi": 3.14159265, "list": [1.25, -2.5e-1]})");
    float list[2] = {};
    const float half = jsonFloat(d, "/half", 9.0f);
    const double pi = jsonDouble(d, "/pi", 9.0);
    const std::int32_t n = jsonFloats(d, "/list", list, 2);
    std::setlocale(LC_ALL, before.c_str());
    INFO(found);
    CHECK(cLibrary == 0.0);   // the locale is in effect: the C library stops at the '.'
    CHECK(half == 0.5f);
    CHECK(pi == 3.14159265);
    CHECK(n == 2);
    CHECK(list[0] == 1.25f);
    CHECK(list[1] == -0.25f);
}

TEST_CASE("json_int is the number when it is an integer an int holds, and never truncates or saturates",
          "[runtime][json]")
{
    const Document d(R"([7, -0, 7.0, 700e-2, 0.07e2, 1e9, 2147483647, -2147483648, 0.0000001e7, 0e99999999999,
                         1.5, 3e9, 2147483648, -2147483649, 1e10, 1e-1, 2147483647.0000000001, 1e99999999999,
                         "7", true, null, [7], {"a": 7}])");
    REQUIRE(d.doc != nullptr);
    const std::int32_t integers[] = {7, 0, 7, 7, 7, 1000000000, 2147483647, -2147483647 - 1, 1, 0};
    for (std::int32_t k = 0; k < 10; ++k)
    {
        INFO(k);
        CHECK(jsonInt(d, "/%d", -99, {k}) == integers[k]);
    }
    for (std::int32_t k = 10; k < 23; ++k)
    {
        INFO(k);
        CHECK(jsonInt(d, "/%d", -99, {k}) == -99);
    }
}

TEST_CASE("json_bool, json_count, json_choice: each kind of value, and the fallbacks", "[runtime][json]")
{
    const Document d(R"({"t": true, "f": false, "n": null, "one": 1, "zero": 0, "s": "true",
                         "list": [1, "a", null, [], {}], "empty": [], "object": {"a": 1, "b": 2, "a": 3}, "nothing": {},
                         "wave": "saw", "nul": "s\u0000w", "unicode": "\u00e9t\u00e9", "prefix": "sa"})");
    REQUIRE(d.doc != nullptr);
    CHECK(jsonBool(d, "/t", 7) == 1);
    CHECK(jsonBool(d, "/f", 7) == 0);
    for (std::string_view other : {"/n", "/one", "/zero", "/s", "/list", "/object", "/missing"})
    {
        INFO(other);
        CHECK(jsonBool(d, other, 7) == 7);
    }
    CHECK(jsonCount(d, "/list") == 5);
    CHECK(jsonCount(d, "/empty") == 0);
    CHECK(jsonCount(d, "/object") == 3);   // members, a name twice among them
    CHECK(jsonCount(d, "/nothing") == 0);
    CHECK(jsonCount(d, "") == 14);
    for (std::string_view other : {"/t", "/n", "/one", "/s", "/missing"})
    {
        INFO(other);
        CHECK(jsonCount(d, other) == 0);
    }
    CHECK(jsonInt(d, "/object/a", 0) == 1);   // the first member of the name
    CHECK(jsonChoice(d, "/wave", {"sine", "saw", "square"}) == 1);
    CHECK(jsonChoice(d, "/wave", {"saw", "saw"}) == 0);
    CHECK(jsonChoice(d, "/wave", {"Saw", "sa", "saws", ""}) == -1);   // byte for byte, the whole string
    CHECK(jsonChoice(d, "/prefix", {"saw", "sa"}) == 1);
    CHECK(jsonChoice(d, "/nul", {"s", std::string_view("s\0w", 3)}) == 1);
    CHECK(jsonChoice(d, "/unicode", {"\xc3\xa9t\xc3\xa9"}) == 0);
    for (std::string_view other : {"/t", "/one", "/list", "/object", "/missing"})
    {
        INFO(other);
        CHECK(jsonChoice(d, other, {"true", "1"}) == -1);
    }
    CHECK(jsonChoice(d, "/s", {"true"}) == 0);
}

TEST_CASE("json_floats reads an array of numbers, or of rows of them, at most cap, and counts them all",
          "[runtime][json]")
{
    const Document d(R"({"flat": [1, 2.5, -3e1, 0.1], "rows": [[1, 2], [], [3], [4, 5, 6]], "empty": [],
                         "mixed": [1, [2]], "string": [1, "2"], "deep": [[1, [2]]], "nulls": [null],
                         "object": {"a": 1}, "number": 1})");
    REQUIRE(d.doc != nullptr);
    float out[8];
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(jsonFloats(d, "/flat", out, 8) == 4);
    CHECK((out[0] == 1.0f && out[1] == 2.5f && out[2] == -30.0f && out[3] == 0.1f && out[4] == -1.0f));
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(jsonFloats(d, "/rows", out, 8) == 6);   // row after row
    CHECK((out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4 && out[4] == 5 && out[5] == 6 && out[6] == -1));
    // A cap shorter than the array: the first cap of them, and the count of all; a cap of 0, or no out, none.
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(jsonFloats(d, "/rows", out, 4) == 6);
    CHECK((out[3] == 4.0f && out[4] == -1.0f));
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(jsonFloats(d, "/flat", out, 0) == 4);
    CHECK(jsonFloats(d, "/flat", out, -5) == 4);
    CHECK(jsonFloats(d, "/flat", nullptr, 8) == 4);
    CHECK(out[0] == -1.0f);
    // Anything but numbers, or rows of them: 0, and nothing written.
    for (std::string_view other : {"/empty", "/mixed", "/string", "/deep", "/nulls", "/object", "/number", "/missing"})
    {
        INFO(other);
        CHECK(jsonFloats(d, other, out, 8) == 0);
        CHECK(out[0] == -1.0f);
    }
}

TEST_CASE("json_base64_floats decodes a base64 string as little-endian float32s, without allocating",
          "[runtime][json]")
{
    // 1, -0.5 and 3.25: with padding and without; and 0.25 and one byte more, which is no float.
    const Document d(R"({"three": "AACAPwAAAL8AAFBA", "padded": "AACAPgE=", "unpadded": "AACAPgE", "empty": "",
                         "bad": "AACA?wAA", "lone": "AACAP", "inside": "AA=APwAA", "three pads": "AACAP===",
                         "spaces": "AACA PwAA", "number": 1, "array": ["AACAPw=="]})");
    REQUIRE(d.doc != nullptr);
    float out[4] = {-1, -1, -1, -1};
    CHECK(jsonBase64(d, "/three", out, 4) == 3);
    CHECK((out[0] == 1.0f && out[1] == -0.5f && out[2] == 3.25f && out[3] == -1.0f));
    CHECK(jsonBase64(d, "/three", out + 3, 1) == 3);   // a cap of 1: the first only
    CHECK(out[3] == 1.0f);
    std::fill(std::begin(out), std::end(out), -1.0f);
    CHECK(jsonBase64(d, "/padded", out, 4) == 1);
    CHECK((out[0] == 0.25f && out[1] == -1.0f));
    CHECK(jsonBase64(d, "/unpadded", out, 4) == 1);
    CHECK(jsonBase64(d, "/three", out, 0) == 3);
    CHECK(jsonBase64(d, "/three", nullptr, 4) == 3);
    CHECK(jsonBase64(d, "/empty", out, 4) == 0);
    std::fill(std::begin(out), std::end(out), -1.0f);
    for (std::string_view other : {"/bad", "/lone", "/inside", "/three pads", "/spaces", "/number", "/array", "/x"})
    {
        INFO(other);
        CHECK(jsonBase64(d, other, out, 4) == 0);
        CHECK(out[0] == -1.0f);
    }
}

TEST_CASE("a path is a JSON Pointer: escapes, the root, empty names, %d in and out of range", "[runtime][json]")
{
    const Document d(R"({"a/b": {"m~n": 1, "~1": 2}, "": {"": 3}, "%": 4, "a\u0000b": 5, "list": [10, 11, 12],
                         "2": 6, "-1": 7, "osc_2_x": 8, "01": 9, "\u00e9": 13, " ": 14})");
    REQUIRE(d.doc != nullptr);
    CHECK(jsonInt(d, "/a~1b/m~0n", 0) == 1);
    CHECK(jsonInt(d, "/a~1b/~01", 0) == 2);    // ~01 is "~1": the escapes are undone once, in order
    CHECK(jsonInt(d, "/", 0) == 0);             // an object: no number
    CHECK(jsonInt(d, "//", 0) == 3);
    CHECK(jsonInt(d, "/%%", 0) == 4);
    CHECK(jsonInt(d, std::string_view("/a\0b", 4), 0) == 5);
    CHECK(jsonInt(d, "/\xc3\xa9", 0) == 13);
    CHECK(jsonInt(d, "/ ", 0) == 14);
    CHECK(jsonCount(d, "") == 11);
    // %d: an array's index, or an object's member whose name is the int in decimal, or part of one.
    for (std::int32_t k = 0; k < 3; ++k)
        CHECK(jsonInt(d, "/list/%d", 0, {k}) == 10 + k);
    CHECK(jsonInt(d, "/list/%d", -1, {3}) == -1);   // past the end
    CHECK(jsonInt(d, "/list/%d", -1, {-1}) == -1);  // negative: no index
    CHECK(jsonInt(d, "/list/%d", -1, {std::numeric_limits<std::int32_t>::min()}) == -1);
    CHECK(jsonInt(d, "/list/%d", -1, {std::numeric_limits<std::int32_t>::max()}) == -1);
    CHECK(jsonInt(d, "/%d", 0, {2}) == 6);
    CHECK(jsonInt(d, "/%d", 0, {-1}) == 7);
    CHECK(jsonInt(d, "/osc_%d_x", 0, {2}) == 8);
    CHECK(jsonInt(d, "/%d%d", 0, {0, 1}) == 9);
    // An index as RFC 6901 writes it: no leading zero, no sign, no "-" (past the end).
    CHECK(jsonInt(d, "/list/01", -1) == -1);
    CHECK(jsonInt(d, "/list/-", -1) == -1);
    CHECK(jsonInt(d, "/list/+1", -1) == -1);
    CHECK(jsonInt(d, "/list/1", -1) == 11);
    CHECK(jsonInt(d, "/list/", -1) == -1);
    // A place without an int (the compiler never makes one): nothing there.
    CHECK(jsonInt(d, "/list/%d", -1) == -1);
    CHECK(fminus_rt_json_int(d.doc, prepared("/list/%d").data(), nullptr, 1, -1) == -1);
}

TEST_CASE("a null document is an empty one, and a malformed table reaches nothing", "[runtime][json]")
{
    const Document none("not JSON");
    REQUIRE(none.doc == nullptr);
    float out[2] = {-1, -1};
    CHECK(jsonFloat(none, "", 9.0f) == 9.0f);
    CHECK(jsonDouble(none, "/a", 9.0) == 9.0);
    CHECK(jsonInt(none, "/a", 9) == 9);
    CHECK(jsonBool(none, "/a", 9) == 9);
    CHECK(jsonCount(none, "") == 0);
    CHECK(jsonFloats(none, "", out, 2) == 0);
    CHECK(jsonBase64(none, "", out, 2) == 0);
    CHECK(jsonChoice(none, "", {"a"}) == -1);
    CHECK(out[0] == -1.0f);

    // The runtime trusts no word of a table: a count or a piece past its end, a piece outside its bytes.
    const Document d(R"({"a": 1})");
    REQUIRE(d.doc != nullptr);
    const std::int32_t good[] = {3, 1, 1, 0, 1, 'a'};
    CHECK(fminus_rt_json_int(d.doc, good, nullptr, 0, -1) == 1);
    const std::int32_t past[] = {3, 1, 2, 0, 1, 'a'};
    CHECK(fminus_rt_json_int(d.doc, past, nullptr, 0, -1) == -1);
    const std::int32_t outside[] = {3, 1, 1, 0, 2, 'a'};
    CHECK(fminus_rt_json_int(d.doc, outside, nullptr, 0, -1) == -1);
    const std::int32_t negative[] = {-3, 1};
    CHECK(fminus_rt_json_int(d.doc, negative, nullptr, 0, -1) == -1);
    CHECK(fminus_rt_json_int(d.doc, nullptr, nullptr, 0, -1) == -1);
    const std::int32_t choices[] = {1, 5, 9, 'a'};   // a length past the bytes
    CHECK(fminus_rt_json_choice(d.doc, prepared("").data(), nullptr, 0, choices) == -1);
}

TEST_CASE("a program reads a document through the JIT: each builtin, its fallback, %d", "[codegen][json]")
{
    const Reads reads;
    void* doc = reads.parse(kPreset.data(), static_cast<int>(kPreset.size()));
    REQUIRE(doc != nullptr);
    CHECK(reads.cutoff(doc) == 2500.5f);
    CHECK(reads.gain(doc) == 0.1);
    CHECK(reads.voices(doc) == 6);
    CHECK(reads.sync(doc, 0));
    CHECK(!reads.sync(doc, 1));
    CHECK(reads.sync(doc, 2));   // the fallback
    CHECK(reads.oscillators(doc) == 3);
    CHECK(reads.detune(doc, 0) == 0.25f);
    CHECK(reads.detune(doc, 1) == -3.0f);
    CHECK(reads.detune(doc, 2) == 0.001f);
    CHECK(reads.detune(doc, 3) == 100.0f);
    CHECK(reads.detune(doc, -1) == 100.0f);
    CHECK(reads.wave(doc, 0) == 1);
    CHECK(reads.wave(doc, 1) == 2);
    CHECK(reads.wave(doc, 2) == -1);
    float row[4] = {-1, -1, -1, -1};
    CHECK(reads.table(doc, 1, row, 4) == 2);
    CHECK((row[0] == 4.5f && row[1] == 5.5f && row[2] == -1.0f));
    CHECK(reads.table(doc, 0, row, 2) == 3);
    CHECK((row[0] == 1.0f && row[1] == 2.0f && row[2] == -1.0f));
    CHECK(reads.table(doc, 2, row, 4) == 0);
    CHECK(reads.tables(doc, row, 4) == 5);
    CHECK((row[0] == 1.0f && row[3] == 4.5f));
    CHECK(reads.packed(doc, row, 4) == 3);
    CHECK((row[0] == 1.0f && row[1] == -0.5f && row[2] == 3.25f));
    CHECK(reads.level(doc, 1) == 0.75f);
    CHECK(reads.level(doc, -1) == -0.75f);
    CHECK(reads.level(doc, 0) == 9.0f);
    CHECK(reads.escaped(doc) == 42);
    CHECK(reads.whole(doc) == 9);
    reads.release(doc);
    // A null document: every fallback.
    CHECK(reads.cutoff(nullptr) == 1000.0f);
    CHECK(reads.voices(nullptr) == 8);
    CHECK(reads.oscillators(nullptr) == 0);
    CHECK(reads.wave(nullptr, 0) == -1);
    CHECK(reads.parse("{", 1) == nullptr);
    reads.release(nullptr);
}

TEST_CASE("every read allocates nothing and makes no system call; parsing allocates", "[runtime][json]")
{
    const Reads reads;
    void* doc = reads.parse(kPreset.data(), static_cast<int>(kPreset.size()));
    REQUIRE(doc != nullptr);
    float out[8];
    // Every read of the program's, once; what they give, summed.
    const auto readAll = [&] {
        double sum = reads.gain(doc) + reads.voices(doc) + reads.sync(doc, 0) + reads.oscillators(doc)
                     + reads.wave(doc, 1) + reads.escaped(doc) + reads.whole(doc);
        sum += static_cast<double>(reads.cutoff(doc)) + static_cast<double>(reads.level(doc, 1));
        for (int k = -1; k < 4; ++k)
            sum += static_cast<double>(reads.detune(doc, k)) + reads.table(doc, k, out, 8) + reads.wave(doc, k);
        sum += reads.tables(doc, out, 8) + reads.packed(doc, out, 8) + static_cast<double>(out[0]);
        sum += static_cast<double>(reads.cutoff(nullptr)) + reads.tables(nullptr, out, 8);
        return sum;
    };
    const double expected = readAll();   // and every function called once before it is counted

    if (HeapCalls::counted())
    {
        long reading = 0;
        bool same = true;
        {
            const HeapCalls calls;
            for (int i = 0; i < 100; ++i)
                same = same && readAll() == expected;
            reading = calls.count();
        }
        CHECK(same);
        CHECK(reading == 0);
        long parsing = 0;
        {
            const HeapCalls calls;
            void* other = reads.parse(kPreset.data(), static_cast<int>(kPreset.size()));
            reads.release(other);
            parsing = calls.count();
        }
        CHECK(parsing >= 2);   // the counter sees the runtime's heap: a document, and its free
    }
#if defined(__linux__)
    // In a child under seccomp's strict mode, where any system call but read, write and exit kills it (no futex, no
    // mmap, no brk): the reads, and their sum written back.
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::close(fds[0]);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
            ::syscall(SYS_exit, 2);   // no seccomp here
        const double sum = readAll();
        const ssize_t written = ::write(fds[1], &sum, sizeof sum);
        ::syscall(SYS_exit, written == sizeof sum ? 0 : 1);
    }
    ::close(fds[1]);
    int status = 0;
    ::waitpid(child, &status, 0);
    double sum = 0;
    const ssize_t got = ::read(fds[0], &sum, sizeof sum);
    ::close(fds[0]);
    INFO("status " << status);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        WARN("seccomp's strict mode is not available here: the system calls are not checked");
    else
    {
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(got == static_cast<ssize_t>(sizeof sum));
        CHECK(sum == expected);
    }
#endif
    reads.release(doc);
}
#endif

TEST_CASE("the effects: json_parse and json_free off the audio thread, every read on it", "[codegen][json][effects]")
{
    using namespace fminus::ir::detail;
    for (std::string_view name : {"json_parse", "json_free"})
    {
        INFO(name);
        CHECK(allocates(name));
        CHECK(!blocks(name));
    }
    for (std::string_view name :
         {"json_float", "json_double", "json_int", "json_bool", "json_count", "json_floats", "json_choice",
          "json_base64_floats"})
    {
        INFO(name);
        CHECK(!allocates(name));
        CHECK(!blocks(name));
    }
    if (FMINUS_HAS_JSON == 0)
        return;   // a call of one is an error of its own (below)
    std::string errors;
    auto jit = compile("module Preset\n"
                       "{\n"
                       "    void* doc;\n"
                       "    float cutoff;\n"
                       "    bool load(const void* data, int bytes)\n"
                       "    {\n"
                       "        doc = json_parse(data, bytes);\n"
                       "        if (!doc)\n"
                       "            return false;\n"
                       "        return true;\n"
                       "    }\n"
                       "    void process(float* out, int n) [[clang::nonblocking]]\n"
                       "    {\n"
                       "        cutoff = json_float(doc, \"/cutoff\", cutoff);\n"
                       "        for (int i = 0; i < n; ++i)\n"
                       "            out[i] = cutoff + json_floats(doc, \"/table\", out, 0);\n"
                       "    }\n"
                       "    void end() { json_free(doc); }\n"
                       "};\n",
                       &errors);
    INFO(errors);
    CHECK(jit);
    CHECK(errorsOf("void* open(const void* data, int bytes) { return json_parse(data, bytes); }\n"
                   "float process(const void* data, int bytes) [[clang::nonblocking]]\n"
                   "{\n"
                   "    void* doc = open(data, bytes);\n"
                   "    const float x = json_float(doc, \"/x\", 0.0f);\n"
                   "    json_free(doc);\n"
                   "    return x;\n"
                   "}\n"
                   "void close(void* doc) [[clang::nonallocating]] { json_free(doc); }\n"
                   "void* again(const void* data) [[clang::nonallocating]] { return json_parse(data, 2); }\n")
          == "test.fm:4:17: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'open'\n"
             "test.fm:1:50: note: function cannot be inferred 'nonblocking' because it calls non-'nonblocking' "
             "function 'json_parse'\n"
             "test.fm:6:5: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'json_free'\n"
             "test.fm:9:50: error: function with 'nonallocating' attribute must not call non-'nonallocating' function "
             "'json_free'\n"
             "test.fm:10:65: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'json_parse'\n");
}

#if FMINUS_HAS_JSON
TEST_CASE("a path's errors are at its own bytes, and its ints are a format's", "[codegen][json]")
{
    // Not a JSON Pointer: at its first byte; an escape that is not ~0 or ~1: at its ~.
    CHECK(errorsOf("float a(const void* d) { return json_float(d, \"osc\", 0.0f); }\n")
          == "test.fm:1:48: error: a JSON Pointer must be empty or begin with '/'\n");
    CHECK(errorsOf("float b(const void* d) { return json_float(d, \"/a~2/b~\", 0.0f); }\n")
          == "test.fm:1:50: error: unknown escape sequence '~2' in a JSON Pointer: '~0' is '~' and '~1' is '/'\n"
             "test.fm:1:54: error: incomplete escape sequence '~' at the end of a JSON Pointer: '~0' is '~' and '~1' "
             "is '/'\n");
    // Its places: %d and %%, checked as a format's (Strings.cpp), at clang's columns.
    CHECK(errorsOf("float c(const void* d, int k) { return json_float(d, \"/osc/%f\", k, 0.0f); }\n")
          == "test.fm:1:61: error: invalid conversion specifier 'f'\n"
             "test.fm:1:61: note: a format here takes %d, an int, and %%, a percent sign\n");
    CHECK(errorsOf("float e(const void* d) { return json_float(d, \"/osc/%d/x\", 0.0f); }\n")
          == "test.fm:1:54: error: more '%' conversions than data arguments\n");
    CHECK(errorsOf("int f(const void* d, int k) { return json_count(d, \"/osc\", k); }\n")
          == "test.fm:1:60: error: data argument not used by format string\n");
    CHECK(errorsOf("int g(const void* d, float k) { return json_count(d, \"/osc/%d\", k); }\n")
          == "test.fm:1:65: error: format specifies type 'int' but the argument has type 'float'\n");
    // The path and the choices are literals; json_choice takes one at least.
    CHECK(errorsOf("int h(const void* d, int k) { return json_count(d, k); }\n"
                   "int i(const void* d, int k) { return json_choice(d, \"/w\", \"a\", k); }\n"
                   "int j(const void* d, int k) { return json_choice(d, \"/w/%d\", k); }\n"
                   "int k(const void* d) { return json_choice(d, \"/w\"); }\n")
          == "test.fm:1:52: error: argument to 'json_count' must be a string literal\n"
             "test.fm:2:64: error: argument to 'json_choice' must be a string literal\n"
             "test.fm:3:38: error: too few arguments to function call, expected at least 4, have 3\n"
             "test.fm:4:31: error: too few arguments to function call, expected at least 3, have 2\n");
    // The other arguments convert as their parameters' types say.
    CHECK(errorsOf("float l(float d) { return json_float(d, \"/a\", 0.0f); }\n"
                   "float m(const void* d, float* f) { return json_float(d, \"/a\", f); }\n"
                   "int n(const void* d, int* o) { return json_floats(d, \"/a\", o, 4); }\n"
                   "void o(const void* d) { json_free(d); }\n"
                   "int p(const void* d) { return json_floats(d, \"/a\"); }\n"
                   "void* q(const void* d) { return json_parse(d); }\n")
          == "test.fm:1:27: error: passing 'float' to parameter of incompatible type 'json_doc'\n"
             "test.fm:2:43: error: passing 'float *' to parameter of incompatible type 'float'\n"
             "test.fm:3:39: error: incompatible pointer types passing 'int *' to parameter of type 'float *'\n"
             "test.fm:4:25: error: passing 'const void *' to parameter of type 'json_doc' discards qualifiers\n"
             "test.fm:5:31: error: too few arguments to function call, expected at least 4, have 2\n"
             "test.fm:6:33: error: too few arguments to function call, expected 2, have 1\n");
}

TEST_CASE("a prototype of a json_ builtin with its own types declares nothing new", "[codegen][json]")
{
    std::string errors;
    auto jit = compile("void* json_parse(const void* data, int bytes);\n"
                       "void json_free(void* doc);\n"
                       "int count(const void* data, int bytes)\n"
                       "{\n"
                       "    void* doc = json_parse(data, bytes);\n"
                       "    const int n = json_count(doc, \"\");\n"
                       "    json_free(doc);\n"
                       "    return n;\n"
                       "}\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    CHECK(lookup<int(const void*, int)>(*jit, "count")("[1, 2, 3]", 9) == 3);
    CHECK(errorsOf("float* json_parse(const void* data, int bytes);\n"
                   "void json_free(const void* doc);\n"
                   "float json_float(const void* doc);\n"
                   "void json_free(void* doc) {}\n")
          == "test.fm:1:8: error: conflicting types for 'json_parse'\n"
             "test.fm:2:6: error: conflicting types for 'json_free'\n"
             "test.fm:3:7: error: conflicting types for 'json_float'\n"
             "test.fm:4:6: error: 'json_free' is a builtin and cannot be redefined\n");
}
#endif

TEST_CASE("a json_ builtin is a known name in every build, and an error that names the option in one without it",
          "[codegen][json]")
{
    for (const auto& [name, call] : kCalls)
    {
        INFO(name);
        std::string errors;
        const auto jit = compile(call, &errors);
        if (FMINUS_HAS_JSON != 0)
        {
            CHECK(jit);
            CHECK(errors.empty());
        }
        else
        {
            CHECK(!jit);
            CHECK(errors.find("error: '" + std::string(name) + "' needs the JSON parser, which this build of FMinus "
                              "leaves out (FMINUS_JSON=OFF)\n")
                  != std::string::npos);
        }
    }
}
