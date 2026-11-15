// tests/unit/JsonLiteralTest.cpp: the JSON literal's values and sinks (src/ir/Shapes.cpp, Calls.cpp, Serve.cpp;
// docs/web.md, "The JSON literal"): every kind of value a literal writes (numbers, bools, strings, enumerations by
// their constants' names, char arrays, arrays of one and two dimensions, vectors, modules and arrays of them, nested),
// written as a file's bytes and read back, through json_parse and json_read, into a module equal to the one written;
// `json (value)` of each kind at the top; json_slice, of an array and of a pointer; a module's JSON names, what it
// leaves out and its parameters, written as *this in a method of its own; file_append's JSON Lines; a literal over the
// policy's size; values evaluated once whatever the number of passes; every error once, at its column. Then, with the
// web server, a reply of a module, http_reply_json in the http_is style, a reply over maxReplyBytes, a POST's body and
// its limit, the wait-free proof of a reply's module in a [[clang::nonblocking]] method (HeapCalls and seccomp's strict
// mode), and the research's "data work": an array of modules written with file_write and posted with http_post to a
// loopback server the test starts, from days of readings fetched from it at once under a within.
//
// Files are written only in a directory each test makes beside bin/ (the build tree) and removes; nothing binds or
// connects anywhere but 127.0.0.1. Each part is compiled only in a build with what it needs (#if), and the errors of a
// build without it are checked there.
#include "Compile.h"
#include "fminus/Config.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fminus::test;

TEST_CASE("json_slice is a JSON literal's value and nothing else; a JSON literal is no value", "[codegen][json]")
{
#if FMINUS_HAS_JSON
    const std::string slice = "'json_slice' is a JSON literal's value, the first elements of an array written there, "
                              "and nothing anywhere else\n";
#else
    const std::string slice = "'json_slice' needs the JSON writer, which this build of FMinus leaves out "
                              "(FMINUS_JSON=OFF)\n";
#endif
    CHECK(errorsOf("float f(float* p)\n"
                   "{\n"
                   "    float a[4];\n"
                   "    json_slice(a, 2);\n"
                   "    const float x = json_slice(p, 1) + json (1.0f);\n"
                   "    return x;\n"
                   "}\n")
          == "test.fm:4:5: error: " + slice + "test.fm:5:21: error: " + slice
                 + "test.fm:5:40: error: FMinus has no JSON values: a JSON literal is only the body of a reply, "
                   "http_reply_json, http_post, file_write or file_append\n");
    // A builtin's name: a prototype cannot be written (it takes an array or a pointer of any kind), nor a definition.
    CHECK(errorsOf("int json_slice(float* a, int n);\n"
                   "int http_reply_json(http_request r, int status, int n) { return 0; }\n")
          == "test.fm:1:5: error: conflicting types for 'json_slice'\n"
             "test.fm:2:5: error: 'http_reply_json' is a builtin and cannot be redefined\n");
}

#if FMINUS_HAS_FILES && !FMINUS_HAS_FILES_WRITE
TEST_CASE("a JSON literal to a file, in a build without file writing, is the error that names the option",
          "[codegen][json][files]")
{
    CHECK(errorsOf("int f(int n) { return file_write(\"out/a.json\", json { \"n\": n }) + file_append(\"out/a.jsonl\", "
                   "json (n)); }\n")
          == "test.fm:1:23: error: 'file_write' needs file writing, which this build of FMinus leaves out "
             "(FMINUS_FILES_WRITE=OFF)\n"
             "test.fm:1:67: error: 'file_append' needs file writing, which this build of FMinus leaves out "
             "(FMINUS_FILES_WRITE=OFF)\n");
}
#endif

#if !FMINUS_HAS_WEB
TEST_CASE("a POST of a JSON literal, in a build without the web library, is the error that names the option",
          "[codegen][json][web]")
{
    CHECK(errorsOf("http_response f(int n) { return http_post(\"https://example.com/a\", json { \"n\": n }); }\n")
          == "test.fm:1:33: error: 'http_post' needs the web library, which this build of FMinus leaves out "
             "(FMINUS_WEB=OFF)\n");
}
#endif

