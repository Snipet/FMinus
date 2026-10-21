// tests/unit/FftTest.cpp: the FFT (extending.md, recipe 13): plans made off the audio thread; real and complex
// transforms against a double-precision DFT, and their round trips, unnormalised; null plans, and transforms that do
// nothing on them; the effects and the errors, run through the JIT.
#include "Compile.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

using namespace fminus::test;

namespace
{
    // The transforms, as a program calls them: plans off the audio thread, transforms on it.
    constexpr const char* kProgram =
        "void* real_plan(int n) { return fft_create(n); }\n"
        "void* complex_plan(int n) { return fft_create_complex(n); }\n"
        "void release(void* plan) { fft_destroy(plan); }\n"
        "void forward(void* p, const float* in, float2* out) [[clang::nonblocking]] { fft_forward(p, in, out); }\n"
        "void inverse(void* p, const float2* in, float* out) [[clang::nonblocking]] { fft_inverse(p, in, out); }\n"
        "void cforward(void* p, const float2* in, float2* out) [[clang::nonblocking]] { fft_forward(p, in, out); }\n"
        "void cinverse(void* p, const float2* in, float2* out) [[clang::nonblocking]] { fft_inverse(p, in, out); }\n";

    using Complex = std::complex<double>;

    // The DFT, in double: X[k] = sum x[j] e^(-2 pi i j k / n) forward, with a + in the exponent for the inverse.
    std::vector<Complex> dft(const std::vector<Complex>& x, bool inverse)
    {
        const std::size_t n = x.size();
        std::vector<Complex> y(n);
        const double sign = inverse ? 1.0 : -1.0;
        for (std::size_t k = 0; k < n; ++k)
        {
            Complex sum = 0.0;
            for (std::size_t j = 0; j < n; ++j)
                sum += x[j] * std::polar(1.0, sign * 2.0 * std::numbers::pi * static_cast<double>((j * k) % n)
                                                  / static_cast<double>(n));
            y[k] = sum;
        }
        return y;
    }

    // Uniform floats in [-1, 1), from a fixed LCG.
    std::vector<float> noise(std::size_t count, std::uint32_t seed)
    {
        std::vector<float> x(count);
        for (float& v : x)
        {
            seed = seed * 1664525u + 1013904223u;
            v = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        }
        return x;
    }

    // The largest difference between `got` and `want`, relative to the largest value of `want`.
    double relativeError(const std::vector<Complex>& got, const std::vector<Complex>& want)
    {
        double error = 0.0;
        double scale = 0.0;
        for (std::size_t i = 0; i < want.size(); ++i)
        {
            error = std::max(error, std::abs(got[i] - want[i]));
            scale = std::max(scale, std::abs(want[i]));
        }
        return error / scale;
    }

    std::vector<Complex> complexOf(const std::vector<float2>& v)
    {
        std::vector<Complex> c;
        for (const float2& z : v)
            c.emplace_back(z.x, z.y);
        return c;
    }
} // namespace

TEST_CASE("a real FFT is the DFT of its input, in n/2 + 1 bins, and its inverse gives n times the input",
          "[codegen][fft]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    auto plan = lookup<void*(int)>(*jit, "real_plan");
    auto forward = lookup<void(void*, const float*, float2*)>(*jit, "forward");
    auto inverse = lookup<void(void*, const float2*, float*)>(*jit, "inverse");
    for (const int n : {64, 1024})
    {
        INFO("n = " << n);
        void* p = plan(n);
        REQUIRE(p != nullptr);
        const std::vector<float> x = noise(static_cast<std::size_t>(n), 7u);
        std::vector<float2> bins(static_cast<std::size_t>(n / 2 + 1));
        forward(p, x.data(), bins.data());
        std::vector<Complex> reference = dft(std::vector<Complex>(x.begin(), x.end()), false);
        reference.resize(bins.size());
        CHECK(relativeError(complexOf(bins), reference) < 2e-6);
        CHECK(bins.front().y == 0.0f);   // DC and Nyquist are real
        CHECK(bins.back().y == 0.0f);

        std::vector<float> back(static_cast<std::size_t>(n));
        inverse(p, bins.data(), back.data());
        std::vector<Complex> scaled;
        for (const float v : x)
            scaled.emplace_back(static_cast<double>(n) * static_cast<double>(v), 0.0);
        CHECK(relativeError(std::vector<Complex>(back.begin(), back.end()), scaled) < 2e-6);
        lookup<void(void*)>(*jit, "release")(p);
    }
}

