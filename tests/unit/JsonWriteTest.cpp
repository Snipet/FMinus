// tests/unit/JsonWriteTest.cpp: the JSON writer (runtime/Runtime.h, fminus_rt_json_write), from C++, before any
// literal uses it: each kind of value and how it is written (a float's shortest digits, NaN and the infinities as
// null, an enumeration's names, a string's escapes, a char array to its first 0), arrays of one and two dimensions,
// with a count fixed or given at run time and clamped, vectors, pointers and null, modules nested and in arrays; the
// text the runtime's own parser reads back; a buffer too small (the size still told, nothing written past it); every
// way a description is not well formed, which writes nothing; and the promise behind a reply's literal: it allocates
// nothing and makes no system call. In a build without FMINUS_JSON nothing here is compiled.
#include "fminus/Config.h"

#if FMINUS_HAS_JSON
#include "HeapCalls.h"
#include "Shapes.h"
#include "ir/Lowering.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
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

using namespace fminus::test::shapes;

namespace
{
    // Slots of zeros, for a description that is refused before any is read.
    constexpr std::uint64_t kZeros[8] = {};

    template <typename T>
    std::uint64_t slot(T value)
    {
        static_assert(sizeof(T) <= 8);
        std::uint64_t s = 0;
        std::memcpy(&s, &value, sizeof value);
        return s;
    }

    // The whole text a description and its values make (checked to fit a large buffer, and nothing written past it).
    std::string write(const Table& table, const void* values)
    {
        const Words layout = table.layout();
        std::string out(1 << 16, '#');
        const std::int32_t n = fminus_rt_json_write(out.data(), 1 << 15, layout.data(), values);
        REQUIRE(n >= 0);
        REQUIRE(n <= 1 << 15);
        CHECK(out.find_first_not_of('#', static_cast<std::size_t>(n)) == std::string::npos);
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

    // One value of a shape, from its slots.
    std::string writeValue(const Words& shape, const std::vector<std::uint64_t>& slots)
    {
        Table table;
        table.value(0, shape);
        return write(table, slots.data());
    }

    std::string writeFloat(float value)
    {
        return writeValue(Table::scalar(Float), {slot(value)});
    }

    std::string writeDouble(double value)
    {
        return writeValue(Table::scalar(Double), {slot(value)});
    }

    std::int32_t describe(const Words& code, std::string_view bytes = {}, const void* values = nullptr,
                          char* buffer = nullptr, std::int32_t capacity = 0)
    {
        Words table{static_cast<std::int32_t>(code.size()), static_cast<std::int32_t>(bytes.size())};
        table.insert(table.end(), code.begin(), code.end());
        const std::size_t at = table.size();
        table.resize(at + (bytes.size() + 3) / 4);
        if (!bytes.empty())
            std::memcpy(table.data() + at, bytes.data(), bytes.size());
        return fminus_rt_json_write(buffer, capacity, table.data(), values);
    }

    // A document the runtime parsed, freed at the end.
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

    // A JSON Pointer prepared as the compiler prepares one.
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

    // A module as a program declares one, and its shape.
    enum Wave : std::int32_t
    {
        Sine = 0,
        Saw = 1,
        Square = 4,
    };

    typedef float Float4 __attribute__((vector_size(16)));

    struct Osc
    {
        float detune;
        std::int32_t wave;
    };

    struct Preset
    {
        float gain;
        double tuning;
        std::int32_t voices;
        bool mono;
        char name[12];
        std::int32_t wave;
        float taps[3];
        std::int32_t grid[2][2];
        Osc osc[2];
        Float4 lfo;
    };

    Words waveShape(Table& t)
    {
        return t.enumeration(4, {{Sine, "sine"}, {Saw, "saw"}, {Square, "square"}});
    }

    Words oscShape(Table& t)
    {
        return t.module({{"detune", offsetof(Osc, detune), Table::scalar(Float)},
                         {"wave", offsetof(Osc, wave), waveShape(t)}});
    }

    Words presetShape(Table& t)
    {
        return t.module({
            {"gain", offsetof(Preset, gain), Table::scalar(Float)},
            {"tuning", offsetof(Preset, tuning), Table::scalar(Double)},
            {"voices", offsetof(Preset, voices), Table::scalar(Int)},
            {"mono", offsetof(Preset, mono), Table::scalar(Bool)},
            {"name", offsetof(Preset, name), Table::chars(12)},
            {"wave", offsetof(Preset, wave), waveShape(t)},
            {"taps", offsetof(Preset, taps), Table::array(3, 4, Table::scalar(Float))},
            {"grid", offsetof(Preset, grid), Table::array(2, 8, Table::array(2, 4, Table::scalar(Int)))},
            {"osc", offsetof(Preset, osc), Table::array(2, sizeof(Osc), oscShape(t))},
            {"lfo", offsetof(Preset, lfo), Table::vector(4)},
        });
    }

    Preset aPreset()
    {
        Preset p{};
        p.gain = 0.1f;
        p.tuning = 440.0;
        p.voices = -6;
        p.mono = true;
        std::memcpy(p.name, "warm \"pad\"", 10);
        p.wave = Saw;
        p.taps[0] = 1.0f;
        p.taps[1] = -0.5f;
        p.taps[2] = 1e-7f;
        p.grid[0][0] = 1;
        p.grid[0][1] = 2;
        p.grid[1][0] = 3;
        p.grid[1][1] = 4;
        p.osc[0] = {0.25f, Square};
        p.osc[1] = {-3.0f, 7};
        p.lfo = decltype(p.lfo){0.5f, 1.5f, 2.5f, 3.5f};
        return p;
    }

    constexpr std::string_view kPresetText =
        R"({"gain":0.1,"tuning":440,"voices":-6,"mono":true,"name":"warm \"pad\"","wave":"saw","taps":[1,-0.5,1e-07],)"
        R"("grid":[[1,2],[3,4]],"osc":[{"detune":0.25,"wave":"square"},{"detune":-3,"wave":7}],)"
        R"("lfo":[0.5,1.5,2.5,3.5]})";
} // namespace

