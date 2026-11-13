// tests/unit/JsonReadTest.cpp: json_read(doc, "pointer", ..., &module), a module filled from the JSON object at a path
// by its type's table (ir/Shapes.cpp, runtime's fminus_rt_json_module), and the json:: attributes (extending.md, recipe
// 50). Through the JIT: each kind of field read, nested modules and arrays of them, names and json::skip, a string, a
// pointer, a handle and a parameter left alone, a field missing with and without its default, out of its range, of
// another kind, a member no field names, a null document, a path with nothing or no object there, a default that does
// not initialise; json_read through `this`, its writes seen after it; the table's words against the handoff's worked
// example, one table a type however many calls; the errors, each once at its column; a nonblocking function that calls
// it, allocating nothing and making no system call. In a build without FMINUS_JSON, json_read is the error that names
// the option.
#include "Compile.h"
#include "HeapCalls.h"
#include "StrictMode.h"
#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if FMINUS_HAS_JSON
#include "runtime/Runtime.h"
#endif

using namespace fminus::test;

namespace
{
    // A call a program could make, compiled in every build.
    constexpr const char* kCall = "module M { float a; };\n"
                                  "bool r(json_doc d, M* m) { return json_read(d, \"\", m); }\n";

#if FMINUS_HAS_JSON
    // The program compiled at -O0 into a module, kept with its context: its constants, as the JIT would get them.
    struct Lowered
    {
        std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module;
        std::string errors;

        explicit Lowered(std::string_view source)
        {
            fminus::Diagnostics diagnostics("test.fm");
            const auto program = fminus::frontend::parse(source, diagnostics);
            if (program)
                module = fminus::ir::lower(*program, *context, diagnostics, {});
            errors = diagnostics.format();
        }
    };

    // A table constant's words and bytes: { i32 words, i32 length, [words x i32], [length x i8] }.
    struct Words
    {
        std::vector<std::int32_t> words;
        std::string bytes;
    };

    Words wordsOf(const llvm::GlobalVariable& table)
    {
        Words out;
        const auto* value = llvm::cast<llvm::ConstantStruct>(table.getInitializer());
        for (unsigned i : {0u, 1u})
            out.words.push_back(
                static_cast<std::int32_t>(llvm::cast<llvm::ConstantInt>(value->getOperand(i))->getSExtValue()));
        const auto* code = llvm::cast<llvm::ConstantDataArray>(value->getOperand(2));
        for (unsigned i = 0; i < code->getNumElements(); ++i)
            out.words.push_back(static_cast<std::int32_t>(code->getElementAsInteger(i)));
        out.bytes = llvm::cast<llvm::ConstantDataArray>(value->getOperand(3))->getAsString().str();
        return out;
    }

    std::vector<std::string> shapeTables(const llvm::Module& module)
    {
        std::vector<std::string> names;
        for (const llvm::GlobalVariable& global : module.globals())
        {
            if (global.getName().starts_with("json.shape."))
                names.push_back(global.getName().str());
        }
        return names;
    }
#endif
} // namespace

#if FMINUS_HAS_JSON
namespace
{
    // Every kind of field json_read fills, and the functions that read a document's text into one.
    constexpr const char* kSynth =
        "enum Mode { OFF, MONO, POLY };\n"
        "enum Latch : bool { OPEN, HELD };\n"
        "module Filter { float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]]; float q; };\n"
        "module Synth\n"
        "{\n"
        "    float gain;\n"
        "    double tuning;\n"
        "    int voices;\n"
        "    bool sync;\n"
        "    enum Mode mode;\n"
        "    enum Latch latch;\n"
        "    char name[8];\n"
        "    float taps[4];\n"
        "    int grid[2][3];\n"
        "    Filter filter;\n"
        "    Filter bands[3];\n"
        "    float4 lfo;\n"
        "    double fine[2];\n"
        "};\n"
        "bool read(const void* data, int bytes, Synth* s)\n"
        "{\n"
        "    json_doc doc = json_parse(data, bytes);\n"
        "    defer json_free(doc);\n"
        "    return json_read(doc, \"\", s);\n"
        "}\n"
        "bool readAt(const void* data, int bytes, int k, Synth* s)\n"
        "{\n"
        "    json_doc doc = json_parse(data, bytes);\n"
        "    defer json_free(doc);\n"
        "    return json_read(doc, \"/synths/%d\", k, s);\n"
        "}\n"
        "bool fill(json_doc doc, Synth* s) [[clang::nonblocking]] { return json_read(doc, \"\", s); }\n"
        "int size() { return sizeof(Synth); }\n";

