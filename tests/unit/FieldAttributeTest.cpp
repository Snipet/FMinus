// tests/unit/FieldAttributeTest.cpp: attributes on a module's fields (ir/Modules.cpp, checkAttributes). FMinus's own,
// fminus::range on a field of numbers or a parameter and fminus::default on a parameter, are checked; clang's are a
// function's; a name without a namespace that C23 does not have is ignored with clang's warning; any other is the
// host's, passed on as written, and changes nothing in the code. A local's (a parallel for's counter's too) or a
// constant's is refused. The host's view of them is tests/library/FieldAttributeTest.cpp.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/Support/raw_ostream.h>

#include <string>
#include <string_view>

using namespace fminus::test;

namespace
{
    // The program's IR as the code generator lowers it, unoptimised, without its source file's name.
    std::string loweredIr(std::string_view source)
    {
        fminus::Diagnostics diagnostics("test.fm");
        const auto program = fminus::frontend::parse(source, diagnostics);
        REQUIRE(program);
        llvm::LLVMContext context;
        const std::unique_ptr<llvm::Module> module = fminus::ir::lower(*program, context, diagnostics, {});
        REQUIRE(module);
        std::string text;
        llvm::raw_string_ostream stream(text);
        module->print(stream, nullptr);
        return text;
    }

    constexpr const char* kMeter = "module Meter\n"
                                   "{\n"
                                   "    param float gain [[fminus::range(-24, 12)]] [[fminus::default(0)]];\n"
                                   "    float level;\n"
                                   "    float history[2][4];\n"
                                   "    double sum;\n"
                                   "    float tick(float x)\n"
                                   "    {\n"
                                   "        level = x * gain[0];\n"
                                   "        history[1][3] = level;\n"
                                   "        sum += level;\n"
                                   "        return level;\n"
                                   "    }\n"
                                   "};\n";

    constexpr const char* kAttributedMeter =
        "module Meter\n"
        "{\n"
        "    param float gain [[fminus::range(-24, 12)]] [[fminus::default(0)]] [[ui::unit(\"dB\")]];\n"
        "    float level [[ui::unit(\"dB\"), ui::label(\"Level\")]] [[fminus::range(-60, 0)]];\n"
        "    float history [[ui::hidden]] [2][4];\n"
        "    double sum [[gnu::aligned(8)]] [[deprecated]];\n"
        "    float tick(float x)\n"
        "    {\n"
        "        level = x * gain[0];\n"
        "        history[1][3] = level;\n"
        "        sum += level;\n"
        "        return level;\n"
        "    }\n"
        "};\n";
} // namespace

TEST_CASE("a field's attributes are the host's: the code is the same without them", "[codegen][fieldattr]")
{
    // An attribute of another namespace, or of none, means nothing to FMinus, as C23 ignores one it does not know.
    CHECK(loweredIr(kAttributedMeter) == loweredIr(kMeter));
    auto jit = compile(kAttributedMeter);
    REQUIRE(jit);
    struct Slot
    {
        const float* stream;
        float value;
    };
    struct Meter
    {
        Slot gain;
        float level;
        float history[2][4];
        double sum;
    } meter{{nullptr, 2.0f}, 0.0f, {}, 0.0};
    const auto tick = lookup<float(Meter*, float)>(*jit, "Meter::tick");
    CHECK(tick(&meter, 0.25f) == 0.5f);
    CHECK(meter.history[1][3] == 0.5f);
    CHECK(meter.sum == 0.5);

    // A name without a namespace is C's: one C23 does not have is ignored, with clang's warning (and a note when it is
    // one of FMinus's without its namespace), and the program compiles; C's standard attributes are no warning.
    std::string warnings;
    CHECK(compile("module M\n"
                  "{\n"
                  "    param float cutoff [[range(20, 20000)]] [[default(1000)]] [[fminus::default(1000)]];\n"
                  "    float level [[label(\"Level\")]] [[default(1)]] [[maybe_unused]] [[__deprecated__]] "
                  "[[nodiscard]];\n"
                  "};\n",
                  &warnings));
    CHECK(warnings == "test.fm:3:26: warning: unknown attribute 'range' ignored [-Wunknown-attributes]\n"
                      "test.fm:3:26: note: did you mean 'fminus::range'?\n"
                      "test.fm:3:47: warning: unknown attribute 'default' ignored [-Wunknown-attributes]\n"
                      "test.fm:3:47: note: did you mean 'fminus::default'?\n"
                      "test.fm:4:19: warning: unknown attribute 'label' ignored [-Wunknown-attributes]\n"
                      "test.fm:4:38: warning: unknown attribute 'default' ignored [-Wunknown-attributes]\n");
}

