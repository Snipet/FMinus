// tests/unit/JsonModuleTest.cpp: a module filled from a JSON object in one walk (runtime/Runtime.h,
// fminus_rt_json_module) and a string read as a pointer and a length (fminus_rt_json_text), from C++, before any
// builtin calls them: each kind of field and the values that fill it and those that do not, ranges and defaults,
// arrays of one and two dimensions, vectors, nested modules and arrays of them, members unnamed and named twice, what
// the result says, a document or a path with no object there, every way a table is not well formed (which writes
// nothing); and the promise behind them: neither allocates nor makes a system call. In a build without FMINUS_JSON
// nothing here is compiled.
#include "fminus/Config.h"

#if FMINUS_HAS_JSON
#include "HeapCalls.h"
#include "Shapes.h"
#include "ir/Lowering.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

    typedef float Float4 __attribute__((vector_size(16)));

    enum Mode : std::int32_t
    {
        Off = 0,
        Mono = 1,
        Poly = 2,
    };

    struct Filter
    {
        float cutoff;
        float q;
    };

    struct Synth
    {
        float gain;              // [0, 1], 0.5 by default
        double tuning;           // required
        std::int32_t voices;     // [1, 16], 8 by default
        bool sync;               // false by default
        std::int32_t mode;       // an enumeration, required
        std::uint8_t latch;      // an enumeration of bool, true by default
        char name[8];            // required
        float taps[4];           // 0 by default, each
        std::int32_t grid[2][3]; // [0, 9], required
        Filter filter;           // its cutoff 1000 by default, its q required
        Filter bands[3];         // required
        Float4 lfo;              // required
    };

    Words filterShape(Table& t)
    {
        return t.module({{"cutoff", offsetof(Filter, cutoff), Table::scalar(Float), std::pair{20.0, 20000.0}, 1000.0},
                         {"q", offsetof(Filter, q), Table::scalar(Float)}});
    }

    Words synthShape(Table& t)
    {
        return t.module({
            {"gain", offsetof(Synth, gain), Table::scalar(Float), std::pair{0.0, 1.0}, 0.5},
            {"tuning", offsetof(Synth, tuning), Table::scalar(Double)},
            {"voices", offsetof(Synth, voices), Table::scalar(Int), std::pair{1.0, 16.0}, 8.0},
            {"sync", offsetof(Synth, sync), Table::scalar(Bool), std::nullopt, 0.0},
            {"mode", offsetof(Synth, mode), t.enumeration(4, {{Off, "off"}, {Mono, "mono"}, {Poly, "poly"}})},
            {"latch", offsetof(Synth, latch), t.enumeration(1, {{0, "open"}, {1, "held"}}), std::nullopt, 1.0},
            {"name", offsetof(Synth, name), Table::chars(8)},
            {"taps", offsetof(Synth, taps), Table::array(4, 4, Table::scalar(Float)), std::nullopt, 0.0},
            {"grid", offsetof(Synth, grid), Table::array(2, 12, Table::array(3, 4, Table::scalar(Int))),
             std::pair{0.0, 9.0}},
            {"filter", offsetof(Synth, filter), filterShape(t)},
            {"bands", offsetof(Synth, bands), Table::array(3, sizeof(Filter), filterShape(t))},
            {"lfo", offsetof(Synth, lfo), Table::vector(4)},
        });
    }

    std::vector<std::int32_t> synthTable()
    {
        Table t;
        t.code = synthShape(t);
        return t.layout();
    }

    // A module filled with a pattern no read writes, so that "left as it was" shows.
    Synth untouched()
    {
        Synth s;
        std::memset(&s, 0x5a, sizeof s);
        return s;
    }

    bool same(const void* a, const void* b, std::size_t n)
    {
        return std::memcmp(a, b, n) == 0;
    }

    std::int32_t fill(const Document& doc, std::string_view pointer, Synth& s,
                      const std::vector<std::int32_t>& table = synthTable())
    {
        return fminus_rt_json_module(doc.doc, prepared(pointer).data(), nullptr, 0, table.data(), &s);
    }

    constexpr std::string_view kSynth = R"({"synth": {
        "gain": 0.25, "tuning": 432.1, "voices": 6, "sync": true, "mode": "poly", "latch": "open", "name": "lead",
        "taps": [1, 2, 3, 4], "grid": [[1, 2, 3], [4, 5, 6]],
        "filter": {"cutoff": 2500, "q": 0.7},
        "bands": [{"cutoff": 100, "q": 1}, {"cutoff": 1000, "q": 2}, {"cutoff": 10000, "q": 3}],
        "lfo": [0.5, 1.5, 2.5, 3.5],
        "comment": "a member no field names"
    }})";
} // namespace