TEST_CASE("json_write writes a float, a double, an int and a bool as JSON numbers and literals", "[runtime][json]")
{
    CHECK(writeFloat(0.1f) == "0.1");
    CHECK(writeFloat(1.0f) == "1");
    CHECK(writeFloat(-0.0f) == "-0");
    CHECK(writeFloat(1e20f) == "1e+20");
    CHECK(writeFloat(std::numeric_limits<float>::max()) == "3.4028235e+38");
    CHECK(writeFloat(std::numeric_limits<float>::denorm_min()) == "1e-45");
    CHECK(writeFloat(16777216.0f) == "16777216");
    CHECK(writeFloat(std::numeric_limits<float>::quiet_NaN()) == "null");
    CHECK(writeFloat(std::numeric_limits<float>::infinity()) == "null");
    CHECK(writeFloat(-std::numeric_limits<float>::infinity()) == "null");
    CHECK(writeDouble(0.1) == "0.1");
    CHECK(writeDouble(1.0 / 3.0) == "0.3333333333333333");
    CHECK(writeDouble(static_cast<double>(0.1f)) == "0.10000000149011612");
    CHECK(writeDouble(-std::numeric_limits<double>::infinity()) == "null");
    CHECK(writeDouble(std::numeric_limits<double>::quiet_NaN()) == "null");
    CHECK(writeDouble(5e-324) == "5e-324");
    CHECK(writeValue(Table::scalar(Int), {slot(std::int32_t{0})}) == "0");
    CHECK(writeValue(Table::scalar(Int), {slot(std::numeric_limits<std::int32_t>::min())}) == "-2147483648");
    CHECK(writeValue(Table::scalar(Int), {slot(std::numeric_limits<std::int32_t>::max())}) == "2147483647");
    CHECK(writeValue(Table::scalar(Bool), {slot(true)}) == "true");
    CHECK(writeValue(Table::scalar(Bool), {slot(false)}) == "false");
    CHECK(writeValue(Table::scalar(Bool), {slot(std::uint8_t{7})}) == "true");
}