#if FMINUS_HAS_FILES_WRITE && !FMINUS_HAS_JSON
TEST_CASE("a JSON literal to a file, in a build without the JSON writer, is the error that names the option",
          "[codegen][json][files]")
{
    CHECK(errorsOf("int f(int n) { return file_write(\"out/a.json\", json { \"n\": n }); }\n")
          == "test.fm:1:48: error: a JSON literal needs the JSON writer, which this build of FMinus leaves out "
             "(FMINUS_JSON=OFF)\n");
}
#endif

#if FMINUS_HAS_FILES_WRITE && FMINUS_HAS_JSON && !defined(_WIN32)
#include "fminus/FMinus.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <unistd.h>

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
            path_ = tree / "json-literal-test" / ("t" + std::to_string(::getpid()) + "-" + std::to_string(made++));
            fs::remove_all(path_);
            fs::create_directories(path_);
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

        const fs::path& path() const { return path_; }

        // The bytes of the file `name` in it, or "<none>".
        std::string read(const std::string& name) const
        {
            std::ifstream in(path_ / name, std::ios::binary);
            if (!in)
                return "<none>";
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

    private:
        fs::path path_;
    };

    // `source` compiled with the scratch directory as the folder "out", writable.
    std::unique_ptr<fminus::Program> compileWriting(const std::string& source, const Scratch& scratch,
                                                    fminus::Program::Options options = {})
    {
        options.files.folders = {{"out", scratch.path().string(), true, {}}};
        std::string errors;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
    }

    // A module's instance: its bytes, zeroed and aligned as Program::Module says (a float8's 32 among them).
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

    constexpr const char* kAll = R"(
enum Wave { SINE, SAW, SQUARE };
enum Flag : bool { OFF, ON };

module Inner { int id; float level; };

module All
{
    float f;
    double d;
    int i;
    bool b;
    enum Wave wave;
    enum Flag flag;
    char name[8];
    float taps[3];
    float grid[2][3];
    float4 v;
    Inner inner;
    Inner list[2];
    string tag;
    int ids[2];
    enum Wave waves[3];
    float2 u;
    float8 w;
    double ds[2];
    char names[2][4];
};

void fill(All* a)
{
    a->f = 0.1f;
    a->d = 1.0 / 3.0;
    a->i = -7;
    a->b = true;
    a->wave = SAW;
    a->flag = ON;
    a->name[0] = 104;
    a->name[1] = 105;
    a->taps[0] = 0.5f;
    a->taps[1] = 1.0f;
    a->taps[2] = -2.0f;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            a->grid[r][c] = (float)(3 * r + c + 1);
    a->grid[1][2] = 6.5f;
    a->v = (float4)(1, 2, 3, 4.5f);
    a->inner.id = 3;
    a->inner.level = 0.25f;
    a->list[0].id = 1;
    a->list[0].level = -1.0f;
    a->list[1].id = 2;
    a->list[1].level = 1e20f;
    a->tag = "a\"b\n";
    a->ids[0] = 10;
    a->ids[1] = -20;
    a->waves[0] = SQUARE;
    a->waves[1] = SINE;
    a->waves[2] = (enum Wave)7;
    a->u = (float2)(0.25f, -0.5f);
    a->w = (float8)(1, 2, 3, 4, 5, 6, 7, 8);
    a->ds[0] = 0.1;
    a->ds[1] = -2.5;
    a->names[0][0] = 120;
    a->names[1][0] = 121;
    a->names[1][1] = 122;
}

int write(const All* a) { return file_write("out/all.json", json (*a)); }

// The file read back into `back` by json_read: 1 when every field it fills was filled; the tag, a string json_read
// leaves out, compared through json_string (2 more when it is the same).
int readBack(const All* a, All* back)
{
    file_content f = file_read("out/all.json");
    defer file_close(f);
    json_doc doc = json_parse(file_data(f), file_size(f));
    defer json_free(doc);
    return (json_read(doc, "", back) ? 1 : 0) + (json_string(doc, "/tag", "?") == a->tag ? 2 : 0);
}
)";

    constexpr std::string_view kAllText =
        R"({"f":0.1,"d":0.3333333333333333,"i":-7,"b":true,"wave":"SAW","flag":"ON","name":"hi","taps":[0.5,1,-2],)"
        R"("grid":[[1,2,3],[4,5,6.5]],"v":[1,2,3,4.5],"inner":{"id":3,"level":0.25},)"
        R"("list":[{"id":1,"level":-1},{"id":2,"level":1e+20}],"tag":"a\"b\n","ids":[10,-20],)"
        R"("waves":["SQUARE","SINE",7],"u":[0.25,-0.5],"w":[1,2,3,4,5,6,7,8],"ds":[0.1,-2.5],"names":["x","yz"]})";
} // namespace