    struct Filter
    {
        float cutoff;
        float q;
    };

    // Synth as FMinus lays it out, a C struct's layout.
    struct Synth
    {
        float gain;
        double tuning;
        std::int32_t voices;
        bool sync;
        std::int32_t mode;
        bool latch;   // an enumeration of bool: a byte
        char name[8];
        float taps[4];
        std::int32_t grid[2][3];
        Filter filter;
        Filter bands[3];
        float4 lfo;
        double fine[2];
    };

    Synth patterned()
    {
        Synth s;
        std::memset(&s, 0x5a, sizeof s);
        return s;
    }

    struct SynthReads
    {
        std::unique_ptr<fminus::jit::Jit> jit;
        bool (*read)(const void*, int, Synth*) = nullptr;
        bool (*readAt)(const void*, int, int, Synth*) = nullptr;
        bool (*fill)(void*, Synth*) = nullptr;

        SynthReads()
        {
            std::string errors;
            jit = compile(kSynth, &errors);
            INFO(errors);
            REQUIRE(jit);
            REQUIRE(lookup<int()>(*jit, "size")() == static_cast<int>(sizeof(Synth)));
            read = lookup<bool(const void*, int, Synth*)>(*jit, "read");
            readAt = lookup<bool(const void*, int, int, Synth*)>(*jit, "readAt");
            fill = lookup<bool(void*, Synth*)>(*jit, "fill");
        }

        bool operator()(std::string_view text, Synth& s) const
        {
            return read(text.data(), static_cast<int>(text.size()), &s);
        }
    };

    constexpr std::string_view kFull = R"({
        "gain": 0.5, "tuning": 440.25, "voices": 7, "sync": true, "mode": "POLY", "latch": "HELD", "name": "warm",
        "taps": [1, 2, 3, 4, 5], "grid": [[1, 2, 3], [4, 5, 6]], "filter": {"cutoff": 2000, "q": 0.7},
        "bands": [{"cutoff": 100, "q": 1}, {"q": 2, "cutoff": 200}, {"cutoff": 300, "q": 3}],
        "lfo": [0.5, 1.5, 2.5, 3.5], "fine": [0.125, -0.25]
    })";
} // namespace

TEST_CASE("json_read fills each kind of field of a module, its modules and arrays of them, by name",
          "[codegen][json-read]")
{
    const SynthReads read;
    Synth s = patterned();
    CHECK(read(kFull, s));
    CHECK(s.gain == 0.5f);
    CHECK(s.tuning == 440.25);
    CHECK(s.voices == 7);
    CHECK(s.sync);
    CHECK(s.mode == 2);
    CHECK(s.latch);
    CHECK(std::string(s.name) == "warm");
    CHECK(std::memcmp(s.name, "warm\0\0\0\0", 8) == 0);   // the rest zeroed
    CHECK((s.taps[0] == 1 && s.taps[1] == 2 && s.taps[2] == 3 && s.taps[3] == 4));   // more is no error
    CHECK((s.grid[0][0] == 1 && s.grid[0][2] == 3 && s.grid[1][0] == 4 && s.grid[1][2] == 6));
    CHECK((s.filter.cutoff == 2000 && s.filter.q == 0.7f));
    CHECK((s.bands[0].cutoff == 100 && s.bands[1].cutoff == 200 && s.bands[1].q == 2 && s.bands[2].q == 3));
    CHECK(lanesOf(s.lfo) == std::array<float, 4>{0.5f, 1.5f, 2.5f, 3.5f});
    CHECK((s.fine[0] == 0.125 && s.fine[1] == -0.25));

    // An enumeration from an integer too; its names byte for byte, as written.
    Synth e = s;
    CHECK(!read(R"({"mode": 1, "latch": 0})", e));   // the rest missing
    CHECK(e.mode == 1);
    CHECK(!e.latch);
    CHECK(!read(R"({"mode": "poly", "latch": 2})", e));
    CHECK(e.mode == 1);   // "poly" is no constant's name, and an enumeration of bool takes 0 or 1
    CHECK(!e.latch);

    // At a path, with its ints: the second synth.
    const std::string text = std::string(R"({"synths": [{}, )") + std::string(kFull) + "]}";
    Synth at = patterned();
    CHECK(read.readAt(text.data(), static_cast<int>(text.size()), 1, &at));
    CHECK((at.voices == 7 && at.filter.q == 0.7f && at.bands[2].cutoff == 300));
}