TEST_CASE("a complex FFT is the DFT of its input, and its inverse gives n times the input", "[codegen][fft]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    auto forward = lookup<void(void*, const float2*, float2*)>(*jit, "cforward");
    auto inverse = lookup<void(void*, const float2*, float2*)>(*jit, "cinverse");
    for (const int n : {64, 1024})
    {
        INFO("n = " << n);
        void* p = lookup<void*(int)>(*jit, "complex_plan")(n);
        REQUIRE(p != nullptr);
        const std::vector<float> values = noise(2 * static_cast<std::size_t>(n), 11u);
        std::vector<float2> x(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = float2{values[2 * i], values[2 * i + 1]};
        std::vector<float2> y(x.size());
        forward(p, x.data(), y.data());
        CHECK(relativeError(complexOf(y), dft(complexOf(x), false)) < 2e-6);
        std::vector<float2> back(x.size());
        inverse(p, y.data(), back.data());
        std::vector<Complex> scaled = complexOf(x);
        for (Complex& z : scaled)
            z *= static_cast<double>(n);
        CHECK(relativeError(complexOf(back), scaled) < 2e-6);
        lookup<void(void*)>(*jit, "release")(p);
    }
}

TEST_CASE("a plan of a size the library cannot take is null, and a transform does nothing on it", "[codegen][fft]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    auto plan = lookup<void*(int)>(*jit, "real_plan");
    CHECK(plan(0) == nullptr);
    CHECK(plan(-64) == nullptr);
    CHECK(lookup<void*(int)>(*jit, "complex_plan")(0) == nullptr);

    const std::vector<float> x = noise(64, 3u);
    std::vector<float2> bins(33, float2{5.0f, 5.0f});
    lookup<void(void*, const float*, float2*)>(*jit, "forward")(nullptr, x.data(), bins.data());
    CHECK(bins[0].x == 5.0f);   // untouched

    // A real plan given to a complex transform does nothing either.
    void* real = plan(64);
    REQUIRE(real != nullptr);
    std::vector<float2> in(64, float2{1.0f, 0.0f});
    std::vector<float2> out(64, float2{5.0f, 5.0f});
    lookup<void(void*, const float2*, float2*)>(*jit, "cforward")(real, in.data(), out.data());
    CHECK(out[0].x == 5.0f);
    lookup<void(void*)>(*jit, "release")(real);
    lookup<void(void*)>(*jit, "release")(nullptr);   // as free(nullptr)
}

TEST_CASE("plans are PFFFT's sizes, and buffers need no alignment", "[codegen][fft]")
{
    std::string errors;
    auto jit = compile(kProgram, &errors);
    INFO(errors);
    REQUIRE(jit);
    auto plan = lookup<void*(int)>(*jit, "real_plan");
    auto complexPlan = lookup<void*(int)>(*jit, "complex_plan");
    auto release = lookup<void(void*)>(*jit, "release");
    // A real plan takes a multiple of 32 points, a complex one of 16, with no prime factor but 2, 3 and 5.
    for (const int n : {32, 96, 480, 960, 1920})
    {
        void* p = plan(n);
        CHECK(p != nullptr);
        release(p);
    }
    for (const int n : {16, 48, 80})
    {
        void* p = complexPlan(n);
        CHECK(p != nullptr);
        release(p);
    }
    CHECK(plan(16) == nullptr);           // not a multiple of 32
    CHECK(plan(1000) == nullptr);         // 2^3 5^3: not a multiple of 32
    CHECK(plan(224) == nullptr);          // 32 7
    CHECK(complexPlan(40) == nullptr);    // not a multiple of 16
    CHECK(plan(1 << 27) == nullptr);      // past PFFFT's 2^26

    // A buffer a float off 16 bytes gives the bits an aligned one does: the plan copies it through its own.
    constexpr int n = 96;
    void* p = plan(n);
    REQUIRE(p != nullptr);
    const std::vector<float> x = noise(n, 5u);
    std::vector<float> shiftedIn(n + 1);
    std::copy(x.begin(), x.end(), shiftedIn.begin() + 1);
    std::vector<float2> bins(n / 2 + 1);
    std::vector<float> shiftedBins(2 * (n / 2 + 1) + 1);
    auto forward = lookup<void(void*, const float*, float*)>(*jit, "forward");   // the floats of the bins
    forward(p, x.data(), reinterpret_cast<float*>(bins.data()));
    forward(p, shiftedIn.data() + 1, shiftedBins.data() + 1);
    CHECK(std::equal(shiftedBins.begin() + 1, shiftedBins.end(), reinterpret_cast<const float*>(bins.data())));
    auto inverse = lookup<void(void*, const float*, float*)>(*jit, "inverse");
    std::vector<float> back(n);
    std::vector<float> shiftedBack(n + 1);
    inverse(p, reinterpret_cast<const float*>(bins.data()), back.data());
    inverse(p, shiftedBins.data() + 1, shiftedBack.data() + 1);
    CHECK(std::equal(back.begin(), back.end(), shiftedBack.begin() + 1));
    release(p);

    // And a complex plan's input and output.
    constexpr int m = 48;
    void* c = complexPlan(m);
    REQUIRE(c != nullptr);
    const std::vector<float> z = noise(2 * m, 9u);
    std::vector<float> shiftedZ(2 * m + 1);
    std::copy(z.begin(), z.end(), shiftedZ.begin() + 1);
    std::vector<float> y(2 * m);
    std::vector<float> shiftedY(2 * m + 1);
    auto cforward = lookup<void(void*, const float*, float*)>(*jit, "cforward");
    cforward(c, z.data(), y.data());
    cforward(c, shiftedZ.data() + 1, shiftedY.data() + 1);
    CHECK(std::equal(y.begin(), y.end(), shiftedY.begin() + 1));
    release(c);
}

TEST_CASE("an FFT plan is made and freed off the audio thread: the effects, and the errors", "[codegen][fft]")
{
    // A plan lives in a module, or in memory the host owns: made in init, used in process, freed at the end.
    CHECK(errorsOf("module Spectrum\n"
                   "{\n"
                   "    void* plan;\n"
                   "    void process(const float* in, float2* bins) [[clang::nonblocking]]\n"
                   "    {\n"
                   "        if (!plan)\n"
                   "            plan = fft_create(1024);\n"
                   "        fft_forward(plan, in, bins);\n"
                   "    }\n"
                   "    void end() [[clang::nonallocating]] { fft_destroy(plan); }\n"
                   "};\n")
          == "test.fm:7:20: error: function with 'nonblocking' attribute must not call non-'nonblocking' function "
             "'fft_create'\n"
             "test.fm:10:43: error: function with 'nonallocating' attribute must not call non-'nonallocating' "
             "function 'fft_destroy'\n");
    CHECK(errorsOf("void f(void* p, const int* k, float* x, const float2* c, float2* b)\n"
                   "{\n"
                   "    fft_forward(p, k, b);\n"
                   "    fft_inverse(p, c, k);\n"
                   "    fft_forward(p, x, c);\n"
                   "    fft_forward(p, x);\n"
                   "    fft_inverse(p, x, b);\n"
                   "}\n"
                   "void* fft_create(int n) { return 0; }\n")
          == "test.fm:3:5: error: no matching function for call to 'fft_forward': it takes (void *, const float *, "
             "float2 *) for a real plan, or (void *, const float2 *, float2 *) for a complex one\n"
             "test.fm:4:5: error: no matching function for call to 'fft_inverse': it takes (void *, const float2 *, "
             "float *) for a real plan, or (void *, const float2 *, float2 *) for a complex one\n"
             "test.fm:5:5: error: passing 'const float2 *' to parameter of type 'float2 *' discards qualifiers\n"
             "test.fm:6:5: error: too few arguments to function call, expected 3, have 2\n"
             "test.fm:7:5: error: incompatible pointer types passing 'float *' to parameter of type 'const float2 *'\n"
             "test.fm:9:7: error: 'fft_create' is a builtin and cannot be redefined\n");
}