TEST_CASE("json_module fills every kind of field from the object at a path, in one walk", "[runtime][json]")
{
    const Document doc(kSynth);
    REQUIRE(doc.doc != nullptr);
    Synth s = untouched();
    CHECK(fill(doc, "/synth", s) == 1);
    CHECK(s.gain == 0.25f);
    CHECK(s.tuning == 432.1);
    CHECK(s.voices == 6);
    CHECK(s.sync);
    CHECK(s.mode == Poly);
    CHECK(s.latch == 0);
    CHECK(std::string_view(s.name, 8) == std::string_view("lead\0\0\0\0", 8));
    CHECK((s.taps[0] == 1 && s.taps[1] == 2 && s.taps[2] == 3 && s.taps[3] == 4));
    CHECK((s.grid[0][0] == 1 && s.grid[0][2] == 3 && s.grid[1][0] == 4 && s.grid[1][2] == 6));
    CHECK(s.filter.cutoff == 2500.0f);
    CHECK(s.filter.q == 0.7f);
    CHECK((s.bands[0].cutoff == 100.0f && s.bands[1].q == 2.0f && s.bands[2].cutoff == 10000.0f));
    CHECK((s.lfo[0] == 0.5f && s.lfo[3] == 3.5f));
    // The members in any order, and the root itself.
    const Document root(R"({"lfo": [1, 2, 3, 4], "bands": [], "grid": [], "name": "", "mode": 0, "tuning": -1e300,
                            "filter": {"q": 2, "cutoff": 20}})");
    Synth r = untouched();
    CHECK(fill(root, "", r) == 1);
    CHECK(r.tuning == -1e300);
    CHECK(r.mode == Off);
    CHECK(r.name[0] == '\0');
    CHECK(r.filter.cutoff == 20.0f);
    CHECK(r.gain == 0.5f);   // the defaults
    CHECK(r.voices == 8);
    CHECK(!r.sync);
    CHECK(r.latch == 1);
    CHECK(r.taps[3] == 0.0f);
    const Synth before = untouched();
    CHECK(same(r.grid, before.grid, sizeof r.grid));   // an empty array fills it, and no element
    CHECK(same(r.bands, before.bands, sizeof r.bands));
}