TEST_CASE("json_read: a field missing, of another kind or out of its range is its default, or unfilled",
          "[codegen][json-read]")
{
    const auto jit = compile("enum Mode { OFF, MONO, POLY };\n"
                             "module Ranged\n"
                             "{\n"
                             "    float gain [[fminus::range(0, 1)]];\n"
                             "    float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                             "    int voices [[fminus::range(1, 16)]] [[fminus::default(8)]];\n"
                             "    bool sync [[fminus::default(1)]];\n"
                             "    enum Mode mode [[fminus::default(2)]];\n"
                             "    float taps [[fminus::default(0.5)]] [3];\n"
                             "};\n"
                             "module Defaults { float a [[fminus::default(1)]]; int b [[fminus::default(-2)]]; };\n"
                             "bool read(const void* data, int bytes, int k, Ranged* r)\n"
                             "{\n"
                             "    json_doc doc = json_parse(data, bytes);\n"
                             "    defer json_free(doc);\n"
                             "    return json_read(doc, \"/items/%d\", k, r);\n"
                             "}\n"
                             "bool whole(const void* data, int bytes, Defaults* d)\n"
                             "{\n"
                             "    json_doc doc = json_parse(data, bytes);\n"
                             "    defer json_free(doc);\n"
                             "    return json_read(doc, \"\", d);\n"
                             "}\n"
                             "float fresh() { Ranged r; return r.cutoff + (float)r.voices + r.taps[2]; }\n"
                             "int size() { return sizeof(Ranged); }\n");
    REQUIRE(jit);
    struct Ranged
    {
        float gain;
        float cutoff;
        std::int32_t voices;
        bool sync;
        std::int32_t mode;
        float taps[3];
    };
    REQUIRE(lookup<int()>(*jit, "size")() == static_cast<int>(sizeof(Ranged)));
    auto* read = lookup<bool(const void*, int, int, Ranged*)>(*jit, "read");
    constexpr std::string_view kItems = R"({"items": [
        {"gain": 0.5},
        {"gain": 2, "cutoff": 500, "voices": 3, "sync": false, "mode": "MONO", "taps": [1, "x"]},
        {"gain": "loud", "cutoff": 30000, "voices": 3.5, "sync": 1, "mode": "NOPE", "taps": 7, "other": [1, 2]},
        {"gain": 0.25, "other": {"gain": 0.75}, "gain": 0.75},
        5
    ]})";
    const auto at = [&](int k, Ranged& r) { return read(kItems.data(), static_cast<int>(kItems.size()), k, &r); };
    const auto defaulted = [](const Ranged& r) {
        return r.cutoff == 1000 && r.voices == 8 && r.sync && r.mode == 2 && r.taps[0] == 0.5f && r.taps[2] == 0.5f;
    };
    Ranged r{};
    // Only gain has no default: the rest take theirs, each element of an array its own.
    r.gain = -1;
    CHECK(at(0, r));
    CHECK(r.gain == 0.5f);
    CHECK(defaulted(r));
    // Out of its range: left as it was, and counted missing. Inside theirs, the others are filled; an array's elements
    // each by its kind, the one of another kind and those the document lacks left as they were.
    r.gain = -1;
    r.taps[1] = 9;
    r.taps[2] = 9;
    CHECK(!at(1, r));
    CHECK(r.gain == -1);
    CHECK((r.cutoff == 500 && r.voices == 3 && !r.sync && r.mode == 1));
    CHECK((r.taps[0] == 1 && r.taps[1] == 9 && r.taps[2] == 9));
    // Of another kind, or out of range: each takes its default (an int from 3.5 is none, a bool from 1 is none, an
    // array from a number is none); a member no field names is ignored.
    r = Ranged{};
    r.gain = -1;
    CHECK(!at(2, r));
    CHECK(r.gain == -1);
    CHECK(defaulted(r));
    // Of two members of one name, the first; a nested "gain" is no field's.
    CHECK(at(3, r));
    CHECK(r.gain == 0.25f);
    // No object there, nothing there, no document: nothing filled, every default set, and false (gain has none).
    for (const int k : {4, 5, -1})
    {
        INFO(k);
        r = Ranged{};
        r.gain = -1;
        CHECK(!at(k, r));
        CHECK(r.gain == -1);
        CHECK(defaulted(r));
    }
    r = Ranged{};
    CHECK(!read(nullptr, 0, 0, &r));   // json_parse gives no document: a null one is an empty one
    CHECK(defaulted(r));
    CHECK(!read("{]", 2, 0, &r));
    // A module whose every field has a default is read whole from nothing.
    struct Defaults
    {
        float a;
        std::int32_t b;
    };
    auto* whole = lookup<bool(const void*, int, Defaults*)>(*jit, "whole");
    Defaults d{};
    CHECK(whole(nullptr, 0, &d));
    CHECK((d.a == 1 && d.b == -2));
    CHECK(whole("[1, 2]", 6, &d));   // no object: the defaults
    // A default does not initialise: a module's fields start at 0, as C's do.
    CHECK(lookup<float()>(*jit, "fresh")() == 0.0f);
}