TEST_CASE("json_write's floats are the shortest digits that the runtime's parser reads back as the same float",
          "[runtime][json]")
{
    std::mt19937 random(12345);
    const auto path = prepared("/0");
    int checked = 0;
    for (int i = 0; i < 20000; ++i)
    {
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(random()));
        if (!std::isfinite(value))
            continue;
        const std::string text = "[" + writeFloat(value) + "]";
        const Document doc(text);
        REQUIRE(doc.doc != nullptr);
        const float back = fminus_rt_json_float(doc.doc, path.data(), nullptr, 0, std::nanf(""));
        INFO(text);
        CHECK(std::bit_cast<std::uint32_t>(back) == std::bit_cast<std::uint32_t>(value));
        // The shortest: one digit fewer, rounded either way, is another float.
        char digits[64];
        const auto end = std::to_chars(digits, digits + sizeof digits, value, std::chars_format::scientific);
        const std::string_view sci(digits, static_cast<std::size_t>(end.ptr - digits));
        const std::size_t e = sci.find('e');
        const std::size_t significant = e - (sci.find('.') == std::string_view::npos ? 0 : 1) - (value < 0 ? 1 : 0);
        if (significant > 1)
        {
            char fewer[64];
            const auto f = std::to_chars(fewer, fewer + sizeof fewer, value, std::chars_format::scientific,
                                         static_cast<int>(significant) - 2);
            float near = 0;
            std::from_chars(fewer, f.ptr, near);
            CHECK(near != value);
        }
        ++checked;
    }
    CHECK(checked > 19000);
}

TEST_CASE("json_write writes an enumeration's name, or its number when no constant has it", "[runtime][json]")
{
    Table t;
    const Words wave = waveShape(t);
    t.value(0, wave);
    t.literal(",");
    t.value(1, wave);
    t.literal(",");
    t.value(2, wave);
    t.literal(",");
    t.value(3, t.enumeration(1, {{0, "off"}, {1, "on"}}));
    t.literal(",");
    t.value(4, t.enumeration(4, {{3, "first"}, {3, "second"}}));
    const std::vector<std::uint64_t> slots{slot(Square), slot(std::int32_t{2}), slot(std::int32_t{-1}), slot(true),
                                           slot(std::int32_t{3})};
    CHECK(write(t, slots.data()) == R"("square",2,-1,"on","first")");
}

TEST_CASE("json_write escapes a string as RFC 8259 requires, and writes every other byte as it is", "[runtime][json]")
{
    std::string all;
    for (int c = 0; c < 256; ++c)
        all.push_back(static_cast<char>(c));
    const std::vector<std::uint64_t> slots{slot(all.data()), slot(static_cast<std::int32_t>(all.size()))};
    const std::string text = writeValue(Table::scalar(String), slots);
    std::string expected = R"("\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f)"
                           R"(\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d)"
                           R"(\u001e\u001f !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`)"
                           "abcdefghijklmnopqrstuvwxyz{|}~\x7f";
    for (int c = 128; c < 256; ++c)
        expected.push_back(static_cast<char>(c));
    expected += "\"";
    CHECK(text == expected);

    // What the runtime's parser reads back is the string's bytes (UTF-8 here, which it checks).
    const std::string utf8 = std::string("tab\there \"quoted\" back\\slash \x01 caf\xc3\xa9 \xe2\x82\xac") +
                             std::string(1, '\0') + "end";
    const std::string doc = "[" +
                            writeValue(Table::scalar(String), {slot(utf8.data()), slot(static_cast<std::int32_t>(utf8.size()))}) +
                            "]";
    const Document parsed(doc);
    REQUIRE(parsed.doc != nullptr);
    std::int32_t length = -1;
    const char* back = fminus_rt_json_text(parsed.doc, prepared("/0").data(), nullptr, 0, &length);
    REQUIRE(back != nullptr);
    CHECK(std::string(back, static_cast<std::size_t>(length)) == utf8);

    // A null address, or a length that is not positive, is the empty string.
    CHECK(writeValue(Table::scalar(String), {slot(static_cast<const char*>(nullptr)), slot(std::int32_t{5})}) ==
          R"("")");
    CHECK(writeValue(Table::scalar(String), {slot(all.data()), slot(std::int32_t{-3})}) == R"("")");
    CHECK(writeValue(Table::scalar(String), {slot(all.data() + 97), slot(std::int32_t{0})}) == R"("")");
}