TEST_CASE("field attributes' errors are reported in clang's words", "[codegen][fieldattr]")
{
    CHECK(errorsOf("module Osc { float phase; };\n"
                   "module M\n"
                   "{\n"
                   "    float a [[fminus::default(1)]];\n"
                   "    float b [[fminus::range(1)]] [[fminus::range(\"0\", 1)]] [[fminus::range(2, 1)]];\n"
                   "    Osc o [[fminus::range(0, 1)]];\n"
                   "    float* p [[fminus::range(0, 1)]];\n"
                   "    bool on [[fminus::range(0, 1)]];\n"
                   "    float c [[fminus::smooth(5)]] [[clang::nonblocking]] [[clang::fast]];\n"
                   "    param float g [[fminus::default(\"1\")]] [[ui::unit(\"Hz\")]] [[fminus::label(\"G\")]];\n"
                   "    float tick(float x)\n"
                   "    {\n"
                   "        float local [[ui::unit(\"dB\")]] = x;\n"
                   "        for (int i [[ui::step(1)]] = 0; i < 2; ++i) { }\n"
                   "        return local;\n"
                   "    }\n"
                   "    void fill(float* out, int n) { parallel for (int i [[ui::step(1)]] = 0; i < n; ++i) "
                   "out[i] = 0; }\n"
                   "};\n"
                   "const float k [[ui::unit(\"dB\")]] = 1;\n")
          == "test.fm:4:15: error: 'fminus::default' attribute only applies to parameters: a field starts at 0, and a "
             "method such as init sets it\n"
             "test.fm:5:15: error: 'fminus::range' takes two numbers, a minimum and a maximum\n"
             "test.fm:5:36: error: 'fminus::range' takes two numbers, a minimum and a maximum\n"
             "test.fm:5:62: error: fminus::range's minimum must be below its maximum\n"
             "test.fm:6:13: error: 'fminus::range' attribute only applies to fields of numbers (float, double, int, a "
             "float vector and arrays of them), not 'Osc'\n"
             "test.fm:7:16: error: 'fminus::range' attribute only applies to fields of numbers (float, double, int, a "
             "float vector and arrays of them), not 'float *'\n"
             "test.fm:8:15: error: 'fminus::range' attribute only applies to fields of numbers (float, double, int, a "
             "float vector and arrays of them), not 'bool'\n"
             "test.fm:9:15: error: unknown attribute 'fminus::smooth': a field takes fminus::range(min, max)\n"
             "test.fm:9:37: error: 'clang::nonblocking' attribute only applies to functions\n"
             "test.fm:9:60: error: unknown attribute 'clang::fast': FMinus has clang::nonallocating and "
             "clang::nonblocking\n"
             "test.fm:10:21: error: 'fminus::default' takes one number\n"
             "test.fm:10:65: error: unknown attribute 'fminus::label': a parameter takes fminus::range(min, max) and "
             "fminus::default(value)\n"
             "test.fm:13:23: error: 'ui::unit' attribute only applies to a module's fields and parameters\n"
             "test.fm:14:22: error: 'ui::step' attribute only applies to a module's fields and parameters\n"
             "test.fm:17:58: error: 'ui::step' attribute only applies to a module's fields and parameters\n"
             "test.fm:19:17: error: 'ui::unit' attribute only applies to a module's fields and parameters\n");

    // A field's type is worked out first: fminus::range names it as declared, an array's with its size, and a type
    // that is an error of its own is the declaration's one error.
    CHECK(errorsOf("module Osc { float phase; };\n"
                   "module M\n"
                   "{\n"
                   "    Osc oscs [[fminus::range(0, 1)]] [4];\n"
                   "    bool flags [[fminus::range(0, 1)]] [4];\n"
                   "    void v [[fminus::range(0, 1)]];\n"
                   "    float rows [[fminus::range(0, 1)]] [];\n"
                   "};\n")
          == "test.fm:4:16: error: 'fminus::range' attribute only applies to fields of numbers (float, double, int, a "
             "float vector and arrays of them), not 'Osc[4]'\n"
             "test.fm:5:10: error: FMinus has arrays of float, double, int, float2, float4, float8 and modules, not of "
             "'bool'\n"
             "test.fm:6:10: error: field has incomplete type 'void'\n"
             "test.fm:7:11: error: field has incomplete type 'float[]'\n");
}