TEST_CASE("a module of every kind of field, written as a JSON literal and read back with json_read, is the same",
          "[codegen][json][json-read][files]")
{
    const Scratch scratch;
    auto program = compileWriting(kAll, scratch);
    std::string errors;
    auto* fill = program->function<void(void*)>("fill", errors);
    auto* write = program->function<int(const void*)>("write", errors);
    auto* readBack = program->function<int(const void*, void*)>("readBack", errors);
    INFO(errors);
    REQUIRE(fill);
    REQUIRE(write);
    REQUIRE(readBack);
    const fminus::Program::Module& layout = *program->module("All");
    Instance all(layout);
    Instance back(layout);
    fill(all.memory());
    CHECK(write(all.memory()) == 0);   // FILE_OK
    CHECK(scratch.read("all.json") == kAllText);
    CHECK(readBack(all.memory(), back.memory()) == 3);
    // Every field but the tag (a string's address, which json_read leaves as it was) is the same, byte for byte.
    for (const fminus::Program::Module::Field& field : layout.fields)
    {
        if (field.name == "tag")
            continue;
        INFO(field.name);
        CHECK(std::memcmp(static_cast<char*>(all.memory()) + field.offset,
                          static_cast<char*>(back.memory()) + field.offset, field.size)
              == 0);
    }
}

TEST_CASE("each kind of value at the top of json (value), in an object and in an array; json_slice of an array and of "
          "a pointer",
          "[codegen][json][files]")
{
    const Scratch scratch;
    auto program = compileWriting(R"(
enum Wave { SINE, SAW, SQUARE };
module Day { int sensor; float mean; float peak; };

int values(int n, const Day* p)
{
    float taps[3] = {0.5f, 1.0f, -2.0f};
    float grid[2][2] = {{1, 2}, {3, 4}};
    Day days[3];
    for (int k = 0; k < 3; ++k)
    {
        days[k].sensor = k;
        days[k].mean = 0.5f * (float)k;
        days[k].peak = (float)(2 * k);
    }
    const string names[2] = {"warm", "c\x01ld"};
    enum Wave w = SQUARE;
    const float* q = &taps[1];
    int failed = 0;
    failed += file_write("out/number.json", json (0.1f)) != FILE_OK ? 1 : 0;
    failed += file_write("out/double.json", json (0.1)) != FILE_OK ? 1 : 0;
    failed += file_write("out/bool.json", json (n > 1)) != FILE_OK ? 1 : 0;
    failed += file_write("out/string.json", json ("t\"x")) != FILE_OK ? 1 : 0;
    failed += file_write("out/enum.json", json (w)) != FILE_OK ? 1 : 0;
    failed += file_write("out/array.json", json (taps)) != FILE_OK ? 1 : 0;
    failed += file_write("out/module.json", json (days[1])) != FILE_OK ? 1 : 0;
    failed += file_write("out/pointed.json", json (*p)) != FILE_OK ? 1 : 0;
    failed += file_write("out/vector.json", json ((float4)(1, 2, 3, 4))) != FILE_OK ? 1 : 0;
    failed += file_write("out/slice.json", json (json_slice(days, n))) != FILE_OK ? 1 : 0;
    failed += file_write("out/object.json", json { "first": json_slice(days, n), "all": json_slice(days, 99),
                                                   "none": json_slice(days, -1), "ptr": json_slice(q, 2),
                                                   "rows": json_slice(grid, 1), "names": names, "grid": grid,
                                                   "names1": json_slice(names, 1) }) != FILE_OK ? 1 : 0;
    failed += file_write("out/array2.json", json [w, (enum Wave)9, names[1], taps[0], [1, json_slice(taps, 2)], {}])
              != FILE_OK ? 1 : 0;
    return failed;
}
)",
                                  scratch);
    std::string errors;
    auto* values = program->function<int(int, const void*)>("values", errors);
    INFO(errors);
    REQUIRE(values);
    struct Day
    {
        std::int32_t sensor;
        float mean;
        float peak;
    } pointed{9, -0.125f, 1.5f};
    CHECK(values(2, &pointed) == 0);
    CHECK(scratch.read("number.json") == "0.1");
    CHECK(scratch.read("double.json") == "0.1");
    CHECK(scratch.read("bool.json") == "true");
    CHECK(scratch.read("string.json") == R"("t\"x")");
    CHECK(scratch.read("enum.json") == R"("SQUARE")");
    CHECK(scratch.read("array.json") == "[0.5,1,-2]");
    CHECK(scratch.read("module.json") == R"({"sensor":1,"mean":0.5,"peak":2})");
    CHECK(scratch.read("pointed.json") == R"({"sensor":9,"mean":-0.125,"peak":1.5})");
    CHECK(scratch.read("vector.json") == "[1,2,3,4]");
    CHECK(scratch.read("slice.json")
          == R"([{"sensor":0,"mean":0,"peak":0},{"sensor":1,"mean":0.5,"peak":2}])");
    CHECK(scratch.read("object.json")
          == R"({"first":[{"sensor":0,"mean":0,"peak":0},{"sensor":1,"mean":0.5,"peak":2}],)"
             R"("all":[{"sensor":0,"mean":0,"peak":0},{"sensor":1,"mean":0.5,"peak":2},{"sensor":2,"mean":1,"peak":4}],)"
             R"("none":[],"ptr":[1,-2],"rows":[[1,2]],"names":["warm","c\u0001ld"],"grid":[[1,2],[3,4]],)"
             R"("names1":["warm"]})");
    CHECK(scratch.read("array2.json") == R"(["SQUARE",9,"c\u0001ld",0.5,[1,[0.5,1]],{}])");
}