TEST_CASE("json_module fills a field only from a value of its kind, within its range, else its default", "[runtime][json]")
{
    const auto read = [](std::string_view members, Synth& s)
    {
        const std::string text = std::string(R"({"tuning": 1, "mode": 1, "name": "x", "grid": [], "filter": {"q": 1},
                                                 "bands": [], "lfo": []})")
                                     .insert(1, std::string(members) + (members.empty() ? "" : ","));
        const Document doc(text);
        REQUIRE(doc.doc != nullptr);
        return fill(doc, "", s);
    };
    Synth s = untouched();
    // Numbers: a float as json_float reads it, an int only from an integer an int holds, each within its range.
    CHECK(read(R"("gain": 1.5, "voices": 7.0)", s) == 1);
    CHECK(s.gain == 0.5f);   // out of range: its default
    CHECK(s.voices == 7);
    CHECK(read(R"("gain": "0.3", "voices": 1.5)", s) == 1);
    CHECK((s.gain == 0.5f && s.voices == 8));
    CHECK(read(R"("gain": 1e-50, "voices": 17)", s) == 1);
    CHECK((s.gain == 0.0f && s.voices == 8));   // 1e-50 is a float's 0, in range
    CHECK(read(R"("gain": 1e39, "voices": 3e9)", s) == 1);
    CHECK((s.gain == 0.5f && s.voices == 8));
    CHECK(read(R"("gain": -0, "voices": 16)", s) == 1);
    CHECK((s.gain == 0.0f && s.voices == 16));
    // A bool from true or false only.
    CHECK(read(R"("sync": 1)", s) == 1);
    CHECK(!s.sync);
    CHECK(read(R"("sync": true)", s) == 1);
    CHECK(s.sync);
    // An enumeration from a name, byte for byte, or an integer (0 or 1 for one of bool).
    s = untouched();
    CHECK(read(R"("latch": "held")", s) == 1);
    CHECK(s.latch == 1);
    CHECK(read(R"("latch": 0)", s) == 1);
    CHECK(s.latch == 0);
    CHECK(read(R"("latch": 2)", s) == 1);
    CHECK(s.latch == 1);   // its default
    CHECK(read(R"("latch": "Held")", s) == 1);
    CHECK(s.latch == 1);
    const auto mode = [&](std::string_view value)
    {
        Synth m = untouched();
        const std::string text = R"({"tuning": 1, "mode": )" + std::string(value) +
                                 R"(, "name": "x", "grid": [], "filter": {"q": 1}, "bands": [], "lfo": []})";
        const Document doc(text);
        REQUIRE(doc.doc != nullptr);
        return fill(doc, "", m) == 1 ? m.mode : -99;
    };
    CHECK(mode(R"("mono")") == Mono);
    CHECK(mode("2") == Poly);
    CHECK(mode("-7") == -7);   // an integer no constant has: C's enumerations hold it
    CHECK(mode("2.0") == Poly);
    CHECK(mode(R"("POLY")") == -99);
    CHECK(mode("2.5") == -99);
    CHECK(mode("true") == -99);
    // A char array from a string with room for its 0, the rest zeroed.
    const auto name = [&](std::string_view value)
    {
        Synth m = untouched();
        const std::string text = R"({"tuning": 1, "mode": 1, "name": )" + std::string(value) +
                                 R"(, "grid": [], "filter": {"q": 1}, "bands": [], "lfo": []})";
        const Document doc(text);
        REQUIRE(doc.doc != nullptr);
        const std::int32_t filled = fill(doc, "", m);
        return filled == 1 ? std::string(m.name, 8) : std::string("unfilled");
    };
    CHECK(name(R"("1234567")") == std::string("1234567\0", 8));
    CHECK(name(R"("12345678")") == "unfilled");
    CHECK(name(R"("a\u0000b")") == std::string("a\0b\0\0\0\0\0", 8));
    CHECK(name("7") == "unfilled");
}

TEST_CASE("json_module fills an array from an array: as many as fit, each by its kind and range", "[runtime][json]")
{
    const auto read = [](std::string_view members, Synth& s)
    {
        const std::string text = R"({"tuning": 1, "mode": 1, "name": "x", "filter": {"q": 1}, )" + std::string(members) +
                                 "}";
        const Document doc(text);
        REQUIRE(doc.doc != nullptr);
        return fill(doc, "", s);
    };
    Synth s = untouched();
    const Synth before = untouched();
    CHECK(read(R"("taps": [1, "x", 3, 4, 5, 6], "grid": [[1, 2, 3, 4], [5, 10, true]], "bands": [], "lfo": [9])", s) ==
          1);
    CHECK(s.taps[0] == 1);
    CHECK(same(&s.taps[1], &before.taps[1], 4));   // the wrong kind: left as it was
    CHECK((s.taps[2] == 3 && s.taps[3] == 4));
    CHECK((s.grid[0][0] == 1 && s.grid[0][1] == 2 && s.grid[0][2] == 3 && s.grid[1][0] == 5));
    CHECK(same(&s.grid[1][1], &before.grid[1][1], 8));   // 10 out of range, true no int
    CHECK(s.lfo[0] == 9);
    CHECK(same(reinterpret_cast<const char*>(&s.lfo) + 4, reinterpret_cast<const char*>(&before.lfo) + 4, 12));
    // A row that is no array is left; and a module of an array's that is no object.
    s = untouched();
    CHECK(read(R"("taps": [], "grid": [7, [0, 0, 0]], "bands": [1, {"q": 5}, {"cutoff": 5, "q": 6}], "lfo": [])", s) ==
          1);
    CHECK(same(s.grid[0], before.grid[0], sizeof s.grid[0]));
    CHECK(s.grid[1][2] == 0);
    CHECK(same(&s.bands[0], &before.bands[0], sizeof(Filter)));
    CHECK((s.bands[1].cutoff == 1000.0f && s.bands[1].q == 5.0f));   // its cutoff's default
    CHECK((s.bands[2].cutoff == 1000.0f && s.bands[2].q == 6.0f));   // 5 is out of its range
    CHECK(same(s.taps, before.taps, sizeof s.taps));   // an empty array: no element; filled, so no default
    // A module of an array's that the document has, missing a field with no default, makes the result 0.
    CHECK(read(R"("taps": [], "grid": [], "bands": [{}], "lfo": [])", s) == 0);
    // An array that is no array: its default, each element; or no default, and the result 0.
    s = untouched();
    CHECK(read(R"("taps": {"0": 1}, "grid": [], "bands": [], "lfo": [])", s) == 1);
    CHECK((s.taps[0] == 0 && s.taps[3] == 0));
    CHECK(read(R"("taps": [], "grid": 5, "bands": [], "lfo": [])", s) == 0);
    CHECK(read(R"("taps": [], "grid": [], "bands": {}, "lfo": [])", s) == 0);
    CHECK(read(R"("taps": [], "grid": [], "bands": [], "lfo": 1)", s) == 0);
}