TEST_CASE("json_write writes a char array to its first 0, or all of it", "[runtime][json]")
{
    char name[8] = {'p', 'a', 'd', '\0', 'x', 'y', 'z', 'w'};
    const std::vector<std::uint64_t> at{slot(static_cast<char*>(name))};
    CHECK(writeValue(Table::pointer(Table::chars(8)), at) == R"("pad")");
    name[3] = '"';
    CHECK(writeValue(Table::pointer(Table::chars(8)), at) == R"("pad\"xyzw")");
    CHECK(writeValue(Table::pointer(Table::chars(0)), at) == R"("")");
    // In the slots themselves: 8 bytes, all of them.
    CHECK(writeValue(Table::chars(8), {slot(std::uint64_t{0x6867666564636261})}) == R"("abcdefgh")");
}

TEST_CASE("json_write writes arrays of a count fixed or given at run time, clamped, of one and two dimensions",
          "[runtime][json]")
{
    const float taps[5] = {1.5f, -2.0f, 0.1f, 3.0f, 4.0f};
    const std::int32_t grid[2][3] = {{1, 2, 3}, {4, 5, 6}};
    const double values[2] = {0.5, 1e300};
    const std::vector<std::uint64_t> taps3{slot(taps)};
    CHECK(writeValue(Table::pointer(Table::array(3, 4, Table::scalar(Float))), taps3) == "[1.5,-2,0.1]");
    CHECK(writeValue(Table::pointer(Table::array(0, 4, Table::scalar(Float))), taps3) == "[]");
    CHECK(writeValue(Table::pointer(Table::array(3, 8, Table::scalar(Float))), taps3) == "[1.5,0.1,4]");   // a stride
    CHECK(writeValue(Table::pointer(Table::array(2, 12, Table::array(3, 4, Table::scalar(Int)))), {slot(grid)}) ==
          "[[1,2,3],[4,5,6]]");
    CHECK(writeValue(Table::pointer(Table::array(2, 8, Table::scalar(Double))), {slot(values)}) == "[0.5,1e+300]");
    // A count at run time: the span's two slots, the count clamped to 0 and to the capacity.
    const Words span = Table::span(4, 4, Table::scalar(Float));
    CHECK(writeValue(span, {slot(taps), slot(std::int32_t{2})}) == "[1.5,-2]");
    CHECK(writeValue(span, {slot(taps), slot(std::int32_t{9})}) == "[1.5,-2,0.1,3]");
    CHECK(writeValue(span, {slot(taps), slot(std::int32_t{-1})}) == "[]");
    CHECK(writeValue(span, {slot(static_cast<const float*>(nullptr)), slot(std::int32_t{3})}) == "[]");
    CHECK(writeValue(Table::span(2, 12, Table::array(3, 4, Table::scalar(Int))), {slot(grid), slot(std::int32_t{1})}) ==
          "[[1,2,3]]");
    // Arrays of strings and of enumerations.
    const char* words[2] = {"a\nb", "c"};
    struct Text
    {
        const char* bytes;
        std::int32_t length;
    };
    const Text texts[2] = {{words[0], 3}, {words[1], 1}};
    CHECK(writeValue(Table::pointer(Table::array(2, sizeof(Text), Table::scalar(String))), {slot(texts)}) ==
          R"(["a\nb","c"])");
    Table t;
    const std::int32_t waves[3] = {Sine, Square, 9};
    t.value(0, Table::pointer(Table::array(3, 4, waveShape(t))));
    CHECK(write(t, std::vector<std::uint64_t>{slot(waves)}.data()) == R"(["sine","square",9])");
}

TEST_CASE("json_write writes a vector's lanes, from its slots or through a pointer, and null for a null pointer",
          "[runtime][json]")
{
    typedef float float2 __attribute__((vector_size(8)));
    typedef float float8 __attribute__((vector_size(32)));
    const float2 two = {0.5f, -1.0f};
    const float8 eight = {1, 2, 3, 4, 5, 6, 7, 8};
    std::uint64_t slots[4];
    std::memcpy(slots, &two, sizeof two);
    CHECK(writeValue(Table::vector(2), {slots[0]}) == "[0.5,-1]");
    std::memcpy(slots, &eight, sizeof eight);
    CHECK(writeValue(Table::vector(8), {slots[0], slots[1], slots[2], slots[3]}) == "[1,2,3,4,5,6,7,8]");
    CHECK(writeValue(Table::pointer(Table::vector(8)), {slot(&eight)}) == "[1,2,3,4,5,6,7,8]");
    CHECK(writeValue(Table::pointer(Table::vector(4)), {slot(static_cast<const void*>(nullptr))}) == "null");
    Table t;
    t.value(0, Table::pointer(oscShape(t)));
    CHECK(write(t, std::vector<std::uint64_t>{slot(static_cast<const void*>(nullptr))}.data()) == "null");
}