TEST_CASE("a module's JSON names, what it leaves out, and a char array to its first 0", "[codegen][json][files]")
{
    const Scratch scratch;
    auto program = compileWriting(R"(
module Note { int pitch; };

module Tagged
{
    float gain [[json::name("Gain")]];
    int count [[json::skip]];
    http_response r;
    float* p;
    char label[6];
    char empty[4];
    string note;
    float q [[json::name("q \"quoted\"\n")]];
    float e [[json::name("\xc3\xa9t\xc3\xa9")]];
    Note notes [[json::name("notes!")]] [2];
    Note skipped [[json::skip]];
};

int write()
{
    Tagged t;
    t.gain = 0.5f;
    t.count = 7;
    t.label[0] = 97;
    t.label[1] = 98;
    t.label[2] = 99;
    t.label[3] = 100;
    t.label[4] = 101;
    t.label[5] = 102;
    t.note = "n";
    t.q = 2.0f;
    t.e = -1.0f;
    t.notes[1].pitch = 60;
    return file_write("out/tagged.json", json { "t": t, "also": [t.label, t.empty] });
}
)",
                                  scratch);
    std::string errors;
    auto* write = program->function<int()>("write", errors);
    INFO(errors);
    REQUIRE(write);
    CHECK(write() == 0);
    // A char array of six bytes and no 0 is the six; one of no letters is "".
    const std::string tagged = R"({"Gain":0.5,"label":"abcdef","empty":"","note":"n","q \"quoted\"\n":2,)"
                               "\"\xc3\xa9t\xc3\xa9\":-1,"
                               R"("notes!":[{"pitch":0},{"pitch":60}]})";
    CHECK(scratch.read("tagged.json") == R"({"t":)" + tagged + R"(,"also":["abcdef",""]})");
}