TEST_CASE("json_read: names, json::skip, and a string, a pointer, a handle and a parameter left as they were",
          "[codegen][json-read]")
{
    const auto jit = compile("module Named\n"
                             "{\n"
                             "    float level [[json::name(\"Level dB\")]] [[ui::unit(\"dB\")]];\n"
                             "    float gain [[json::skip]];\n"
                             "    string note;\n"
                             "    float* p;\n"
                             "    json_doc d;\n"
                             "    param float knob;\n"
                             "    float said [[json::name(\"say \\\"hi\\\"\")]];\n"
                             "    float micro [[json::name(\"\\u00B5s\")]];\n"
                             "    float own;\n"
                             "    float last [[json::name(\"first\")]] [[json::name(\"second\")]];\n"
                             "};\n"
                             "bool read(const void* data, int bytes, Named* n)\n"
                             "{\n"
                             "    json_doc doc = json_parse(data, bytes);\n"
                             "    defer json_free(doc);\n"
                             "    return json_read(doc, \"\", n);\n"
                             "}\n"
                             "int size() { return sizeof(Named); }\n");
    REQUIRE(jit);
    struct Named
    {
        float level;
        float gain;
        struct
        {
            const char* data;
            std::int32_t size;
        } note;
        float* p;
        void* d;
        struct
        {
            const float* stream;
            float value;
        } knob;
        float said;
        float micro;
        float own;
        float last;
    };
    REQUIRE(lookup<int()>(*jit, "size")() == static_cast<int>(sizeof(Named)));
    auto* read = lookup<bool(const void*, int, Named*)>(*jit, "read");
    constexpr std::string_view kText = R"({"level": 99, "Level dB": 3, "gain": 4, "note": "x", "p": 1, "d": 2,
        "knob": 5, "say \"hi\"": 6, "µs": 7, "own": 8, "first": 9, "second": 10})";
    Named n;
    std::memset(&n, 0x5a, sizeof n);
    Named was = n;
    CHECK(read(kText.data(), static_cast<int>(kText.size()), &n));
    CHECK(n.level == 3);   // under its JSON name, not its own
    CHECK((n.said == 6 && n.micro == 7 && n.own == 8));
    CHECK(n.last == 10);   // the last json::name counts
    // Left as they were: a skipped field, a string (it would point into the document), a pointer, a handle, and a
    // parameter (the host's).
    CHECK(std::memcmp(&n.gain, &was.gain, sizeof n.gain) == 0);
    CHECK(std::memcmp(&n.note, &was.note, sizeof n.note) == 0);
    CHECK(std::memcmp(&n.p, &was.p, sizeof n.p) == 0);
    CHECK(std::memcmp(&n.d, &was.d, sizeof n.d) == 0);
    CHECK(std::memcmp(&n.knob, &was.knob, sizeof n.knob) == 0);
}