TEST_CASE("a host's attribute takes numbers C writes, and fminus::range those of a field's type",
          "[codegen][fieldattr]")
{
    // A hex float's f suffix follows its exponent; every number of an attribute is one C writes.
    CHECK(compile("module M\n"
                  "{\n"
                  "    float a [[ui::step(0x1p-3f, 1.5f, -2, 010, 0x1F)]] [[fminus::range(-0x1p1f, 0x1p1f)]];\n"
                  "    double d [[fminus::range(-1e300, 1e300)]];\n"
                  "    int n [[fminus::range(0, 7)]];\n"
                  "    float4 v [[fminus::range(0, 1)]];\n"
                  "    float rows [[fminus::range(0, 1)]] [2][2];\n"
                  "    enum Mode { A, B } mode [[fminus::range(0, 1)]];\n"
                  "};\n"));
    // A string is no number, even "12", in an attribute FMinus reads.
    CHECK(errorsOf("module M { param int n [[fminus::range(0, \"12\")]]; };")
          == "test.fm:1:26: error: 'fminus::range' takes two numbers, a minimum and a maximum\n");
    // An integer is one of C's integer types, the widest unsigned long long, as clang reads it: a larger one is no
    // number.
    CHECK(compile("module M { float x [[ui::step(18446744073709551615, -18446744073709551615, "
                  "9223372036854775808)]]; };"));
    CHECK(errorsOf("module M\n"
                   "{\n"
                   "    float x [[ui::step(99999999999999999999)]] [[ui::step(-18446744073709551616)]];\n"
                   "    param float g [[fminus::range(0, 0x10000000000000000)]];\n"
                   "};\n")
          == "test.fm:3:15: error: invalid number '99999999999999999999' in 'ui::step'\n"
             "test.fm:3:50: error: invalid number '-18446744073709551616' in 'ui::step'\n"
             "test.fm:4:21: error: invalid number '0x10000000000000000' in 'fminus::range'\n");
}

TEST_CASE("#if keeps or skips attributes, strings and all", "[codegen][fieldattr]")
{
    // A skipped group's strings are strings: a /* in one starts no comment, so the #endif after it ends the group.
    CHECK(compile("#if 0\n"
                  "module M { float x [[ui::label(\"/* not a comment\")]]; };\n"
                  "#endif\n"
                  "module M\n"
                  "{\n"
                  "#ifdef UNITS\n"
                  "    float x [[ui::unit(\"dB\")]];\n"
                  "#else\n"
                  "    float x [[ui::label(\"#else, // and */ in a string\")]];\n"
                  "#endif\n"
                  "};\n"));
}