TEST_CASE("json_write writes a module as an object of its fields, nested modules and arrays of them, which the "
          "runtime's parser and json_module read back",
          "[runtime][json]")
{
    const Preset preset = aPreset();
    Table t;
    t.literal(R"({"preset":)");
    t.value(0, Table::pointer(presetShape(t)));
    t.literal(R"(,"level":)");
    t.value(1, Table::scalar(Float));
    t.literal("}");
    const std::string text = write(t, std::vector<std::uint64_t>{slot(&preset), slot(0.75f)}.data());
    CHECK(text == R"({"preset":)" + std::string(kPresetText) + R"(,"level":0.75})");

    // Read back by the module's own shape, as a table for json_module.
    const Document doc(text);
    REQUIRE(doc.doc != nullptr);
    Table shape;
    shape.code = presetShape(shape);
    const Words table = shape.layout();
    Preset back{};
    CHECK(fminus_rt_json_module(doc.doc, prepared("/preset").data(), nullptr, 0, table.data(), &back) == 1);
    CHECK(back.gain == preset.gain);
    CHECK(back.tuning == preset.tuning);
    CHECK(back.voices == preset.voices);
    CHECK(back.mono == preset.mono);
    CHECK(std::memcmp(back.name, preset.name, sizeof back.name) == 0);
    CHECK(back.wave == preset.wave);
    CHECK(std::memcmp(back.taps, preset.taps, sizeof back.taps) == 0);
    CHECK(std::memcmp(back.grid, preset.grid, sizeof back.grid) == 0);
    CHECK(back.osc[0].detune == preset.osc[0].detune);
    CHECK(back.osc[0].wave == preset.osc[0].wave);
    CHECK(back.osc[1].wave == preset.osc[1].wave);
    for (int i = 0; i < 4; ++i)
        CHECK(back.lfo[i] == preset.lfo[i]);

    // An empty module, an empty name, a key that needs escaping.
    Table e;
    e.value(0, e.module({}));
    CHECK(write(e, std::vector<std::uint64_t>{0}.data()) == "{}");
    Table k;
    k.value(0, k.module({{"", 0, Table::scalar(Int)}, {"a\"b", 0, Table::scalar(Int)}}));
    CHECK(write(k, std::vector<std::uint64_t>{slot(std::int32_t{5})}.data()) == R"({"":5,"a\"b":5})");
}

TEST_CASE("a literal's description, word by word, as a compiler emits it", "[runtime][json]")
{
    // json { "gain": gain, "taps": taps, "preset": preset }, with float gain, float taps[3] and a module
    // Osc { float detune; enum Wave wave; } (Wave { SINE, SAW }), the slots 0 to 2.
    const std::string_view bytes = R"({"gain":,"taps":,"preset":}detunewavesinesaw)";
    std::vector<std::int32_t> table = {
        47, 44,                                               // words, bytes
        0, 0, 8,                                              // {"gain":
        1, 0, 1,                                              // slot 0: a float
        0, 8, 8,                                              // ,"taps":
        1, 1, 8, 9, 3, 4, 1,                                  // slot 1: a pointer to 3 floats, 4 bytes apart
        0, 16, 10,                                            // ,"preset":
        1, 2, 8, 12, 2,                                       // slot 2: a pointer to a module of 2 fields
        6, 27, 6, 0, 0, 1,                                    //   "detune" at 0, a float
        14, 33, 4, 4, 0, 7, 4, 2, 0, 37, 4, 1, 41, 3,         //   "wave" at 4, an int enumeration: 0 sine, 1 saw
        0, 26, 1,                                             // }
    };
    REQUIRE(table.size() == 49);
    table.resize(49 + 11);
    std::memcpy(table.data() + 49, bytes.data(), bytes.size());
    const float taps[3] = {0.5f, 1, -2};
    const Osc osc = {0.1f, 1};
    const std::uint64_t slots[3] = {slot(0.75f), slot(taps), slot(&osc)};
    char out[128];
    const std::int32_t n = fminus_rt_json_write(out, sizeof out, table.data(), slots);
    REQUIRE(n > 0);
    CHECK(std::string_view(out, static_cast<std::size_t>(n)) ==
          R"({"gain":0.75,"taps":[0.5,1,-2],"preset":{"detune":0.1,"wave":"saw"}})");
}

