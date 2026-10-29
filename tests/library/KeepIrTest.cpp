// tests/library/KeepIrTest.cpp: Program::Options::keepIr, as a host sets it: the program's LLVM IR as text, as the
// code generator wrote it and as optimised, for a host to show what FMinus made of a program. tests/unit/KeepIrTest.cpp
// checks the texts against the code generator's own module and the fminus driver's --emit ll.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace
{
    // A module's method that reads a parameter into a local, and a plain function.
    const char* const kDrive = "module Drive\n"
                               "{\n"
                               "    param float drive;\n"
                               "\n"
                               "    float tick(float x)\n"
                               "    {\n"
                               "        float level = drive[0];\n"
                               "        return x * (level + 1.0f);\n"
                               "    }\n"
                               "};\n"
                               "\n"
                               "float twice(float x) { return x + x; }\n";

    bool has(const std::string& text, const char* part)
    {
        return text.find(part) != std::string::npos;
    }

    // The function `name`'s definition in a module's text: from its `define` line to the `}` that ends it.
    std::string definition(const std::string& text, const char* name)
    {
        const std::size_t start = text.find(std::string("define float @") + name + "(");
        if (start == std::string::npos)
            return {};
        const std::size_t end = text.find("\n}\n", start);
        return text.substr(start, end == std::string::npos ? std::string::npos : end + 3 - start);
    }

    std::unique_ptr<fminus::Program> compiled(const fminus::Program::Options& options)
    {
        std::string errors;
        auto program = fminus::Program::compile(kDrive, errors, options);
        INFO(errors);
        REQUIRE(program);
        return program;
    }
} // namespace

TEST_CASE("Options::keepIr is off by default: a Program keeps no IR", "[library][ir]")
{
    CHECK(fminus::Program::Options{}.keepIr == fminus::Program::KeepIr::None);
    auto program = compiled({});
    CHECK(program->loweredIr().empty());
    CHECK(program->optimisedIr().empty());
    std::string errors;
    CHECK(program->function<float(float)>("twice", errors)(21.0f) == 42.0f);
    CHECK(program->loweredIr().empty());   // a look-up keeps none either
    CHECK(program->optimisedIr().empty());
}

TEST_CASE("Options::keepIr keeps the program's IR as lowered and as optimised, and the program runs from it",
          "[library][ir]")
{
    fminus::Program::Options options;
    options.keepIr = fminus::Program::KeepIr::Both;
    auto program = compiled(options);
    const std::string& lowered = program->loweredIr();
    const std::string& optimised = program->optimisedIr();
    INFO(lowered);
    INFO(optimised);
    for (const std::string* text : {&lowered, &optimised})
    {
        CHECK(has(*text, "define float @twice(float %x)"));
        CHECK(has(*text, "define float @\"Drive::tick\"(ptr "));   // a method: its instance, then its parameters
        CHECK(has(*text, "target datalayout = \""));               // set up for this machine
        CHECK(has(*text, "target triple = \""));
    }
    // The code generator keeps every variable in a stack slot, and LLVM's pipeline makes them SSA values.
    CHECK(has(lowered, "alloca"));
    CHECK(!has(optimised, "alloca"));
    CHECK(lowered != optimised);

    // The JIT compiles the optimised module: the program runs as it does without the option.
    std::string errors;
    auto* twice = program->function<float(float)>("twice", errors);
    INFO(errors);
    REQUIRE(twice != nullptr);
    CHECK(twice(21.0f) == 42.0f);

    // Each text alone is the same text.
    options.keepIr = fminus::Program::KeepIr::Lowered;
    auto loweredOnly = compiled(options);
    CHECK(loweredOnly->loweredIr() == lowered);
    CHECK(loweredOnly->optimisedIr().empty());
    CHECK(loweredOnly->function<float(float)>("twice", errors)(1.5f) == 3.0f);
    options.keepIr = fminus::Program::KeepIr::Optimised;
    auto optimisedOnly = compiled(options);
    CHECK(optimisedOnly->loweredIr().empty());
    CHECK(optimisedOnly->optimisedIr() == optimised);
    CHECK(optimisedOnly->function<float(float)>("twice", errors)(1.5f) == 3.0f);
}

TEST_CASE("Options::keepIr: a frozen parameter's value is folded into the optimised IR", "[library][ir][param]")
{
    fminus::Program::Options options;
    options.keepIr = fminus::Program::KeepIr::Both;
    options.parameters["Drive::drive"] = fminus::Program::Binding::frozen(2.0);
    auto frozen = compiled(options);
    const std::string lowered = definition(frozen->loweredIr(), "\"Drive::tick\"");
    const std::string optimised = definition(frozen->optimisedIr(), "\"Drive::tick\"");
    INFO(lowered);
    INFO(optimised);
    // As written, the value goes into the local and the sum is computed; optimised, x is multiplied by 3.
    CHECK(has(lowered, "store float 2.000000e+00, ptr %level,"));
    CHECK(has(lowered, "fadd float"));
    CHECK(!has(lowered, "3.000000e+00"));
    CHECK(has(optimised, "fmul float %x, 3.000000e+00"));
    CHECK(!has(optimised, "fadd"));
    CHECK(!has(optimised, "load"));

    // The method reads no slot: any instance's memory, zeroed, does.
    const fminus::Program::Module* drive = frozen->module("Drive");
    REQUIRE(drive != nullptr);
    alignas(16) unsigned char instance[64] = {};
    REQUIRE(drive->size <= sizeof(instance));
    REQUIRE(drive->alignment <= 16);
    std::string errors;
    auto* tick = frozen->function<float(void*, float)>("Drive::tick", errors);
    INFO(errors);
    REQUIRE(tick != nullptr);
    CHECK(tick(instance, 5.0f) == 15.0f);

    // A knob is loaded from its slot when the method starts, and nothing is folded.
    options.parameters.clear();
    auto knob = compiled(options);
    const std::string knobbed = definition(knob->optimisedIr(), "\"Drive::tick\"");
    INFO(knobbed);
    CHECK(has(knobbed, "load atomic float"));
    CHECK(!has(knobbed, "3.000000e+00"));
}

TEST_CASE("Options::keepIr: the same source and options give the same text, and each level its own", "[library][ir]")
{
    fminus::Program::Options options;
    options.keepIr = fminus::Program::KeepIr::Both;
    auto first = compiled(options);
    auto second = compiled(options);
    CHECK(first->loweredIr() == second->loweredIr());
    CHECK(first->optimisedIr() == second->optimisedIr());

    // -O0's pipeline keeps the stack slots; the code generator writes the same module at every level.
    options.optimisation = 0;
    auto unoptimised = compiled(options);
    CHECK(unoptimised->loweredIr() == first->loweredIr());
    CHECK(has(unoptimised->optimisedIr(), "alloca"));
    CHECK(unoptimised->optimisedIr() != first->optimisedIr());
}