TEST_CASE("json_read through `this`, and what a method reads after it", "[codegen][json-read]")
{
    const auto jit = compile("module Voice\n"
                             "{\n"
                             "    float gain;\n"
                             "    int note;\n"
                             "    float load(json_doc doc) [[clang::nonblocking]]\n"
                             "    {\n"
                             "        gain = 5.0f;\n"
                             "        note = 1;\n"
                             "        bool all = json_read(doc, \"/voice\", this);\n"
                             "        return gain + (float)note + (all ? 1000.0f : 0.0f);\n"
                             "    }\n"
                             "};\n"
                             "void* parse(const void* data, int bytes) { return json_parse(data, bytes); }\n"
                             "void release(json_doc doc) { json_free(doc); }\n"
                             "float load(Voice* v, json_doc doc) { return v->load(doc); }\n");
    REQUIRE(jit);
    struct Voice
    {
        float gain;
        std::int32_t note;
    };
    auto* parse = lookup<void*(const void*, int)>(*jit, "parse");
    auto* release = lookup<void(void*)>(*jit, "release");
    auto* load = lookup<float(Voice*, void*)>(*jit, "load");
    constexpr std::string_view kText = R"({"voice": {"gain": 0.25, "note": 60}})";
    void* doc = parse(kText.data(), static_cast<int>(kText.size()));
    REQUIRE(doc != nullptr);
    Voice v{};
    CHECK(load(&v, doc) == 1060.25f);   // the stores before the call do not reach past it
    CHECK((v.gain == 0.25f && v.note == 60));
    CHECK(load(&v, nullptr) == 6.0f);   // nothing filled: the method's own values
    release(doc);
}

TEST_CASE("every read by json_read allocates nothing and makes no system call", "[codegen][json-read]")
{
    const SynthReads read;
    void* doc = fminus_rt_json_parse(kFull.data(), static_cast<std::int32_t>(kFull.size()));
    REQUIRE(doc != nullptr);
    const auto readAll = [&] {
        Synth s = patterned();
        double sum = read.fill(doc, &s) ? 1 : 0;
        sum += static_cast<double>(s.gain) + s.tuning + s.voices + s.mode + static_cast<double>(s.bands[2].q)
               + static_cast<double>(s.lfo.w) + s.grid[1][2] + s.name[0];
        Synth none = patterned();
        sum += read.fill(nullptr, &none) ? 1 : 0;
        sum += static_cast<double>(none.filter.cutoff);   // its default
        return sum;
    };
    const double expected = readAll();
    CHECK(expected == 1 + 0.5 + 440.25 + 7 + 2 + 3 + 3.5 + 6 + 'w' + 0 + 1000);
    if (HeapCalls::counted())
    {
        bool same = true;
        long calls = 0;
        {
            const HeapCalls counting;
            for (int i = 0; i < 100; ++i)
                same = same && readAll() == expected;
            calls = counting.count();
        }
        CHECK(same);
        CHECK(calls == 0);
    }
#if defined(__linux__)
    // In a child under seccomp's strict mode, where any system call but read, write and exit kills it.
    if (const Sealed<double> sum = inStrictMode(readAll))
        CHECK(*sum == expected);
#endif
    fminus_rt_json_free(doc);
}

