// tests/unit/HashTest.cpp: hash and randf (extending.md, recipe 25; src/ir/Hash.cpp): fixed values for fixed inputs,
// the C++ reference's (lowbias32, and its top 24 bits over 2^23) for every input tried; the same bits in every math
// mode, at -O0 and -O2, with subnormals flushed or not, in every lane of a vectorised loop and its remainder, whatever
// the block size; a crude look at the distribution; and the builtins' errors.
#include "Compile.h"

#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

using namespace fminus::test;

namespace
{
    // What hash computes: Chris Wellons's lowbias32 on the int's bits.
    int hashOf(int n)
    {
        auto x = static_cast<std::uint32_t>(n);
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return static_cast<int>(x);
    }

    // What randf computes: the hash's top 24 bits, signed, over 2^23.
    float randfOf(int n)
    {
        return static_cast<float>(hashOf(n) >> 8) * 0x1p-23f;
    }

    constexpr int kMin = std::numeric_limits<int>::min();
    constexpr int kMax = std::numeric_limits<int>::max();

    // White noise from a counter, as a loop that vectorises; and as a module whose field counts, a call at a time.
    constexpr std::string_view kNoise = "void white(float* restrict out, int n, int start) [[clang::nonblocking]]\n"
                                        "{\n"
                                        "    for (int i = 0; i < n; ++i)\n"
                                        "        out[i] = randf(start + i);\n"
                                        "}\n"
                                        "void hashes(int* restrict out, int n, int start)\n"
                                        "{\n"
                                        "    for (int i = 0; i < n; ++i)\n"
                                        "        out[i] = hash(start + i);\n"
                                        "}\n"
                                        "module Noise\n"
                                        "{\n"
                                        "    int count;\n"
                                        "    void render(float* restrict out, int n) [[clang::nonblocking]]\n"
                                        "    {\n"
                                        "        for (int i = 0; i < n; ++i)\n"
                                        "            out[i] = randf(count++);\n"
                                        "    }\n"
                                        "};\n";

    // The index of the first sample whose bits differ from the reference's, or -1.
    long firstDifference(const std::vector<float>& got, int start)
    {
        for (std::size_t i = 0; i < got.size(); ++i)
        {
            const float expected = randfOf(start + static_cast<int>(i));
            if (std::bit_cast<std::uint32_t>(got[i]) != std::bit_cast<std::uint32_t>(expected))
                return static_cast<long>(i);
        }
        return -1;
    }
} // namespace

TEST_CASE("hash and randf give fixed values for fixed inputs, the reference's", "[codegen][hash]")
{
    std::string errors;
    auto jit = compile("int h(int n) { return hash(n); }\n"
                       "float r(int n) { return randf(n); }\n"
                       // Constants, which the code generator folds: the same values.
                       "int h12345() { return hash(12345); }\n"
                       "float r12345() { return randf(12345); }\n"
                       "float rMin() { return randf(-2147483647 - 1); }\n"
                       "int converted(float x) { return hash(x); }\n"   // a float is truncated, as for an int parameter
                       "int fromBool(bool b) { return hash(b); }\n",
                       &errors);
    INFO(errors);
    REQUIRE(jit);
    auto h = lookup<int(int)>(*jit, "h");
    auto r = lookup<float(int)>(*jit, "r");
    // Golden values: if these change, every program's noise changes.
    CHECK(h(0) == 0);
    CHECK(h(1) == 1753845952);
    CHECK(h(2) == -787275391);
    CHECK(h(3) == 1408362973);
    CHECK(h(-1) == 1734902346);
    CHECK(h(kMax) == -1926627400);
    CHECK(h(kMin) == -867483356);
    CHECK(h(12345) == -1859191561);
    CHECK(h(48000) == -1144637302);
    CHECK(std::bit_cast<std::uint32_t>(r(0)) == 0x00000000u);
    CHECK(std::bit_cast<std::uint32_t>(r(1)) == 0x3f511320u);   // 0.816698074
    CHECK(std::bit_cast<std::uint32_t>(r(2)) == 0xbebbb37cu);   // -0.366603732
    CHECK(std::bit_cast<std::uint32_t>(r(12345)) == 0xbf5da208u);
    CHECK(std::bit_cast<std::uint32_t>(r(kMin)) == 0xbeced2fcu);
    CHECK(lookup<int()>(*jit, "h12345")() == -1859191561);
    CHECK(std::bit_cast<std::uint32_t>(lookup<float()>(*jit, "r12345")()) == 0xbf5da208u);
    CHECK(std::bit_cast<std::uint32_t>(lookup<float()>(*jit, "rMin")()) == 0xbeced2fcu);
    CHECK(lookup<int(float)>(*jit, "converted")(2.9f) == h(2));
    CHECK(lookup<int(float)>(*jit, "converted")(-2.9f) == h(-2));
    CHECK(lookup<int(bool)>(*jit, "fromBool")(true) == h(1));
    for (int n = -100000; n <= 100000; n += 7)
    {
        INFO("n = " << n);
        REQUIRE(h(n) == hashOf(n));
        REQUIRE(std::bit_cast<std::uint32_t>(r(n)) == std::bit_cast<std::uint32_t>(randfOf(n)));
    }
}