TEST_CASE("a module's parameters are written as *this, in a method of its own, each as name[0] reads it",
          "[codegen][json][files][params]")
{
    constexpr const char* kSynth = R"(
module Synth
{
    param float cutoff;
    param int voices;
    param bool on;
    param float drive [[json::skip]];
    float level;
    float tail[2];
    int writes;

    void init() { level = 0.5f; tail[0] = 1.0f; tail[1] = 2.0f; }
    int save()
    {
        writes += 1;
        return file_write("out/synth.json", json { "synth": *this, "writes": writes });
    }
};
)";
    const Scratch scratch;
    for (const char* binding : {"knob", "frozen", "stream"})
    {
        INFO(binding);
        fminus::Program::Options options;
        if (std::string(binding) == "frozen")
            options.parameters["Synth::cutoff"] = fminus::Program::Binding::frozen(440.0);
        if (std::string(binding) == "stream")
            options.parameters["Synth::cutoff"] = fminus::Program::Binding::stream();
        auto program = compileWriting(kSynth, scratch, options);
        std::string errors;
        auto* init = program->function<void(void*)>("Synth::init", errors);
        auto* save = program->function<int(void*)>("Synth::save", errors);
        INFO(errors);
        REQUIRE(init);
        REQUIRE(save);
        Instance synth(*program->module("Synth"));
        init(synth.memory());
        fminus::Program::setValue(synth.memory(), *program->parameter("Synth::cutoff"), 1200.0);
        fminus::Program::setValue(synth.memory(), *program->parameter("Synth::voices"), 3.0);
        fminus::Program::setValue(synth.memory(), *program->parameter("Synth::on"), 1.0);
        const float samples[2] = {7.5f, 8.0f};
        if (std::string(binding) == "stream")
            fminus::Program::setStream(synth.memory(), *program->parameter("Synth::cutoff"), samples);
        CHECK(save(synth.memory()) == 0);
        const std::string cutoff = std::string(binding) == "knob" ? "1200" : std::string(binding) == "frozen" ? "440"
                                                                                                            : "7.5";
        CHECK(scratch.read("synth.json")
              == R"({"synth":{"cutoff":)" + cutoff
                     + R"(,"voices":3,"on":true,"level":0.5,"tail":[1,2],"writes":1},"writes":1})");
    }
}

TEST_CASE("file_append writes JSON Lines; a literal over maxFileBytes is FILE_LIMIT; values are evaluated once",
          "[codegen][json][files]")
{
    const Scratch scratch;
    fminus::Program::Options options;
    options.files.maxFileBytes = 40;
    auto program = compileWriting(R"(
module Counter
{
    int calls;
    int next() { calls += 1; return calls; }
    int log(int k)
    {
        const string name = k == 0 ? "a" : "b";
        return file_append("out/{name}.jsonl", json { "n": next(), "m": [next(), next()] });
    }
    int big() { return file_write("out/big.json", json { "a long key, longer than the policy's limit": next() }); }
    int fits() { return file_write("out/fits.json", json [next(), next()]); }
};
)",
                                  scratch, options);
    std::string errors;
    auto* log = program->function<int(void*, int)>("Counter::log", errors);
    auto* big = program->function<int(void*)>("Counter::big", errors);
    auto* fits = program->function<int(void*)>("Counter::fits", errors);
    INFO(errors);
    REQUIRE(log);
    REQUIRE(big);
    REQUIRE(fits);
    Instance counter(*program->module("Counter"));
    const auto calls = [&] {
        std::int32_t n = 0;
        std::memcpy(&n, counter.memory(), sizeof n);
        return n;
    };
    // Each value once, in the order written, though the text is measured and then written.
    CHECK(log(counter.memory(), 0) == 0);
    CHECK(calls() == 3);
    CHECK(log(counter.memory(), 0) == 0);
    CHECK(scratch.read("a.jsonl") == "{\"n\":1,\"m\":[2,3]}\n{\"n\":4,\"m\":[5,6]}\n");
    CHECK(log(counter.memory(), 1) == 0);
    CHECK(scratch.read("b.jsonl") == "{\"n\":7,\"m\":[8,9]}\n");
    // A third line would take a.jsonl past 40 bytes; a literal of 46 bytes is past them alone: FILE_LIMIT, and its
    // value was evaluated all the same.
    CHECK(log(counter.memory(), 0) == -4);   // FILE_LIMIT
    CHECK(big(counter.memory()) == -4);
    CHECK(scratch.read("big.json") == "<none>");
    CHECK(fits(counter.memory()) == 0);
    CHECK(scratch.read("fits.json") == "[14,15]");
    CHECK(calls() == 15);
}