TEST_CASE("a module's table, word by word: the handoff's worked example, one constant a type", "[codegen][json-read]")
{
    const Lowered lowered("enum Wave { SINE, SAW };\n"
                          "module Osc { float detune; enum Wave wave; };\n"
                          "module Filter\n"
                          "{\n"
                          "    float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                          "    float q;\n"
                          "};\n"
                          "module Rack { Filter filters[2]; Osc osc; string label; param float level; };\n"
                          "bool a(json_doc d, Filter* f) { return json_read(d, \"\", f); }\n"
                          "bool b(json_doc d, Filter* f, int k) { return json_read(d, \"/f/%d\", k, f); }\n"
                          "module Box { Filter f; bool c(json_doc d) { return json_read(d, \"/f\", &f); } };\n"
                          "bool e(json_doc d, Osc* o) { return json_read(d, \"\", o); }\n"
                          "bool g(json_doc d, Rack* r) { return json_read(d, \"\", r); }\n");
    INFO(lowered.errors);
    REQUIRE(lowered.module);
    // Three calls of Filter, one table; one for Osc and one for Rack, which holds Filter's and Osc's shapes inside it.
    CHECK(shapeTables(*lowered.module)
          == std::vector<std::string>{"json.shape.Filter", "json.shape.Osc", "json.shape.Rack"});
    const Words filter = wordsOf(*lowered.module->getNamedGlobal("json.shape.Filter"));
    CHECK(filter.words == std::vector<std::int32_t>{20, 7,                         // words, bytes
                                                    12, 2,                         // a module of 2 fields
                                                    12, 0, 6, 0, 3, 0, 1077149696, // "cutoff" at 0, range 20..
                                                    0, 1087604736, 0, 1083129856,  // ..20000, default 1000
                                                    1,                             // float
                                                    6, 6, 1, 4, 0, 1});            // "q" at 4, no flags, float
    CHECK(filter.bytes == "cutoffq");
    // The handoff's Osc, its enumeration's constants as written.
    const Words osc = wordsOf(*lowered.module->getNamedGlobal("json.shape.Osc"));
    CHECK(osc.words == std::vector<std::int32_t>{22, 17, 12, 2, 6, 0, 6, 0, 0, 1, 14, 6, 4, 4, 0, 7, 4, 2, 0, 10, 4, 1,
                                                 14, 3});
    CHECK(osc.bytes == "detunewaveSINESAW");
    // Rack: an array of 2 Filters, 8 bytes apart, and an Osc at 16; no string, no parameter.
    const Words rack = wordsOf(*lowered.module->getNamedGlobal("json.shape.Rack"));
    CHECK(rack.words
          == std::vector<std::int32_t>{
              57, 34, 12, 2,
              // "filters" at 0, an array of 2, stride 8, of Filter
              28, 0, 7, 0, 0, 9, 2, 8, 12, 2, 12, 7, 6, 0, 3, 0, 1077149696, 0, 1087604736, 0, 1083129856, 1,
              6, 13, 1, 4, 0, 1,
              // "osc" at 16, Osc
              27, 14, 3, 16, 0, 12, 2, 6, 17, 6, 0, 0, 1, 14, 23, 4, 4, 0, 7, 4, 2, 0, 27, 4, 1, 31, 3});
    CHECK(rack.bytes == "filterscutoffqoscdetunewaveSINESAW");
}