TEST_CASE("json_module: a field with no default not filled makes it 0; a missing module takes its fields' defaults",
          "[runtime][json]")
{
    const Document doc(kSynth);
    REQUIRE(doc.doc != nullptr);
    // A member missing, named twice (the first is read, of whatever kind), or of no field.
    const Document missing(R"({"gain": 0.75, "gain": 0.5, "sync": 2, "sync": true, "Gain": 0})");
    Synth s = untouched();
    const Synth before = untouched();
    CHECK(fill(missing, "", s) == 0);
    CHECK(s.gain == 0.75f);
    CHECK(!s.sync);   // its first member is no bool: the default
    CHECK(s.voices == 8);
    CHECK(s.latch == 1);
    CHECK(s.filter.cutoff == 1000.0f);   // a module not there: its own fields' defaults
    CHECK(same(&s.filter.q, &before.filter.q, 4));
    CHECK(same(&s.tuning, &before.tuning, 8));   // no default: left as it was
    CHECK(same(s.name, before.name, 8));
    CHECK(same(s.bands, before.bands, sizeof s.bands));
    // A null document, a path where nothing is, and a value there that is no object: nothing filled, defaults apply.
    for (const char* pointer : {"/none", "/synth/gain", "/synth/taps", "/synth/name"})
    {
        INFO(pointer);
        s = untouched();
        CHECK(fill(doc, pointer, s) == 0);
        CHECK((s.gain == 0.5f && s.voices == 8 && s.filter.cutoff == 1000.0f && s.taps[0] == 0.0f));
        CHECK(same(&s.tuning, &before.tuning, 8));
    }
    s = untouched();
    CHECK(fminus_rt_json_module(nullptr, prepared("").data(), nullptr, 0, synthTable().data(), &s) == 0);
    CHECK(s.gain == 0.5f);
    // A module whose every field has a default is filled by its defaults alone: 1.
    Table t;
    t.code = t.module({{"a", 0, Table::scalar(Float), std::nullopt, 2.5}, {"b", 4, Table::scalar(Int), std::nullopt, -3.0},
                       {"c", 8, t.module({{"d", 0, Table::scalar(Double), std::nullopt, 1e300}})}});
    const std::vector<std::int32_t> table = t.layout();
    struct
    {
        float a;
        std::int32_t b;
        double d;
    } small{};
    CHECK(fminus_rt_json_module(nullptr, prepared("").data(), nullptr, 0, table.data(), &small) == 1);
    CHECK((small.a == 2.5f && small.b == -3 && small.d == 1e300));
    // An empty module: always 1, nothing written.
    Table e;
    e.code = e.module({});
    CHECK(fminus_rt_json_module(doc.doc, prepared("/synth").data(), nullptr, 0, e.layout().data(), &small) == 1);
    // The path's places are the reads'.
    const Document bands(R"([{"cutoff": 30, "q": 0.5}, {"q": 9}])");
    Filter f{};
    Table ft;
    ft.code = filterShape(ft);
    const std::vector<std::int32_t> filterTable = ft.layout();
    const std::int32_t one[] = {1};
    CHECK(fminus_rt_json_module(bands.doc, prepared("/%d").data(), one, 1, filterTable.data(), &f) == 1);
    CHECK((f.cutoff == 1000.0f && f.q == 9.0f));
}