TEST_CASE("a JSON literal's errors, each once at its column", "[codegen][json]")
{
    const std::string values = "a JSON literal's value is a float, a double, an int, a bool, a string, an enumeration's "
                               "value, a float2, float4 or float8, an array or a module, or an object or an array of "
                               "them, not ";
    const std::string parameter = "a JSON literal writes a parameter's value only where a method of its module reads "
                                  "it, as *this: 'cutoff' of 'Synth' is one (leave it out with [[json::skip]])\n";
    CHECK(errorsOf("module Synth\n"
                   "{\n"
                   "    param float cutoff;\n"
                   "    float level;\n"
                   "    int save(Synth* other) { return file_write(\"out/a.json\", json [*other, this]); }\n"
                   "};\n"
                   "module Holder { Synth s; };\n"
                   "int f(float* p, http_server h, Synth* s, Holder* o, const void* v, int n)\n"
                   "{\n"
                   "    http_response rs[2];\n"
                   "    poly float pf = poly_index();\n"
                   "    float4 one = (float4)(1);\n"
                   "    float a[4];\n"
                   "    char text[8];\n"
                   "    Synth many[2];\n"
                   "    return file_write(\"out/a.json\", json { \"p\": p, \"h\": h, \"rs\": rs, \"pf\": pf, \"m\": one > 0, "
                   "\"s\": *s })\n"
                   "           + file_write(\"out/b.json\", json [*o, many, json_slice(many, 1), json_slice(a), "
                   "json_slice(\"ab\", 1)])\n"
                   "           + file_write(\"out/c.json\", json [json_slice(v, 1), json_slice(text, 2), json_slice(a, "
                   "\"x\"), json_slice(n, 1)])\n"
                   "           + file_write(\"out/d.json\", json { \"a\": 1, \"a\": 2 }, n)\n"
                   "           + file_append(\"out/e.json\", n) + file_write(\"out/f.json\", 1, json (n));\n"
                   "}\n")
          == "test.fm:5:68: error: " + parameter
                 + "test.fm:5:76: error: a JSON literal does not write a pointer, 'Synth *': write what it points to, "
                   "*p, or its first elements, json_slice(p, count)\n"
                   "test.fm:16:49: error: a JSON literal does not write a pointer, 'float *': write what it points to, "
                   "*p, or its first elements, json_slice(p, count)\n"
                   "test.fm:16:57: error: " + values + "'http_server'\n"
                 + "test.fm:16:66: error: " + values + "'http_response[2]'\n"
                 + "test.fm:16:76: error: " + values + "'poly float'\n"
                 + "test.fm:16:85: error: " + values + "'bool4'\n"
                 + "test.fm:16:99: error: " + parameter
                 // A module that holds one with a parameter, an array of them, and a slice of that array.
                 + "test.fm:17:45: error: " + parameter + "test.fm:17:49: error: " + parameter
                 + "test.fm:17:66: error: " + parameter
                 + "test.fm:17:76: error: too few arguments to function call, expected 2, have 1\n"
                   "test.fm:17:102: error: 'json_slice' takes an array or a pointer, and a count: not a string "
                   "literal\n"
                   "test.fm:18:56: error: 'json_slice' takes an array or a pointer, and a count: not 'const void *'\n"
                   "test.fm:18:74: error: 'json_slice' writes elements a JSON literal writes, and a char is none: a "
                   "char array is a string, written whole\n"
                   "test.fm:18:98: error: incompatible pointer to integer conversion passing 'char[2]' to parameter of "
                   "type 'int'\n"
                   "test.fm:18:115: error: 'json_slice' takes an array or a pointer, and a count: not 'int'\n"
                   "test.fm:19:54: error: duplicate key 'a' in a JSON object\n"
                   "test.fm:19:46: note: previous key is here\n"
                   // A value after the literal is the URL's or the name's, as any (and this one has no %d).
                   "test.fm:19:64: error: data argument not used by format string\n"
                   // The plain forms, given no literal, or a literal where their bytes' count goes.
                   "test.fm:20:14: error: too few arguments to function call, expected at least 3, have 2\n"
                   "test.fm:20:45: error: passing 'int' to parameter of incompatible type 'const void *'\n"
                   "test.fm:20:73: error: FMinus has no JSON values: a JSON literal is only the body of a reply, "
                   "http_reply_json, http_post, file_write or file_append\n");
}
#endif