TEST_CASE("json_read's errors and the json:: attributes', each once at its column", "[codegen][json-read]")
{
    CHECK(errorsOf("module Empty { string s; param float k; float x [[json::skip]]; float* p; };\n"
                   "module Fine { float a; };\n"
                   "bool e1(json_doc d, Empty* m) { return json_read(d, \"\", m); }\n"
                   "bool e2(json_doc d, float* x) { return json_read(d, \"\", x); }\n"
                   "bool e3(json_doc d) { Fine m; return json_read(d, \"\", m); }\n"
                   "bool e4(json_doc d, const Fine* m) { return json_read(d, \"\", m); }\n"
                   "bool e5(json_doc d) { return json_read(d, \"\", \"m\"); }\n"
                   "bool e6(json_doc d, Fine* m) { return json_read(d, \"/%d\", m); }\n"
                   "bool e7(json_doc d, Fine* m) { return json_read(d, m); }\n"
                   "bool e8(json_doc d, void* m) { return json_read(d, \"x\", m); }\n"
                   "bool e9(json_doc d, Fine* m) { return json_read(d, \"\", 1, m); }\n"
                   "bool e10(xml_doc d, Fine* m) { return json_read(d, \"\", m); }\n")
          == "test.fm:3:40: error: 'json_read' has nothing to fill in 'Empty': it fills no parameter, string, pointer, "
             "handle, poly type or field with [[json::skip]], and 'Empty' has no other field\n"
             "test.fm:4:57: error: 'json_read' fills a module: its last argument must be a pointer to one, not "
             "'float *'\n"
             "test.fm:5:55: error: 'json_read' fills a module: its last argument must be a pointer to one, not 'Fine'\n"
             "test.fm:6:45: error: passing 'const Fine *' to parameter of type 'Fine *' discards qualifiers\n"
             "test.fm:7:47: error: 'json_read' fills a module: its last argument must be a pointer to one, not a "
             "string literal\n"
             "test.fm:8:55: error: more '%' conversions than data arguments\n"
             "test.fm:9:39: error: too few arguments to function call, expected at least 3, have 2\n"
             "test.fm:10:53: error: a JSON Pointer must be empty or begin with '/'\n"
             "test.fm:10:57: error: 'json_read' fills a module: its last argument must be a pointer to one, not "
             "'void *'\n"
             "test.fm:11:56: error: data argument not used by format string\n"
             "test.fm:12:39: error: incompatible pointer types passing 'xml_doc' to parameter of type 'json_doc'\n");
    CHECK(errorsOf("module Attrs\n"
                   "{\n"
                   "    float a [[json::name]] [[json::name(1)]] [[json::name(\"\")]];\n"
                   "    float b [[json::name(\"\\xff\")]] [[json::skip(1)]] [[json::rename(\"x\")]];\n"
                   "    float c [[json::name(\"a\")]];\n"
                   "    float d;\n"
                   "    string s [[json::name(\"d\")]];\n"
                   "    float e [[json::name(\"d\")]] [[json::skip]];\n"
                   "    float* p [[json::name(\"d\")]];\n"
                   "    string t [[fminus::default(1)]];\n"
                   "    int i [[fminus::default(1.5)]];\n"
                   "    bool o [[fminus::default(2)]];\n"
                   "    float r [[fminus::range(0, 1)]] [[fminus::default(2)]];\n"
                   "    param float g [[json::bad]];\n"
                   "};\n")
          == "test.fm:3:15: error: 'json::name' attribute takes one argument\n"
             "test.fm:3:30: error: 'json::name' attribute requires a string\n"
             "test.fm:3:48: error: 'json::name' takes a name of one byte or more: the JSON member's the field is "
             "read and written as\n"
             "test.fm:4:15: error: 'json::name' takes UTF-8 text, as a JSON member's name is, and byte 1 of the "
             "name is no part of any\n"
             "test.fm:4:38: error: 'json::skip' attribute takes no arguments\n"
             "test.fm:4:56: error: unknown attribute 'json::rename': a field takes json::name(\"name\") and "
             "json::skip\n"
             "test.fm:5:15: error: duplicate JSON name 'a' in module 'Attrs'\n"
             "test.fm:3:11: note: previous field of that JSON name is here\n"
             "test.fm:7:16: error: duplicate JSON name 'd' in module 'Attrs'\n"
             "test.fm:6:11: note: previous field of that JSON name is here\n"
             "test.fm:10:16: error: 'fminus::default' does not apply to a string field: json_read leaves a string "
             "field out, so its default would never be used\n"
             "test.fm:11:13: error: the default of an int field is an int, not 1.5\n"
             "test.fm:12:14: error: the default of a bool field is 0 or 1, not 2\n"
             "test.fm:13:11: error: the default of 'r', 2, is outside its fminus::range(0, 1)\n"
             "test.fm:14:21: error: unknown attribute 'json::bad': a field takes json::name(\"name\") and "
             "json::skip\n");
    // A table the runtime would refuse: shapes nested past 16 levels (17 modules, each holding the last: the 17th's
    // float at level 17), a module of more than 1024 fields to fill. One level less, and 1024 fields, are read.
    std::string deep = "module M0 { float a; };\n";
    for (int k = 1; k <= 16; ++k)
        deep += "module M" + std::to_string(k) + " { M" + std::to_string(k - 1) + " m; };\n";
    const auto fields = [](const char* name, int count) {
        std::string module = std::string("module ") + name + " {";
        for (int k = 0; k < count; ++k)
            module += " float f" + std::to_string(k) + ";";
        return module + " };\n";
    };
    std::string errors;
    CHECK(compile(deep + fields("Wide", 1024)
                      + "bool a(json_doc d, M15* m) { return json_read(d, \"\", m); }\n"
                        "bool b(json_doc d, Wide* w) { return json_read(d, \"\", w); }\n",
                  &errors));
    CHECK(errors.empty());
    CHECK(errorsOf(deep + fields("Many", 1025)
                   + "bool a(json_doc d, M16* m) { return json_read(d, \"\", m); }\n"
                     "bool b(json_doc d, Many* w) { return json_read(d, \"\", w); }\n")
          == "test.fm:19:37: error: 'json_read' cannot fill 'M16': its modules and arrays nest more than 16 deep, the "
             "most its table holds\n"
             "test.fm:20:38: error: 'json_read' cannot fill 'Many': a module in it has more than 1024 fields to fill, "
             "the most its table holds\n");
    // A name without its namespace is C's: ignored, with clang's warning and a note.
    std::string warnings;
    CHECK(compile("module M { float a [[name(\"x\")]] [[skip]] [[default(1)]]; };\n", &warnings));
    CHECK(warnings == "test.fm:1:22: warning: unknown attribute 'name' ignored [-Wunknown-attributes]\n"
                      "test.fm:1:22: note: did you mean 'json::name'?\n"
                      "test.fm:1:36: warning: unknown attribute 'skip' ignored [-Wunknown-attributes]\n"
                      "test.fm:1:36: note: did you mean 'json::skip'?\n"
                      "test.fm:1:45: warning: unknown attribute 'default' ignored [-Wunknown-attributes]\n"
                      "test.fm:1:45: note: did you mean 'fminus::default'?\n");
}

