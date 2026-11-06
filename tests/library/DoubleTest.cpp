// tests/library/DoubleTest.cpp: doubles as a host meets them (C++17, FMinus::FMinus alone): a module's double fields as
// module() describes them (the type "double", 8 bytes at an offset aligned to 8), which the host reads and writes as
// C++'s double; functions of doubles called through function<>; a double's slow state, kept where a float's stalls; and
// doubles in a program compiled with the loop budget, with bounds checks (the array's type, "double[8]", in the
// message) and around #ifdef, as the safety options and the conditionals meet them.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    // An instance of a module, as a host makes one: aligned, zeroed, the module's size (FMinus.h, Module).
    struct Instance
    {
        explicit Instance(const fminus::Program::Module& module)
        {
            const std::size_t alignment = module.alignment < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                                                                       : module.alignment;
            const std::size_t size = (module.size + alignment - 1) / alignment * alignment;
#if defined(_WIN32)
            memory = _aligned_malloc(size, alignment);
#else
            memory = std::aligned_alloc(alignment, size);
#endif
            std::memset(memory, 0, size);
        }
        ~Instance()
        {
#if defined(_WIN32)
            _aligned_free(memory);
#else
            std::free(memory);
#endif
        }
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        void* memory = nullptr;
    };

    // A field's description, by name.
    const fminus::Program::Module::Field* fieldNamed(const fminus::Program::Module& module, const std::string& name)
    {
        for (const fminus::Program::Module::Field& field : module.fields)
        {
            if (field.name == name)
                return &field;
        }
        return nullptr;
    }

    // An engine's coolant, warmed a little each block: its temperature a double, and its last block's step a float.
    const char* const kCoolant = "module Coolant\n"
                                 "{\n"
                                 "    float step;\n"
                                 "    double temperature;\n"
                                 "    double history[8];\n"
                                 "    int blocks;\n"
                                 "\n"
                                 "    void process(int n) [[clang::nonblocking]]\n"
                                 "    {\n"
                                 "        step = 1e-6f * n;\n"
                                 "        temperature += 1e-6 * n;\n"
                                 "        history[blocks % 8] = temperature;\n"
                                 "        blocks++;\n"
                                 "    }\n"
                                 "};\n"
                                 "double celsius(double kelvin) { return kelvin - 273.15; }\n";
} // namespace

TEST_CASE("a module's double fields are described as doubles, and read and written as C++'s", "[library][double]")
{
    std::string errors;
    auto program = fminus::Program::compile(kCoolant, errors);
    INFO(errors);
    REQUIRE(program);
    const fminus::Program::Module* coolant = program->module("Coolant");
    REQUIRE(coolant != nullptr);
    CHECK(coolant->alignment == 8);
    const auto* step = fieldNamed(*coolant, "step");
    const auto* temperature = fieldNamed(*coolant, "temperature");
    const auto* history = fieldNamed(*coolant, "history");
    REQUIRE(step != nullptr);
    REQUIRE(temperature != nullptr);
    REQUIRE(history != nullptr);
    CHECK(step->type == "float");
    CHECK(step->size == 4);
    CHECK(temperature->type == "double");
    CHECK(temperature->offset == 8);   // aligned to 8, after the float
    CHECK(temperature->size == 8);
    CHECK(history->type == "double[8]");
    CHECK(history->size == 64);

    Instance instance(*coolant);
    auto* process = program->function<void(void*, int)>("Coolant::process", errors);
    REQUIRE(process != nullptr);
    double expected = 90;
    std::memcpy(static_cast<char*>(instance.memory) + temperature->offset, &expected, sizeof expected);
    for (int block = 0; block < 1000; ++block)
    {
        process(instance.memory, 64);
        expected += 1e-6 * 64;
    }
    double read = 0;
    std::memcpy(&read, static_cast<char*>(instance.memory) + temperature->offset, sizeof read);
    CHECK(read == expected);   // a float at 90 would never have moved: 64e-6 is under half its ulp there
    double last = 0;
    std::memcpy(&last, static_cast<char*>(instance.memory) + history->offset + 7 * sizeof(double), sizeof last);
    CHECK(last == read);   // block 999 wrote history[7]

    auto* celsius = program->function<double(double)>("celsius", errors);
    REQUIRE(celsius != nullptr);
    CHECK(celsius(300.0) == 300.0 - 273.15);
}

TEST_CASE("doubles meet the loop budget, bounds checks and the conditionals", "[library][double]")
{
    fminus::Program::Options options;
    options.loopBudget = true;
    options.boundsCheck = true;
    options.defines["WIDE"] = "1";
    const char* const source = "#ifdef WIDE\n"
                               "double accumulate(int n)\n"
                               "{\n"
                               "    double sum = 0;\n"
                               "    for (int i = 0; i < n; ++i)\n"
                               "        sum += 0.1;\n"
                               "    return sum;\n"
                               "}\n"
                               "#else\n"
                               "float accumulate(int n) { return n; }\n"
                               "#endif\n"
                               "double spin(double x) { while (x > 0) x = x * 1.0000001 + 1; return x; }\n"
                               "double tap(int i) { double taps[8] = {0.5, 0.25}; return taps[i]; }\n";
    std::string errors;
    auto program = fminus::Program::compile(source, errors, options);
    INFO(errors);
    REQUIRE(program);

    auto* accumulate = program->function<double(int)>("accumulate", errors);
    REQUIRE(accumulate != nullptr);
    program->setLoopBudget(1'000'000);
    double sum = 0;
    for (int i = 0; i < 1000; ++i)
        sum += 0.1;
    CHECK(accumulate(1000) == sum);
    CHECK_FALSE(program->loopBudgetExceeded());

    auto* spin = program->function<double(double)>("spin", errors);
    REQUIRE(spin != nullptr);
    program->setLoopBudget(1000);
    spin(1.0);   // for ever, but for the budget
    CHECK(program->loopBudgetExceeded());

    auto* tap = program->function<double(int)>("tap", errors);
    REQUIRE(tap != nullptr);
    program->setLoopBudget(1000);
    CHECK(tap(1) == 0.25);
    CHECK_FALSE(program->boundsViolation().has_value());
    tap(8);
    const std::optional<fminus::Program::BoundsViolation> violation = program->boundsViolation();
    REQUIRE(violation.has_value());
    CHECK(std::string(violation->type) == "double[8]");
    CHECK(violation->index == 8);
    CHECK(violation->message() == "program.fm:13:62: index 8 out of bounds for 'taps' of type 'double[8]'");
    program->clearBoundsViolation();
}