TEST_CASE("randf gives the same bits in every math mode, at -O0 and -O2, flushed or not, in every lane",
          "[codegen][hash]")
{
    using fminus::Program;
    using Render = void(void*, float*, int);
    for (const Program::Math math : {Program::Math::Vector, Program::Math::Libm, Program::Math::Fast})
    {
        for (const int level : {0, 2})
        {
            for (const Program::Denormals denormals : {Program::Denormals::Preserve, Program::Denormals::Flush})
            {
                INFO("math mode " << static_cast<int>(math) << ", -O" << level << ", "
                                  << (denormals == Program::Denormals::Flush ? "flushed" : "preserved"));
                Program::Options options;
                options.math = math;
                options.optimisation = level;
                options.denormals = denormals;
                std::string errors;
                const auto program = Program::compile(kNoise, errors, options);
                INFO(errors);
                REQUIRE(program);
                auto* white = program->function<void(float*, int, int)>("white", errors);
                auto* hashes = program->function<void(int*, int, int)>("hashes", errors);
                auto* render = program->function<Render>("Noise::render", errors);
                REQUIRE(white != nullptr);
                REQUIRE(hashes != nullptr);
                REQUIRE(render != nullptr);
                // 1027 samples: the vectorised body's and the remainder's, across 0.
                const int start = -500;
                std::vector<float> out(1027);
                white(out.data(), static_cast<int>(out.size()), start);
                CHECK(firstDifference(out, start) == -1);
                std::vector<int> bits(1027);
                hashes(bits.data(), static_cast<int>(bits.size()), start);
                for (std::size_t i = 0; i < bits.size(); ++i)
                    REQUIRE(bits[i] == hashOf(start + static_cast<int>(i)));
                // The module's counter, in blocks of 1, 3, 64 and 959 samples: one stream from 0, whatever the blocks.
                int count = 0;   // Noise's one field
                std::vector<float> stream;
                for (const int block : {1, 3, 64, 959})
                {
                    std::vector<float> part(static_cast<std::size_t>(block));
                    render(&count, part.data(), block);
                    stream.insert(stream.end(), part.begin(), part.end());
                }
                CHECK(count == 1027);
                CHECK(firstDifference(stream, 0) == -1);
            }
        }
    }
}

TEST_CASE("randf is uniform in [-1, 1), and hash's bits are each set half the time", "[codegen][hash]")
{
    std::string errors;
    auto jit = compile(kNoise, &errors);
    INFO(errors);
    REQUIRE(jit);
    constexpr int n = 1000000;
    std::vector<float> noise(n);
    lookup<void(float*, int, int)>(*jit, "white")(noise.data(), n, 0);
    double sum = 0;
    double squares = 0;
    float low = 1;
    float high = -1;
    for (const float x : noise)
    {
        const auto wide = static_cast<double>(x);
        sum += wide;
        squares += wide * wide;
        low = std::fmin(low, x);
        high = std::fmax(high, x);
    }
    const double mean = sum / n;
    const double variance = squares / n - mean * mean;
    INFO("mean " << mean << ", variance " << variance << ", from " << low << " to " << high);
    CHECK(low >= -1.0f);
    CHECK(high < 1.0f);
    CHECK(low < -0.9999f);
    CHECK(high > 0.9999f);
    CHECK(std::fabs(mean) < 0.005);                 // its standard error is 0.00058
    CHECK(std::fabs(variance - 1.0 / 3.0) < 0.005);   // a uniform [-1, 1)'s is 1/3
    std::vector<int> hashes(n);
    lookup<void(int*, int, int)>(*jit, "hashes")(hashes.data(), n, 0);
    for (int bit = 0; bit < 32; ++bit)
    {
        int set = 0;
        for (const int h : hashes)
            set += (static_cast<std::uint32_t>(h) >> bit) & 1u;
        INFO("bit " << bit << " set " << set << " times");
        CHECK(std::abs(set - n / 2) < n / 100);
    }
}

TEST_CASE("hash and randf take one int, and are builtins a program cannot declare", "[codegen][hash]")
{
    CHECK(errorsOf("float f(float* p, float4 v)\n"
                   "{\n"
                   "    float a = randf(1, 2);\n"
                   "    int b = hash();\n"
                   "    int c = hash(p);\n"
                   "    float d = randf(v);\n"
                   "    return a + b + c + d;\n"
                   "}\n"
                   "int hash(int x) { return x; }\n"
                   "float randf(int n);\n")
          == "test.fm:3:15: error: too many arguments to function call, expected 1, have 2\n"
             "test.fm:4:13: error: too few arguments to function call, expected 1, have 0\n"
             "test.fm:5:13: error: passing 'float *' to parameter of incompatible type 'int'\n"
             "test.fm:6:15: error: passing 'float4' to parameter of incompatible type 'int'\n"
             "test.fm:9:5: error: 'hash' is a builtin and cannot be redefined\n"
             "test.fm:10:7: error: 'randf' is a builtin and cannot be redefined\n");
}