TEST_CASE("json_read is wait-free: a nonblocking function may call it", "[codegen][json-read][effects]")
{
    std::string errors;
    CHECK(compile("module M { float a; };\n"
                  "bool r(json_doc d, M* m) [[clang::nonblocking]] { return json_read(d, \"\", m); }\n"
                  "bool s(json_doc d, M* m) [[clang::nonallocating]] { return json_read(d, \"\", m); }\n",
                  &errors));
    CHECK(errors.empty());
}
#endif

TEST_CASE("json_read is a known name in every build, and an error that names the option in one without it",
          "[codegen][json-read]")
{
    std::string errors;
    const auto jit = compile(kCall, &errors);
    if (FMINUS_HAS_JSON != 0)
    {
        CHECK(jit);
        CHECK(errors.empty());
    }
    else
    {
        CHECK(!jit);
        CHECK(errors == "test.fm:2:35: error: 'json_read' needs the JSON parser, which this build of FMinus leaves out "
                        "(FMINUS_JSON=OFF)\n");
    }
    // A program's function of its name is refused, as any builtin's.
    CHECK(errorsOf("module M { float a; };\nbool json_read(json_doc d, M* m) { return true; }\n")
          == "test.fm:2:6: error: 'json_read' is a builtin and cannot be redefined\n");
}
