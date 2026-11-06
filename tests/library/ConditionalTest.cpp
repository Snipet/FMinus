// tests/library/ConditionalTest.cpp: Program::Options::defines, a host's macros for a program's #if, #ifdef and
// #ifndef: instrumentation compiled in when the host defines DEBUG, and skipped without it. C++17, as a host builds.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace
{
    // A meter whose peak is the instrumentation: a field and a statement, both compiled out without DEBUG, and the
    // statement without LEVEL 2 or more, the program's default unless the host defines LEVEL.
    constexpr const char* kMeter = "#ifndef LEVEL\n"
                                   "#define LEVEL 2\n"
                                   "#endif\n"
                                   "module Meter\n"
                                   "{\n"
                                   "    float y;\n"
                                   "#ifdef DEBUG\n"
                                   "    float peak;   // the loudest |x| so far\n"
                                   "#endif\n"
                                   "    float tick(float x)\n"
                                   "    {\n"
                                   "        y = 0.5f * x;\n"
                                   "#if defined(DEBUG) && LEVEL >= 2\n"
                                   "        peak = fmaxf(peak, fabsf(x));\n"
                                   "#endif\n"
                                   "        return y;\n"
                                   "    }\n"
                                   "};\n"
                                   "float run(float x)\n"
                                   "{\n"
                                   "    Meter m;\n"
                                   "    m.tick(x);\n"
                                   "    m.tick(-2.0f * x);\n"
                                   "#ifdef DEBUG\n"
                                   "    return m.peak;\n"
                                   "#else\n"
                                   "    return m.y;\n"
                                   "#endif\n"
                                   "}\n";

    std::unique_ptr<fminus::Program> compiled(const fminus::Program::Options& options, std::string& errors)
    {
        return fminus::Program::compile(kMeter, errors, options);
    }
} // namespace

TEST_CASE("Options::defines compiles a program's instrumentation in, or skips it", "[library][conditional]")
{
    std::string errors;
    fminus::Program::Options options;
    const auto plain = compiled(options, errors);
    INFO(errors);
    REQUIRE(plain);
    auto* run = plain->function<float(float)>("run", errors);
    REQUIRE(run != nullptr);
    CHECK(run(3.0f) == -3.0f);   // y: half of -6
    const fminus::Program::Module* meter = plain->module("Meter");
    REQUIRE(meter != nullptr);
    CHECK(meter->fields.size() == 1);

    options.defines["DEBUG"] = "1";
    const auto debug = compiled(options, errors);
    REQUIRE(debug);
    run = debug->function<float(float)>("run", errors);
    REQUIRE(run != nullptr);
    CHECK(run(3.0f) == 6.0f);   // peak: |-6|
    meter = debug->module("Meter");
    REQUIRE(meter != nullptr);
    REQUIRE(meter->fields.size() == 2);
    CHECK(meter->fields[1].name == "peak");

    // The host's LEVEL comes first: the program's #ifndef keeps it.
    options.defines["LEVEL"] = "1";
    const auto quiet = compiled(options, errors);
    REQUIRE(quiet);
    run = quiet->function<float(float)>("run", errors);
    REQUIRE(run != nullptr);
    CHECK(run(3.0f) == 0.0f);   // the field is there, never written
}

TEST_CASE("Options::defines: a name that cannot be a macro, and a macro's name in code, are errors",
          "[library][conditional]")
{
    std::string errors;
    fminus::Program::Options options;
    options.defines["1X"] = "";
    CHECK(fminus::Program::compile("float f() { return 1; }\n", errors, options) == nullptr);
    CHECK(errors == "fminus::Program: defines: '1X': macro name must be an identifier\n");
    options.defines.clear();
    options.defines["float"] = "1";
    CHECK(fminus::Program::compile("float f() { return 1; }\n", errors, options) == nullptr);
    CHECK(errors == "fminus::Program: defines: 'float': 'float' cannot be used as a macro name: it is one of FMinus's "
                    "keywords\n");
    options.defines.clear();
    options.defines["GAIN"] = "2";
    CHECK(fminus::Program::compile("float f(float x) { return GAIN * x; }\n", errors, options) == nullptr);
    CHECK(errors == "program.fm:1:27: error: macro expansion is not supported: 'GAIN' is a macro, which FMinus reads "
                    "only in #if, #elif, #ifdef and #ifndef (for a constant, declare an enum or a const)\n");
}
