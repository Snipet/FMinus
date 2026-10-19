// tests/library/ProgramTest.cpp: the public API, include/fminus/FMinus.h, as a host uses it. Built in C++17 against
// FMinus::FMinus alone (tests/CMakeLists.txt), without LLVM's headers or FMinus's internals, so it also checks that the
// header asks for nothing more.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // FMinus's float4 at the C boundary: clang's ext_vector_type(4).
    using float4 = float __attribute__((ext_vector_type(4)));

    // A block kernel, as a plugin calls one: the loop over the samples is FMinus's.
    const char* const kGain = "void gain(const float* restrict in, float* restrict out, int n, float g)\n"
                              "    [[clang::nonblocking]]\n"
                              "{\n"
                              "    for (int i = 0; i < n; ++i)\n"
                              "        out[i] = g * in[i];\n"
                              "}\n";

    using Gain = void(const float*, float*, int, float);
} // namespace

TEST_CASE("a Program compiles FMinus source and gives its functions as C function pointers", "[library]")
{
    std::string errors = "left over";
    auto program = fminus::Program::compile(kGain, errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(errors.empty());
    Gain* gain = program->function<Gain>("gain", errors);
    REQUIRE(gain != nullptr);
    const std::array<float, 5> in{1.0f, -2.0f, 3.0f, -4.0f, 5.0f};
    std::array<float, 5> out{};
    gain(in.data(), out.data(), 5, 0.5f);
    CHECK(out == std::array<float, 5>{0.5f, -1.0f, 1.5f, -2.0f, 2.5f});
    CHECK(program->address("gain", errors) == reinterpret_cast<std::uintptr_t>(gain));
    CHECK(std::string(FMINUS_VERSION_STRING).size() >= 5);   // Config.h comes with the header
}

TEST_CASE("a Program's functions take and return C's types", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile("float twice(float x) { return 2.0f * x; }\n"
                                            "int rest(int a, int b) { return a % b; }\n"
                                            "bool positive(float x) { return x > 0.0f; }\n"
                                            "float4 sum(float4 a, float4 b) { return a + b; }\n"
                                            "float* ramp(int n)\n"
                                            "{\n"
                                            "    float* p = calloc(n, sizeof(float));\n"
                                            "    for (int i = 0; i < n; ++i)\n"
                                            "        p[i] = i;\n"
                                            "    return p;\n"
                                            "}\n"
                                            "void release(void* p) { free(p); }\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->function<float(float)>("twice", errors)(1.5f) == 3.0f);
    CHECK(program->function<int(int, int)>("rest", errors)(-7, 3) == -1);   // C's: the sign of the dividend
    auto* positive = program->function<bool(float)>("positive", errors);
    CHECK(positive(0.25f));
    CHECK(!positive(-0.25f));
    auto* sum = program->function<float4(float4, float4)>("sum", errors);
    const float4 s = sum(float4{1, 2, 3, 4}, float4{10, 20, 30, 40});
    CHECK((s.x == 11.0f && s.y == 22.0f && s.z == 33.0f && s.w == 44.0f));
    float* p = program->function<float*(int)>("ramp", errors)(64);   // FMinus's heap: the runtime is linked
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    CHECK((p[0] == 0.0f && p[63] == 63.0f));
    program->function<void(void*)>("release", errors)(p);
}

TEST_CASE("a Program that does not compile gives FMinus's messages, as clang words them", "[library]")
{
    std::string errors;
    fminus::Program::Options options;
    options.fileName = "host.fm";
    CHECK(fminus::Program::compile("float f(float x) { return y; }\n", errors, options) == nullptr);
    CHECK(errors == "host.fm:1:27: error: use of undeclared identifier 'y'\n");

    CHECK(fminus::Program::compile("float f(float x) { return x }\n", errors) == nullptr);   // a syntax error
    CHECK(errors.rfind("program.fm:1:", 0) == 0);

    // A function that could reach the heap, marked nonblocking: refused, with clang's notes down to the call.
    CHECK(fminus::Program::compile("float* make(int n) { return calloc(n, sizeof(float)); }\n"
                                   "void run(float* p, int n) [[clang::nonblocking]] { p = make(n); }\n",
                                   errors)
          == nullptr);
    CHECK(errors.find("error:") != std::string::npos);
    CHECK(errors.find("nonblocking") != std::string::npos);
    CHECK(errors.find("note:") != std::string::npos);
}

TEST_CASE("a Program's function that does not exist is a null pointer and an error", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile(kGain, errors);
    REQUIRE(program);
    CHECK(program->function<Gain>("missing", errors) == nullptr);
    CHECK(errors.find("missing") != std::string::npos);
    CHECK(program->address("gain", errors) != 0);   // the program goes on working
    CHECK(errors.empty());
}

TEST_CASE("a Program says which functions are nonblocking and nonallocating", "[library]")
{
    std::string errors;
    auto program = fminus::Program::compile("float* make(int n) { return calloc(n, sizeof(float)); }\n"
                                            "void scale(float* p, int n, float g) [[clang::nonallocating]]\n"
                                            "{\n"
                                            "    for (int i = 0; i < n; ++i)\n"
                                            "        p[i] = g * p[i];\n"
                                            "}\n"
                                            "void run(float* p, int n) [[clang::nonblocking]];\n"   // a prototype
                                            "void run(float* p, int n) { scale(p, n, 0.5f); }\n",
                                            errors);
    INFO(errors);
    REQUIRE(program);
    CHECK(program->nonBlocking("run"));
    CHECK(program->nonAllocating("run"));   // nonblocking includes it
    CHECK(!program->nonBlocking("scale"));
    CHECK(program->nonAllocating("scale"));
    CHECK(!program->nonBlocking("make"));
    CHECK(!program->nonAllocating("make"));
    CHECK(!program->nonBlocking("missing"));
}

TEST_CASE("a Program's options: the optimisation level, a poly float's lanes, and out-of-range values refused",
          "[library]")
{
    std::string errors;
    const std::array<float, 3> in{1.0f, 2.0f, 3.0f};
    for (int level = 0; level <= 3; ++level)
    {
        fminus::Program::Options options;
        options.optimisation = level;
        auto program = fminus::Program::compile(kGain, errors, options);
        INFO("-O" << level << ": " << errors);
        REQUIRE(program);
        std::array<float, 3> out{};
        program->function<Gain>("gain", errors)(in.data(), out.data(), 3, 2.0f);
        CHECK(out == std::array<float, 3>{2.0f, 4.0f, 6.0f});
    }

    auto native = fminus::Program::compile(kGain, errors);
    REQUIRE(native);
    const unsigned lanes = native->polyLanes();   // this machine's: 4 with NEON or SSE
    CHECK((lanes >= 1 && lanes <= 64 && (lanes & (lanes - 1)) == 0));
    fminus::Program::Options eight;
    eight.polyLanes = 8;
    auto wide = fminus::Program::compile("float lanes() { return poly_lanes(); }\n", errors, eight);
    INFO(errors);
    REQUIRE(wide);
    CHECK(wide->polyLanes() == 8);
    CHECK(wide->function<float()>("lanes", errors)() == 8.0f);   // the program's poly floats have 8 lanes

    fminus::Program::Options bad;
    bad.optimisation = 4;
    CHECK(fminus::Program::compile(kGain, errors, bad) == nullptr);
    CHECK(errors == "fminus::Program: optimisation 4 is not a level from 0 to 3\n");
    bad.optimisation = 2;
    bad.polyLanes = 3;
    CHECK(fminus::Program::compile(kGain, errors, bad) == nullptr);
    CHECK(errors == "fminus::Program: polyLanes 3 is not 0 (this machine's) or a power of two from 1 to 64\n");
}

TEST_CASE("Programs compile, look up and run on several threads at once", "[library]")
{
    constexpr int kThreads = 4;
    std::array<float, kThreads> results{};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t, &results] {
            std::string errors;
            auto program = fminus::Program::compile(kGain, errors);
            Gain* gain = program ? program->function<Gain>("gain", errors) : nullptr;
            if (gain == nullptr)
                return;
            const float in = 3.0f;
            float out = 0.0f;
            gain(&in, &out, 1, static_cast<float>(t + 1));
            results[static_cast<std::size_t>(t)] = out;
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    CHECK(results == std::array<float, kThreads>{3.0f, 6.0f, 9.0f, 12.0f});
}