TEST_CASE("a module's table, word by word, as a compiler emits it", "[runtime][json]")
{
    // module Filter { float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]]; float q; };
    std::vector<std::int32_t> table = {
        20, 7,                                                // words, bytes ("cutoffq")
        12, 2,                                                // a module of 2 fields
        12, 0, 6, 0, 3, 0, 1077149696, 0, 1087604736, 0, 1083129856, 1,   // "cutoff" at 0: range, default, a float
        6, 6, 1, 4, 0, 1,                                     // "q" at 4: a float
    };
    REQUIRE(table.size() == 22);
    table.resize(22 + 2);
    std::memcpy(table.data() + 22, "cutoffq", 7);
    const Document doc(R"({"q": 0.5, "cutoff": 30000})");
    Filter f{};
    CHECK(fminus_rt_json_module(doc.doc, prepared("").data(), nullptr, 0, table.data(), &f) == 1);
    CHECK(f.cutoff == 1000.0f);   // out of its range: its default
    CHECK(f.q == 0.5f);
}

TEST_CASE("json_module writes nothing by a table that is not well formed", "[runtime][json]")
{
    const Document doc(R"({"a": 1, "b": "x"})");
    const auto refused = [&](const Words& code, std::string_view bytes = "ab")
    {
        Words table{static_cast<std::int32_t>(code.size()), static_cast<std::int32_t>(bytes.size())};
        table.insert(table.end(), code.begin(), code.end());
        const std::size_t at = table.size();
        table.resize(at + (bytes.size() + 3) / 4);
        std::memcpy(table.data() + at, bytes.data(), bytes.size());
        unsigned char module[64];
        std::memset(module, 0x5a, sizeof module);
        const std::int32_t result = fminus_rt_json_module(doc.doc, prepared("").data(), nullptr, 0, table.data(), module);
        bool untouchedModule = true;
        for (const unsigned char byte : module)
            untouchedModule = untouchedModule && byte == 0x5a;
        return result == 0 && untouchedModule;
    };
    // The field "a" (bytes 0, 1) at offset 0, with its default (so that it is written whenever the table is read).
    const auto field = [](const Words& shape)
    {
        Words record{0, 0, 1, 0, 2, 0, 0};
        record.insert(record.end(), shape.begin(), shape.end());
        record[0] = static_cast<std::int32_t>(record.size());
        return Table::join({Module, 1}, record);
    };
    CHECK(!refused(field({Int})));   // well formed: it writes
    CHECK(refused({}));
    CHECK(refused({Int}));                      // not a module
    CHECK(refused(field({String})));            // what the reader does not fill
    CHECK(refused(field({Pointer, Int})));
    CHECK(refused(field({Span, 1, 4, Int})));
    CHECK(refused(Table::join({Module, 1}, Words{7, 0, 1, 0, 0, Chars, 0})));   // a char array of 0 bytes
    CHECK(!refused(Table::join({Module, 1}, Words{7, 1, 1, 0, 0, Chars, 2})));  // "b", of 2: "x" fills it
    CHECK(refused(field({Array, -1, 4, Int})));
    CHECK(refused(field({99})));
    CHECK(refused({Module, 1, 7, 0, 1, 0, 2, 0, 0}));   // no shape
    CHECK(refused({Module, 1, 6, 0, 1, 0, 0, Int, 0}));   // a word after the module
    CHECK(refused({Module, 2, 6, 0, 1, 0, 0, Int}));      // a field missing
    CHECK(refused({Module, 1, 6, 0, 1, 0, 0, Int}, ""));  // its name past the bytes
    CHECK(refused({Module, 1, 9, 0, 1, 0, 2, 0, 0, Module, 0}));   // a default on a module
    CHECK(refused(Table::join({Module, 1}, Words{10, 0, 1, 0, 1, 0, 0, 0, 0, Bool})));   // a range on a bool
    // Too deep: a module in a module, seventeen deep.
    Words deep{Int};
    for (int i = 0; i < 17; ++i)
    {
        Words record{0, 0, 1, 0, 0};
        record.insert(record.end(), deep.begin(), deep.end());
        record[0] = static_cast<std::int32_t>(record.size());
        deep = Table::join({Module, 1}, record);
    }
    CHECK(refused(deep));
    CHECK(fminus_rt_json_module(doc.doc, prepared("").data(), nullptr, 0, synthTable().data(), nullptr) == 0);
    CHECK(fminus_rt_json_module(doc.doc, prepared("").data(), nullptr, 0, nullptr, &deep) == 0);
}

