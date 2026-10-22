// tests/library/DenormalsTest.cpp: Program::Options::denormals, as a plugin host uses it (C++17, FMinus::FMinus alone).
// Preserve, the default, keeps subnormals; Flush makes each entry point, a [[clang::nonblocking]] function nothing in
// the program calls, run flushing and hand the host's floating-point mode back as it was.
#include "fminus/FMinus.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace
{
    // The host's floating-point control register: AArch64's FPCR (FZ, bit 24), x86's MXCSR (FTZ and DAZ).
#if defined(__aarch64__)
    constexpr std::uint64_t kFlushBits = std::uint64_t{1} << 24;

    std::uint64_t controlRegister()
    {
        std::uint64_t value = 0;
        asm volatile("mrs %0, fpcr" : "=r"(value) : : "memory");
        return value;
    }

    void setControlRegister(std::uint64_t value)
    {
        asm volatile("msr fpcr, %0" : : "r"(value) : "memory");
    }
#elif defined(__x86_64__)
    constexpr std::uint64_t kFlushBits = 0x8040;
    // MXCSR's low six bits are its exception flags, not its mode: sticky, and set by the arithmetic itself (a subnormal
    // result sets DE and UE, an inexact one PE), the host's and FMinus's alike. So the mode is the other bits. (FPCR
    // has no flags: AArch64 keeps them in FPSR.)
    constexpr std::uint64_t kExceptionFlags = 0x3F;

    std::uint64_t controlRegister()
    {
        return _mm_getcsr() & ~kExceptionFlags;
    }

    void setControlRegister(std::uint64_t value)
    {
        _mm_setcsr(static_cast<unsigned>(value));
    }
#else
#error "the denormal tests read the floating-point control register of arm64 or x86-64"
#endif

    // A block kernel whose products underflow, and the per-sample helper it calls.
    const char* const kScale = "float times(float a, float b) [[clang::nonblocking]] { return a * b; }\n"
                               "\n"
                               "void scale(const float* restrict in, float* restrict out, int n, float g)\n"
                               "    [[clang::nonblocking]]\n"
                               "{\n"
                               "    for (int i = 0; i < n; ++i)\n"
                               "        out[i] = times(in[i], g);\n"
                               "}\n";

    using Scale = void(const float*, float*, int, float);
    using Times = float(float, float);

    float processed(fminus::Program& program, float sample, float gain)
    {
        std::string errors;
        auto* scale = program.function<Scale>("scale", errors);
        REQUIRE(scale != nullptr);
        float out = 1.0f;
        scale(&sample, &out, 1, gain);
        return out;
    }
} // namespace

TEST_CASE("Options::denormals: Preserve keeps subnormals, Flush runs an entry point flushing", "[library][denormals]")
{
    const std::uint64_t host = controlRegister() & ~kFlushBits;   // an IEEE host
    setControlRegister(host);

    std::string errors;
    fminus::Program::Options options;
    CHECK(options.denormals == fminus::Program::Denormals::Preserve);   // the default
    auto preserved = fminus::Program::compile(kScale, errors, options);
    INFO(errors);
    REQUIRE(preserved);
    const float kept = processed(*preserved, 1e-30f, 1e-10f);
    CHECK(std::fpclassify(kept) == FP_SUBNORMAL);

    options.denormals = fminus::Program::Denormals::Flush;
    auto flushed = fminus::Program::compile(kScale, errors, options);
    REQUIRE(flushed);
    CHECK(processed(*flushed, 1e-30f, 1e-10f) == 0.0f);
    CHECK(processed(*flushed, 1e-20f, 1e-10f) == 1e-20f * 1e-10f);
    CHECK(controlRegister() == host);   // handed back as it was

    // times is called by scale, so it is no entry point: called from the host, it runs in the host's mode.
    auto* times = flushed->function<Times>("times", errors);
    REQUIRE(times != nullptr);
    CHECK(std::fpclassify(times(1e-30f, 1e-10f)) == FP_SUBNORMAL);
    CHECK(flushed->nonBlocking("scale"));

    // A host that flushes already keeps flushing after the call. As a host does, it looks the kernel up first, off the
    // audio thread, and only then sets its mode: nothing runs between the mode and the register's reading but the call.
    // (On Windows, the register read after a look-up made while flushing has been IEEE's default, 0x1f80.)
    auto* scale = flushed->function<Scale>("scale", errors);
    REQUIRE(scale != nullptr);
    const float sample = 1e-30f;
    float out = 1.0f;
    setControlRegister(host | kFlushBits);
    scale(&sample, &out, 1, 1e-10f);
    const std::uint64_t after = controlRegister();
    setControlRegister(host);
    CHECK(out == 0.0f);
    CHECK(after == (host | kFlushBits));
}

TEST_CASE("Options::denormals: Flush runs an entry point flushing at each optimisation, one that reads no memory too",
          "[library][denormals]")
{
    const std::uint64_t host = controlRegister() & ~kFlushBits;   // an IEEE host
    setControlRegister(host);

    // LLVM finds that scaled's code reads and writes no memory, so nothing orders a plain call of it with the writes of
    // the control register: the entry point's call must be one it keeps between them.
    const char* const source = "float scaled(float x) [[clang::nonblocking]] { return x * 1e-30f * 1e-10f; }\n";
    for (int optimisation = 0; optimisation <= 3; ++optimisation)
    {
        INFO("-O" << optimisation);
        std::string errors;
        fminus::Program::Options options;
        options.optimisation = optimisation;
        options.denormals = fminus::Program::Denormals::Flush;
        auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        auto* scaled = program->function<float(float)>("scaled", errors);
        REQUIRE(scaled != nullptr);
        const float result = scaled(1.0f);
        CHECK(result == 0.0f);
        CHECK_FALSE(std::signbit(result));
        CHECK(controlRegister() == host);
    }
}