TEST_CASE("json_write tells the whole text's size, and writes no byte past its capacity", "[runtime][json]")
{
    const Preset preset = aPreset();
    Table t;
    t.value(0, Table::pointer(presetShape(t)));
    const Words layout = t.layout();
    const std::vector<std::uint64_t> slots{slot(&preset)};
    const auto whole = static_cast<std::int32_t>(kPresetText.size());
    for (std::int32_t capacity = 0; capacity <= whole + 2; ++capacity)
    {
        std::string out(static_cast<std::size_t>(whole) + 16, '#');
        INFO("capacity " << capacity);
        CHECK(fminus_rt_json_write(out.data(), capacity, layout.data(), slots.data()) == whole);
        const auto written = static_cast<std::size_t>(std::min(capacity, whole));
        CHECK(out.substr(0, written) == kPresetText.substr(0, written));
        CHECK(out.find_first_not_of('#', written) == std::string::npos);
    }
    CHECK(fminus_rt_json_write(nullptr, 100, layout.data(), slots.data()) == whole);
    char one = '#';
    CHECK(fminus_rt_json_write(&one, -5, layout.data(), slots.data()) == whole);
    CHECK(one == '#');
    // Nothing but bytes; and no operation at all.
    Table bytes;
    bytes.literal("[1,2]");
    CHECK(write(bytes, nullptr) == "[1,2]");
    CHECK(describe({}) == 0);
}

TEST_CASE("json_write writes nothing for a description that is not well formed", "[runtime][json]")
{
    char buffer[64];
    const auto refused = [&](const Words& code, std::string_view bytes = "abc", const void* v = kZeros)
    {
        std::memset(buffer, '#', sizeof buffer);
        const std::int32_t n = describe(code, bytes, v, buffer, sizeof buffer);
        return n == -1 && std::string_view(buffer, sizeof buffer).find_first_not_of('#') == std::string_view::npos;
    };
    CHECK(fminus_rt_json_write(buffer, 64, nullptr, kZeros) == -1);
    CHECK(refused({2, 0, 1}));                         // an unknown operation
    CHECK(refused({0, 1}));                            // cut short
    CHECK(refused({0, 2, 2}));                         // bytes past the table's
    CHECK(refused({0, -1, 1}));                        // and before them
    CHECK(refused({0, 0, -1}));
    CHECK(refused({0, 0, 3, 1, 0, 99}));               // an unknown kind, after a good operation
    CHECK(refused({1, 0}));                            // no shape
    CHECK(refused({1, -1, Int}));                      // a negative slot
    CHECK(refused({1, 0, Int}, "abc", nullptr));       // no values
    CHECK(refused({1, 0, Chars}));                     // cut short
    CHECK(refused({1, 0, Chars, -1}));
    CHECK(refused({1, 0, Vector, 3}));                 // 3 lanes
    CHECK(refused({1, 0, Enum, 2, 0}));                // 2 bytes
    CHECK(refused({1, 0, Enum, 4, 1, 0, 2, 2}));       // a name past the bytes
    CHECK(refused({1, 0, Enum, 4, 2, 0, 0, 1}));       // two names, one written
    CHECK(refused({1, 0, Array, -1, 4, Int}));         // a negative count
    CHECK(refused({1, 0, Array, 1, -4, Int}));         // a negative stride
    CHECK(refused({1, 0, Span, 1, 4}));                // no element
    CHECK(refused({1, 0, Pointer}));
    CHECK(refused({1, 0, Module, 1}));                 // a field missing
    CHECK(refused({1, 0, Module, 1, 7, 0, 1, 0, 0, Int}));        // a field's size not its record's
    CHECK(refused({1, 0, Module, 1, 6, 0, 9, 0, 0, Int}));        // a name past the bytes
    CHECK(refused({1, 0, Module, 1, 6, 0, 1, -8, 0, Int}));       // a negative offset
    CHECK(refused({1, 0, Module, 1, 6, 0, 1, 0, 4, Int}));        // an unknown flag
    CHECK(refused({1, 0, Module, 1, 9, 0, 1, 0, 2, 0, 0, Chars, 2}));       // a default on chars
    CHECK(refused({1, 0, Module, 1, 10, 0, 1, 0, 1, 0, 0, 0, 0, Bool}));    // a range on a bool
    CHECK(refused({1, 0, Module, 1, 7, 0, 1, 0, 2, 0}));                    // a default's words cut short
    CHECK(!refused({1, 0, Module, 1, 8, 0, 1, 0, 2, 0, 0, Int}));           // and whole
    CHECK(refused({1, 0, Module, 1025}));              // too many fields

    // Depth: sixteen pointers to an int are the most; one more is too deep.
    Words deep{1, 0};
    for (int i = 0; i < 16; ++i)
        deep.push_back(Pointer);
    deep.push_back(Int);
    const std::int32_t number = 42;
    std::uint64_t chain[17];
    chain[16] = slot(number);
    for (int i = 15; i >= 0; --i)
        chain[i] = slot(&chain[i + 1]);
    std::memset(buffer, 0, sizeof buffer);
    CHECK(describe(deep, {}, &chain[0], buffer, sizeof buffer) == 2);
    CHECK(std::string_view(buffer, 2) == "42");
    deep.insert(deep.begin() + 2, Pointer);
    CHECK(refused(deep, {}, &chain[0]));
}