TEST_CASE("json_text gives a string's bytes as the parser holds them, and null for anything else", "[runtime][json]")
{
    const Document doc(R"({"name": "warm pad", "empty": "", "escaped": "a\"b\\c\né\u0000d", "n": 1, "t": true,
                           "z": null, "o": {}, "a": ["x", "y"]})");
    REQUIRE(doc.doc != nullptr);
    const auto text = [&](std::string_view pointer, const std::vector<std::int32_t>& ints = {})
    {
        std::int32_t length = -1;
        const char* bytes = fminus_rt_json_text(doc.doc, prepared(pointer).data(), ints.data(),
                                                static_cast<std::int32_t>(ints.size()), &length);
        if (bytes == nullptr)
            return length == 0 ? std::string("(null)") : std::string("(null, length ") + std::to_string(length) + ")";
        return std::string(bytes, static_cast<std::size_t>(length));
    };
    CHECK(text("/name") == "warm pad");
    CHECK(text("/empty").empty());
    CHECK(text("/escaped") == std::string("a\"b\\c\n\xc3\xa9\0d", 10));
    CHECK(text("/a/%d", {1}) == "y");
    for (const char* pointer : {"/n", "/t", "/z", "/o", "/a", "/none", "/a/2", ""})
    {
        INFO(pointer);
        CHECK(text(pointer) == "(null)");
    }
    std::int32_t length = -1;
    CHECK(fminus_rt_json_text(nullptr, prepared("/name").data(), nullptr, 0, &length) == nullptr);
    CHECK(length == 0);
    const char* name = fminus_rt_json_text(doc.doc, prepared("/name").data(), nullptr, 0, nullptr);
    REQUIRE(name != nullptr);
    CHECK(name[8] == '\0');
}

TEST_CASE("json_module and json_text allocate nothing and make no system call", "[runtime][json]")
{
    const Document doc(kSynth);
    REQUIRE(doc.doc != nullptr);
    const std::vector<std::int32_t> table = synthTable();
    const std::vector<std::int32_t> synth = prepared("/synth");
    const std::vector<std::int32_t> none = prepared("/none");
    const std::vector<std::int32_t> name = prepared("/synth/name");
    const auto readAll = [&]
    {
        Synth s = untouched();
        double sum = fminus_rt_json_module(doc.doc, synth.data(), nullptr, 0, table.data(), &s);
        sum += static_cast<double>(s.gain) + s.tuning + s.voices + s.mode + s.grid[1][2] +
               static_cast<double>(s.bands[2].cutoff + s.lfo[3]);
        sum += 10 * fminus_rt_json_module(doc.doc, none.data(), nullptr, 0, table.data(), &s);
        sum += static_cast<double>(s.filter.cutoff);
        std::int32_t length = 0;
        const char* text = fminus_rt_json_text(doc.doc, name.data(), nullptr, 0, &length);
        sum += length + text[0];
        return sum;
    };
    const double expected = readAll();
    CHECK(std::abs(expected - (1 + 0.25 + 432.1 + 6 + 2 + 6 + 10000 + 3.5 + 0 + 1000 + 4 + 'l')) < 1e-9);
    if (fminus::test::HeapCalls::counted())
    {
        long calls = 0;
        bool sameSum = true;
        {
            const fminus::test::HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                sameSum = sameSum && readAll() == expected;
            calls = counting.count();
        }
        CHECK(sameSum);
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
}
#endif