TEST_CASE("json_write allocates nothing and makes no system call", "[runtime][json]")
{
    const Preset preset = aPreset();
    const float taps[3] = {1, 2, 3};
    Table t;
    t.literal(R"({"preset":)");
    t.value(0, Table::pointer(presetShape(t)));
    t.literal(R"(,"taps":)");
    t.value(1, Table::span(8, 4, Table::scalar(Float)));
    t.literal(R"(,"gain":)");
    t.value(3, Table::scalar(Double));
    t.literal("}");
    const Words layout = t.layout();
    const std::vector<std::uint64_t> slots{slot(&preset), slot(taps), slot(std::int32_t{3}), slot(0.5)};
    const std::string expected = R"({"preset":)" + std::string(kPresetText) + R"(,"taps":[1,2,3],"gain":0.5})";
    char buffer[1024];
    const auto writeAll = [&]
    {
        std::int64_t sum = 0;
        for (std::int32_t capacity : {1024, 10, 0})
            sum += fminus_rt_json_write(buffer, capacity, layout.data(), slots.data());
        return sum;
    };
    const std::int64_t whole = writeAll();
    CHECK(whole == 3 * static_cast<std::int64_t>(expected.size()));
    CHECK(fminus_rt_json_write(buffer, sizeof buffer, layout.data(), slots.data()) ==
          static_cast<std::int32_t>(expected.size()));
    CHECK(std::string_view(buffer, expected.size()) == expected);

    if (fminus::test::HeapCalls::counted())
    {
        long calls = 0;
        bool same = true;
        {
            const fminus::test::HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                same = same && writeAll() == whole;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
    }
    else
        WARN("this build does not count the heap's calls (a sanitizer): allocation is not checked");
#if defined(__linux__)
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::close(fds[0]);
        if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
            ::syscall(SYS_exit, 2);
        std::int64_t result[2] = {writeAll(), 0};
        result[1] = std::memcmp(buffer, expected.data(), 10) == 0 ? 1 : 0;
        const ssize_t written = ::write(fds[1], result, sizeof result);
        ::syscall(SYS_exit, written == sizeof result ? 0 : 1);
    }
    ::close(fds[1]);
    int status = 0;
    ::waitpid(child, &status, 0);
    std::int64_t result[2] = {0, 0};
    const ssize_t got = ::read(fds[0], result, sizeof result);
    ::close(fds[0]);
    INFO("status " << status);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        WARN("seccomp's strict mode is not available here: the system calls are not checked");
    else
    {
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
        CHECK(got == static_cast<ssize_t>(sizeof result));
        CHECK(result[0] == whole);
        CHECK(result[1] == 1);
    }
#endif
}
#endif
