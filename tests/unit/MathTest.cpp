// tests/unit/MathTest.cpp: the math builtins' modes (ir::Options::math, Program::Math; ir/Math.cpp, runtime/Math.cpp).
//
// First the premise the vector mode stands on, checked again on every run, so that a macOS whose libsystem_m changed
// fails here rather than in a program's last bits: each lane of the runtime's four-lane functions is the scalar
// function of that lane, whatever the other lanes hold, and libsystem_m's own functions are lane-independent where the
// runtime calls them whole. Then the promise itself: a program gives a float one result wherever it computes it, in one
// call or in blocks of 61, at -O0 and -O2, in a poly float of 1 to 64 lanes, from a frozen parameter or a knob, for
// each of the five functions, in loops that vectorise, loops that don't, float2, float4, float8 and poly code; and that
// result is fminus::math's. Then the libm mode, C++'s std:: functions bit for bit, and the fast mode, LLVM's
// intrinsics as they were. Last, the runtime's names: a program that calls a builtin declares no function of its
// runtime functions' names, and one that calls none of them may declare and call them as it likes.
#include "fminus/FMinus.h"
#include "frontend/Diagnostics.h"
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "runtime/Runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
// libsystem_m's, as runtime/Math.cpp declares them.
extern "C" fminus_rt_float4 _simd_sin_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_cos_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_exp_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_tanh_f4(fminus_rt_float4 x) noexcept;
extern "C" fminus_rt_float4 _simd_log_f4(fminus_rt_float4 x) noexcept;
#endif

namespace
{
    using Four = fminus_rt_float4 (*)(fminus_rt_float4) noexcept;
    using One = float (*)(float) noexcept;

    // One of the five, by every name it has.
    struct Function
    {
        std::string_view builtin;   // FMinus's: "sinf"
        One scalar;                 // the vector mode's: fminus_rt_sinf
        Four four;                  // fminus_rt_sin_f4
        One math;                   // the public API's: fminus::math::sin
        float (*libm)(float);       // C++'s std::sin of a float
        double (*exact)(double);    // std::sin of a double, which LLVM folds a constant with in the fast mode
        float low;                  // the arguments a program's tests mostly take, in [low, high)
        float high;
    };

    float stdSin(float x)
    {
        return std::sin(x);
    }
    float stdCos(float x)
    {
        return std::cos(x);
    }
    float stdExp(float x)
    {
        return std::exp(x);
    }
    float stdTanh(float x)
    {
        return std::tanh(x);
    }
    float stdLog(float x)
    {
        return std::log(x);
    }
    double doubleSin(double x)
    {
        return std::sin(x);
    }
    double doubleCos(double x)
    {
        return std::cos(x);
    }
    double doubleExp(double x)
    {
        return std::exp(x);
    }
    double doubleTanh(double x)
    {
        return std::tanh(x);
    }
    double doubleLog(double x)
    {
        return std::log(x);
    }

    const std::array<Function, 5> kFunctions{
        Function{"sinf", fminus_rt_sinf, fminus_rt_sin_f4, fminus::math::sin, stdSin, doubleSin, -50.0f, 50.0f},
        Function{"cosf", fminus_rt_cosf, fminus_rt_cos_f4, fminus::math::cos, stdCos, doubleCos, -50.0f, 50.0f},
        Function{"expf", fminus_rt_expf, fminus_rt_exp_f4, fminus::math::exp, stdExp, doubleExp, -30.0f, 30.0f},
        Function{"tanhf", fminus_rt_tanhf, fminus_rt_tanh_f4, fminus::math::tanh, stdTanh, doubleTanh, -10.0f, 10.0f},
        Function{"logf", fminus_rt_logf, fminus_rt_log_f4, fminus::math::log, stdLog, doubleLog, 0.0f, 100.0f},
    };

    const Function& function(std::string_view builtin)
    {
        for (const Function& candidate : kFunctions)
        {
            if (candidate.builtin == builtin)
                return candidate;
        }
        return kFunctions[0];
    }

    // The largest magnitude runtime/Math.cpp hands libsystem_m's sin and cos a whole vector at, and the edge of their
    // fast path (macOS 27, an M5): the arguments around both are where a mistake would show.
    constexpr float kSinCosLimit = 65536.0f;
    constexpr float kAppleEdge = 1e6f;

    // The same float: the same bits, or both NaN (whose payloads say nothing about the function).
    bool same(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b) || (std::isnan(a) && std::isnan(b));
    }

    // The float `steps` floats from x, toward +infinity (or toward -infinity, for a negative count).
    float step(float x, int steps)
    {
        for (; steps > 0; --steps)
            x = std::nextafter(x, std::numeric_limits<float>::infinity());
        for (; steps < 0; ++steps)
            x = std::nextafter(x, -std::numeric_limits<float>::infinity());
        return x;
    }

    // Arguments for the premise: phases and audio's other arguments, the edges of each function and of the runtime's
    // and Apple's paths, subnormals, infinities, NaNs, huge values, and any float at all.
    struct Arguments
    {
        std::mt19937_64 random{20261021};

        float uniform(float low, float high)
        {
            return std::uniform_real_distribution<float>(low, high)(random);
        }

        float next()
        {
            static constexpr float kInfinity = std::numeric_limits<float>::infinity();
            static constexpr std::array kEdges{
                0.0f,
                -0.0f,
                std::numeric_limits<float>::denorm_min(),
                -std::numeric_limits<float>::denorm_min(),
                0x1.fffffcp-127f,   // the largest subnormal
                std::numeric_limits<float>::min(),
                kInfinity,
                -kInfinity,
                std::numeric_limits<float>::quiet_NaN(),
                -std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::max(),
                -std::numeric_limits<float>::max(),
                1e30f,
                -1e30f,
                105414350.0f,   // where sinf's argument reduction changes method
                3.14159265f,
                1.57079637f,
                88.7228394f,    // expf's last finite result
                88.7228470f,    // its first infinite one
                -87.3365479f,   // its first subnormal one
                -103.972084f,   // its last nonzero one
                9.01f,          // tanhf about to round to 1
                1.0f,
                -1.0f,
            };
            const std::uint64_t r = random();
            switch (r % 9)
            {
            case 0:
                return uniform(-2.0f * std::numbers::pi_v<float>, 2.0f * std::numbers::pi_v<float>);   // a phase
            case 1:
                return uniform(-100.0f, 100.0f);
            case 2:
                return uniform(0.0f, 20000.0f);   // a frequency, or a gain's logarithm's argument
            case 3:
                return kEdges[(r >> 8) % kEdges.size()];
            case 4:
            {
                // Around the runtime's limit and Apple's edge, a few floats either side, of either sign.
                const float around = (r >> 8) % 2 == 0 ? kSinCosLimit : kAppleEdge;
                const float near = step(around, static_cast<int>((r >> 9) % 9) - 4);
                return (r >> 13) % 2 == 0 ? near : -near;
            }
            case 5:
                return uniform(-2e6f, 2e6f);   // both sides of both
            case 6:
                return std::ldexp(uniform(-1.0f, 1.0f), static_cast<int>((r >> 8) % 280) - 150);   // any magnitude
            case 7:
                return std::bit_cast<float>(static_cast<std::uint32_t>(r >> 32));   // any float, NaNs with payloads
            default:
                return std::bit_cast<float>(static_cast<std::uint32_t>(r >> 32) & 0x807fffffu);   // subnormal, zero
            }
        }

        // Within the limit, for libsystem_m's sin and cos whole: the same mix, any lane past it brought back.
        float within(float limit)
        {
            const float x = next();
            return std::isfinite(x) && std::fabs(x) <= limit ? x : uniform(-limit, limit);
        }
    };

    // How many of `vectors` four-lane calls of `four` give some lane other than `one` of that lane, over vectors whose
    // lanes `draw` fills; and the first such lane, described, in `first`.
    template <typename Draw, typename OneLane>
    long laneMismatches(Four four, OneLane one, long vectors, Draw draw, std::string& first)
    {
        long mismatches = 0;
        for (long i = 0; i < vectors; ++i)
        {
            const fminus_rt_float4 x{draw(), draw(), draw(), draw()};
            const fminus_rt_float4 y = four(x);
            for (int k = 0; k < 4; ++k)
            {
                const float expected = one(x[k]);
                if (same(y[k], expected))
                    continue;
                if (mismatches++ == 0)
                    first = "lane " + std::to_string(k) + " of (" + std::to_string(x[0]) + ", " + std::to_string(x[1])
                          + ", " + std::to_string(x[2]) + ", " + std::to_string(x[3]) + "): "
                          + std::to_string(y[k]) + ", not " + std::to_string(expected);
            }
        }
        return mismatches;
    }

    constexpr long kVectors = 1'000'000;
} // namespace

TEST_CASE("the vector math mode's four-lane functions give each lane the scalar function of that lane", "[math]")
{
    for (const Function& f : kFunctions)
    {
        INFO(f.builtin);
        Arguments arguments;
        std::string first;
        // Lane k of fminus_rt_sin_f4(x) is fminus_rt_sinf(x[k]), whatever x's other lanes hold.
        const long mismatches = laneMismatches(f.four, f.scalar, kVectors, [&] { return arguments.next(); }, first);
        INFO(first);
        CHECK(mismatches == 0);
        // And fminus_rt_sinf(x) is lane 0 of fminus_rt_sin_f4 of four copies of x, as Runtime.h says.
        long splats = 0;
        for (long i = 0; i < kVectors; ++i)
        {
            const float x = arguments.next();
            splats += same(f.scalar(x), f.four(fminus_rt_float4{x, x, x, x})[0]) ? 0 : 1;
        }
        CHECK(splats == 0);
        // fminus::math is the scalar function.
        for (int i = 0; i < 100000; ++i)
        {
            const float x = arguments.next();
            if (!same(f.math(x), f.scalar(x)))
            {
                CHECK(f.math(x) == f.scalar(x));
                break;
            }
        }
    }
}

TEST_CASE("libsystem_m's vector functions are lane-independent where the runtime calls them whole", "[math]")
{
#if defined(__APPLE__)
    // Lane k of _simd_sin_f4(x) is lane 0 of _simd_sin_f4 of four copies of x[k], for vectors whose every lane is
    // within the runtime's limit: what fminus_rt_sin_f4 relies on when it hands over a whole vector. (Past Apple's
    // edge, one lane sends the whole vector down another path, which is why the runtime has a limit at all.)
    const struct
    {
        const char* name;
        Four simd;
        bool limited;   // sin and cos: within kSinCosLimit only; exp, tanh and log: every float
    } kLibrary[] = {
        {"_simd_sin_f4", _simd_sin_f4, true},    {"_simd_cos_f4", _simd_cos_f4, true},
        {"_simd_exp_f4", _simd_exp_f4, false},   {"_simd_tanh_f4", _simd_tanh_f4, false},
        {"_simd_log_f4", _simd_log_f4, false},
    };
    for (const auto& library : kLibrary)
    {
        INFO(library.name);
        Arguments arguments;
        std::string first;
        const auto splat = [&](float x) { return library.simd(fminus_rt_float4{x, x, x, x})[0]; };
        const auto draw = [&] { return library.limited ? arguments.within(kSinCosLimit) : arguments.next(); };
        const long mismatches = laneMismatches(library.simd, splat, kVectors, draw, first);
        INFO(first);
        CHECK(mismatches == 0);
    }
#else
    SKIP("no libsystem_m here: the vector mode's functions are libm's, lane by lane");
#endif
}

namespace
{
    // The kernels each function is checked in, from one template: `F` the builtin, as in sinf_loop.
    constexpr std::string_view kKernels = R"(
    param float F_shape [[fminus::default(0.5)]];
    float F_y;

    // No dependency from one sample to the next: the loop vectoriser widens it, and a block's end decides which samples
    // its vectorised body takes and which its scalar remainder.
    void F_loop(const float* restrict in, float* restrict out, int n)
    {
        for (int i = 0; i < n; ++i)
            out[i] = F(in[i]);
    }

    // Each sample's argument needs the last one's result, so the loop stays scalar. The clamp lets the state recover
    // from a NaN or an infinity.
    void F_recursive(const float* restrict in, float* restrict out, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            F_y = F(in[i] + 0.5f * fminf(fmaxf(F_y, -1), 1));
            out[i] = F_y;
        }
    }

    // A parameter's function: of a constant, when the parameter is frozen.
    void F_parameter(const float* restrict in, float* restrict out, int n)
    {
        for (int i = 0; i < n; ++i)
            out[i] = in[i] * F(F_shape[i]);
    }

    void F_two(const float* restrict in, float* restrict out, int n)
    {
        int i = 0;
        for (; i + 2 <= n; i += 2)
        {
            const float2 v = F((float2)(in[i], in[i + 1]));
            out[i] = v.x;
            out[i + 1] = v.y;
        }
        for (; i < n; ++i)
            out[i] = F(in[i]);
    }

    void F_four(const float* restrict in, float* restrict out, int n)
    {
        int i = 0;
        for (; i + 4 <= n; i += 4)
        {
            const float4 v = F((float4)(in[i], in[i + 1], in[i + 2], in[i + 3]));
            out[i] = v.x;
            out[i + 1] = v.y;
            out[i + 2] = v.z;
            out[i + 3] = v.w;
        }
        for (; i < n; ++i)
            out[i] = F(in[i]);
    }

    void F_eight(const float* restrict in, float* restrict out, int n)
    {
        int i = 0;
        for (; i + 8 <= n; i += 8)
        {
            const float8 v = F((float8)(in[i], in[i + 1], in[i + 2], in[i + 3], in[i + 4], in[i + 5], in[i + 6],
                                        in[i + 7]));
            out[i] = v.s0;
            out[i + 1] = v.s1;
            out[i + 2] = v.s2;
            out[i + 3] = v.s3;
            out[i + 4] = v.s4;
            out[i + 5] = v.s5;
            out[i + 6] = v.s6;
            out[i + 7] = v.s7;
        }
        for (; i < n; ++i)
            out[i] = F(in[i]);
    }

    // poly_lanes() samples a step, the rest under a mask.
    void F_poly(const float* restrict in, float* restrict out, int n)
    {
        int i = 0;
        for (; i + poly_lanes() <= n; i += poly_lanes())
            poly_store(out + i, F(poly_load(in + i)));
        const poly bool rest = i + poly_index() < n;
        poly_store(out + i, F(poly_load(in + i, rest)), rest);
    }
)";

    constexpr std::array<std::string_view, 7> kShapes{"loop", "recursive", "parameter", "two", "four", "eight", "poly"};

    // A module, Math, with the function's kernels: kKernels with each F (before a '(' or a '_') its builtin.
    std::string kernelsSource(const Function& f)
    {
        std::string kernels(kKernels);
        for (std::size_t at = kernels.find('F'); at != std::string::npos; at = kernels.find('F', at))
        {
            const bool alone = (at == 0 || !std::isalnum(static_cast<unsigned char>(kernels[at - 1])))
                               && at + 1 < kernels.size() && (kernels[at + 1] == '_' || kernels[at + 1] == '(');
            if (!alone)
            {
                ++at;
                continue;
            }
            kernels.replace(at, 1, f.builtin);
            at += f.builtin.size();
        }
        return "module Math\n{\n" + kernels + "};\n";
    }

    // A function's arguments for the kernels: its usual range, and every 37th sample an edge: zeros, a subnormal,
    // infinities, a NaN, and values past the runtime's limit and Apple's edge, which send a vector lane by lane.
    std::vector<float> inputFor(const Function& f, std::size_t samples)
    {
        static constexpr std::array kEdges{0.0f, -0.0f, 1e-40f, std::numeric_limits<float>::infinity(),
                                           -std::numeric_limits<float>::infinity(),
                                           std::numeric_limits<float>::quiet_NaN(), 65536.0f, 65536.0078f, 70000.0f,
                                           -1e6f, 2e6f, 1e30f};
        std::mt19937 random(7);
        std::uniform_real_distribution<float> usual(f.low, f.high);
        std::vector<float> input(samples);
        for (std::size_t i = 0; i < samples; ++i)
            input[i] = i % 37 == 5 ? kEdges[(i / 37) % kEdges.size()] : usual(random);
        return input;
    }

    // What a kernel computes, in C++, with `math` for the builtin: fminus::math's or std::'s.
    std::vector<float> reference(std::string_view shape, float (*math)(float), const std::vector<float>& input,
                                 float parameter)
    {
        std::vector<float> output(input.size());
        float y = 0.0f;
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            if (shape == "recursive")
            {
                // One rounding a statement, as FMinus's IR has it (no fused multiply-add).
                const float clamped = std::fmin(std::fmax(y, -1.0f), 1.0f);
                const float scaled = 0.5f * clamped;
                y = math(input[i] + scaled);
                output[i] = y;
            }
            else if (shape == "parameter")
                output[i] = input[i] * math(parameter);
            else
                output[i] = math(input[i]);
        }
        return output;
    }

    struct Free
    {
        void operator()(void* memory) const
        {
            std::free(memory);
        }
    };

    using Method = void(void*, const float*, float*, int);

    // The kernel over the whole input in calls of `block` samples (0: one call), on a fresh instance (every field 0)
    // whose parameters a knob's program reads at `parameter`.
    std::vector<float> run(const fminus::Program& program, Method* method, const std::vector<float>& input,
                           std::size_t block, float parameter)
    {
        const fminus::Program::Module& module = *program.module("Math");
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        const std::unique_ptr<void, Free> instance(
            std::aligned_alloc(alignment, (module.size + alignment - 1) / alignment * alignment));
        std::memset(instance.get(), 0, module.size);
        for (const fminus::Program::Parameter& p : module.parameters)
            fminus::Program::setValue(instance.get(), p, static_cast<double>(parameter));
        std::vector<float> output(input.size());
        const std::size_t step = block == 0 ? input.size() : block;
        for (std::size_t start = 0; start < input.size(); start += step)
        {
            const std::size_t n = std::min(step, input.size() - start);
            method(instance.get(), input.data() + start, output.data() + start, static_cast<int>(n));
        }
        return output;
    }

    // The first sample at which two outputs are not the same float, or -1.
    long firstDifference(const std::vector<float>& a, const std::vector<float>& b)
    {
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (!same(a[i], b[i]))
                return static_cast<long>(i);
        }
        return -1;
    }

    // A parameter value at which LLVM's folding (the double function, rounded) and `math` disagree, so that a frozen
    // parameter folded by mistake would show: the first float from 0.5 up where they do (or 0.5, if none is near).
    float foldingWouldShow(const Function& f, float (*math)(float))
    {
        float x = 0.5f;
        for (int i = 0; i < (1 << 20); ++i, x = std::nextafter(x, 1.0f))
        {
            if (static_cast<float>(f.exact(static_cast<double>(x))) != math(x))
                return x;
        }
        return 0.5f;
    }

    // Every kernel of `f`, in programs at -O0 and -O2, at every poly width, its parameter a knob and frozen, in one
    // call and in blocks of 61: each gives `math`'s result, bit for bit, at every sample.
    void checkEverywhere(const Function& f, fminus::Program::Math mode, float (*math)(float))
    {
        const std::string source = kernelsSource(f);
        const std::vector<float> input = inputFor(f, 1009);
        const float parameter = foldingWouldShow(f, math);
        const std::string shape = std::string(f.builtin) + "_shape";
        for (const int level : {0, 2})
        {
            for (unsigned lanes = 1; lanes <= 64; lanes *= 2)
            {
                for (const bool frozen : {false, true})
                {
                    fminus::Program::Options options;
                    options.optimisation = level;
                    options.polyLanes = lanes;
                    options.math = mode;
                    if (frozen)
                        options.parameters["Math::" + shape] =
                            fminus::Program::Binding::frozen(static_cast<double>(parameter));
                    std::string errors;
                    const auto program = fminus::Program::compile(source, errors, options);
                    INFO(errors);
                    REQUIRE(program);
                    for (std::string_view kernel : kShapes)
                    {
                        const std::string name = "Math::" + std::string(f.builtin) + "_" + std::string(kernel);
                        auto* method = program->function<Method>(name, errors);
                        REQUIRE(method != nullptr);
                        const std::vector<float> expected = reference(kernel, math, input, parameter);
                        for (const std::size_t block : {std::size_t{0}, std::size_t{61}})
                        {
                            INFO(name << " at -O" << level << ", " << lanes << " lanes, "
                                      << (frozen ? "frozen" : "a knob") << ", "
                                      << (block == 0 ? "one call" : "blocks of 61"));
                            const long at = firstDifference(run(*program, method, input, block, parameter), expected);
                            INFO("first difference at sample " << at << " (" << (at < 0 ? 0.0f : input[at]) << ")");
                            CHECK(at == -1);
                        }
                    }
                }
            }
        }
    }
} // namespace

TEST_CASE("sinf gives a float one result wherever a program computes it: fminus::math::sin's", "[math]")
{
    checkEverywhere(function("sinf"), fminus::Program::Math::Vector, fminus::math::sin);
}

TEST_CASE("cosf gives a float one result wherever a program computes it: fminus::math::cos's", "[math]")
{
    checkEverywhere(function("cosf"), fminus::Program::Math::Vector, fminus::math::cos);
}

TEST_CASE("expf gives a float one result wherever a program computes it: fminus::math::exp's", "[math]")
{
    checkEverywhere(function("expf"), fminus::Program::Math::Vector, fminus::math::exp);
}

TEST_CASE("tanhf gives a float one result wherever a program computes it: fminus::math::tanh's", "[math]")
{
    checkEverywhere(function("tanhf"), fminus::Program::Math::Vector, fminus::math::tanh);
}

TEST_CASE("logf gives a float one result wherever a program computes it: fminus::math::log's", "[math]")
{
    checkEverywhere(function("logf"), fminus::Program::Math::Vector, fminus::math::log);
}

TEST_CASE("the libm math mode is C++'s std:: functions of floats, bit for bit, wherever a program computes them",
          "[math]")
{
    for (const Function& f : kFunctions)
    {
        INFO(f.builtin);
        checkEverywhere(f, fminus::Program::Math::Libm, f.libm);
    }
}

TEST_CASE("the fast math mode is LLVM's intrinsics, as before: libm for a float, the vector library for a float4",
          "[math]")
{
    fminus::Program::Options options;
    options.math = fminus::Program::Math::Fast;
    std::string errors;
    const auto program = fminus::Program::compile("float one(float x) { return sinf(x); }\n"
                                                  "float4 four(float4 x) { return sinf(x); }\n"
                                                  "float folded(void) { return sinf(0.5f); }\n",
                                                  errors, options);
    INFO(errors);
    REQUIRE(program);
    using float4 = float __attribute__((ext_vector_type(4)));
    auto* one = program->function<float(float)>("one", errors);
    auto* four = program->function<float4(float4)>("four", errors);
    auto* folded = program->function<float()>("folded", errors);
    REQUIRE((one != nullptr && four != nullptr && folded != nullptr));
    Arguments arguments;
    long libm = 0;
    long library = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const float4 x{arguments.uniform(-100.0f, 100.0f), arguments.uniform(-100.0f, 100.0f),
                       arguments.uniform(-100.0f, 100.0f), arguments.uniform(-100.0f, 100.0f)};
        libm += same(one(x[0]), std::sin(x[0])) ? 0 : 1;   // the backend's call of sinf
        const float4 y = four(x);
#if defined(__APPLE__)
        const fminus_rt_float4 simd = _simd_sin_f4(fminus_rt_float4{x[0], x[1], x[2], x[3]});   // ReplaceWithVeclib's
#endif
        for (int k = 0; k < 4; ++k)
        {
#if defined(__APPLE__)
            library += same(y[k], simd[k]) ? 0 : 1;
#else
            library += same(y[k], std::sin(x[k])) ? 0 : 1;   // a sinf a lane
#endif
        }
    }
    CHECK(libm == 0);
    CHECK(library == 0);
    // A constant's sine is folded, with the double function, rounded.
    CHECK(folded() == static_cast<float>(std::sin(0.5)));
}

TEST_CASE("a program that calls a math builtin declares no function of its runtime functions' names, in any mode",
          "[math]")
{
    // Programs that compiled wrongly before the rule (ir/Math.cpp, checkMathNames): a definition of sinf's four-lane
    // name of another type, which LoopVectorize took for fminus_rt_sinf's vector variant and asserted on at -O2, one
    // of a float, which it asserted on too, and a prototype, which did the same in the JIT; a definition of the scalar
    // name of the runtime's type, which took the runtime's attributes (memory(none), willreturn, speculatable), so
    // that a call of it that could loop forever was removed; and the libm mode's name, refused in the other modes too.
    // Each is an error at the builtin's call, in every mode, with a note at the program's function; cosf, whose names
    // the program leaves to the runtime, is not.
    struct Case
    {
        const char* source;
        const char* errors;
    };
    const std::array cases{
        Case{"int fminus_rt_sin_f4(int x)\n"
             "{\n"
             "    return x + 1;\n"
             "}\n"
             "void sines(const float* restrict in, float* restrict out, int n)\n"
             "{\n"
             "    for (int i = 0; i < n; ++i)\n"
             "        out[i] = sinf(in[i]);\n"
             "}\n"
             "float one(float x) { return x; }\n",
             "test.fm:8:18: error: 'sinf' reserves 'fminus_rt_sin_f4' for the runtime's function, and this program "
             "declares a function of that name: rename it\n"
             "test.fm:1:5: note: 'fminus_rt_sin_f4' declared here\n"},
        Case{"float fminus_rt_sin_f4(float x) { return x + 1; }\n"
             "void sines(const float* restrict in, float* restrict out, int n)\n"
             "{\n"
             "    for (int i = 0; i < n; ++i)\n"
             "        out[i] = sinf(in[i]);\n"
             "}\n",
             "test.fm:5:18: error: 'sinf' reserves 'fminus_rt_sin_f4' for the runtime's function, and this program "
             "declares a function of that name: rename it\n"
             "test.fm:1:7: note: 'fminus_rt_sin_f4' declared here\n"},
        Case{"float fminus_rt_sin_f4(float x);\n"
             "void sines(const float* restrict in, float* restrict out, int n)\n"
             "{\n"
             "    for (int i = 0; i < n; ++i)\n"
             "        out[i] = sinf(in[i]);\n"
             "}\n"
             "float one(float x) { return x; }\n",
             "test.fm:5:18: error: 'sinf' reserves 'fminus_rt_sin_f4' for the runtime's function, and this program "
             "declares a function of that name: rename it\n"
             "test.fm:1:7: note: 'fminus_rt_sin_f4' declared here\n"},
        Case{"float fminus_rt_sinf(float x)\n"
             "{\n"
             "    if (x > 100.0f)\n"
             "    {\n"
             "        while (1) {}\n"
             "    }\n"
             "    return x;\n"
             "}\n"
             "float f(float x) { return sinf(x) + cosf(x); }\n",
             "test.fm:9:27: error: 'sinf' reserves 'fminus_rt_sinf' for the runtime's function, and this program "
             "declares a function of that name: rename it\n"
             "test.fm:1:7: note: 'fminus_rt_sinf' declared here\n"},
        Case{"float fminus_rt_libm_expf(float x) { return x; }\n"
             "float4 f(float4 x) { return expf(x); }\n",
             "test.fm:2:29: error: 'expf' reserves 'fminus_rt_libm_expf' for the runtime's function, and this "
             "program declares a function of that name: rename it\n"
             "test.fm:1:7: note: 'fminus_rt_libm_expf' declared here\n"},
    };
    const std::array modes{std::pair{fminus::Program::Math::Vector, fminus::ir::Math::Vector},
                           std::pair{fminus::Program::Math::Libm, fminus::ir::Math::Libm},
                           std::pair{fminus::Program::Math::Fast, fminus::ir::Math::Fast}};
    for (const auto& [mode, lowering] : modes)
    {
        for (const Case& c : cases)
        {
            INFO("mode " << static_cast<int>(mode) << ":\n" << c.source);
            // The code generator alone, as fminus --emit obj and an executable meet it: an object file holds a
            // program's definition of a runtime function's name, which the JIT refuses as a second definition of the
            // runtime's symbol, so this is where the definitions above compiled.
            fminus::Diagnostics diagnostics("test.fm");
            const auto parsed = fminus::frontend::parse(c.source, diagnostics);
            REQUIRE(parsed);
            llvm::LLVMContext context;
            fminus::ir::Options irOptions;
            irOptions.math = lowering;
            CHECK(fminus::ir::lower(*parsed, context, diagnostics, irOptions) == nullptr);
            CHECK(diagnostics.format() == c.errors);
            // The JIT, through the public API, at -O2.
            fminus::Program::Options options;
            options.math = mode;
            options.fileName = "test.fm";
            std::string errors;
            CHECK(fminus::Program::compile(c.source, errors, options) == nullptr);
            CHECK(errors == c.errors);
        }
    }
}

TEST_CASE("a program's own functions of the runtime's math names are left alone where it calls none of their builtins",
          "[math]")
{
    // Prototypes of the runtime's own functions, called by name, as a program may call any of the runtime's: in the JIT
    // they are the runtime's. (A definition of one is the program's own code, which the JIT refuses as a second
    // definition of the runtime's symbol; an object file links it.) cosf beside them is the runtime's, in a loop too.
    const char* source = "float fminus_rt_sinf(float x);\n"
                         "float4 fminus_rt_sin_f4(float4 x);\n"
                         "float fminus_rt_libm_sinf(float x);\n"
                         "float own(float x) { return fminus_rt_sinf(x) + fminus_rt_libm_sinf(x); }\n"
                         "float4 own4(float4 x) { return fminus_rt_sin_f4(x); }\n"
                         "void cosines(const float* restrict in, float* restrict out, int n)\n"
                         "{\n"
                         "    for (int i = 0; i < n; ++i)\n"
                         "        out[i] = cosf(in[i]);\n"
                         "}\n";
    using float4 = float __attribute__((ext_vector_type(4)));
    for (const fminus::Program::Math mode :
         {fminus::Program::Math::Vector, fminus::Program::Math::Libm, fminus::Program::Math::Fast})
    {
        INFO("mode " << static_cast<int>(mode));
        fminus::Program::Options options;
        options.math = mode;
        std::string errors;
        const auto program = fminus::Program::compile(source, errors, options);
        INFO(errors);
        REQUIRE(program);
        auto* own = program->function<float(float)>("own", errors);
        auto* own4 = program->function<float4(float4)>("own4", errors);
        auto* cosines = program->function<void(const float*, float*, int)>("cosines", errors);
        REQUIRE((own != nullptr && own4 != nullptr && cosines != nullptr));
        Arguments arguments;
        std::vector<float> in(1001);
        for (float& x : in)
            x = arguments.uniform(-10.0f, 10.0f);
        std::vector<float> out(in.size());
        cosines(in.data(), out.data(), static_cast<int>(in.size()));
        long wrong = 0;
        for (std::size_t i = 0; i + 4 <= in.size(); i += 4)
        {
            const float4 x{in[i], in[i + 1], in[i + 2], in[i + 3]};
            const float4 y = own4(x);
            const fminus_rt_float4 runtime = fminus_rt_sin_f4(fminus_rt_float4{x[0], x[1], x[2], x[3]});
            for (int k = 0; k < 4; ++k)
                wrong += same(y[k], runtime[k]) ? 0 : 1;
        }
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            wrong += same(own(in[i]), fminus_rt_sinf(in[i]) + fminus_rt_libm_sinf(in[i])) ? 0 : 1;
            if (mode == fminus::Program::Math::Vector)
                wrong += same(out[i], fminus::math::cos(in[i])) ? 0 : 1;
            else if (mode == fminus::Program::Math::Libm)
                wrong += same(out[i], std::cos(in[i])) ? 0 : 1;
        }
        CHECK(wrong == 0);
    }

    // A definition of one, lowered: the program's code, with none of the runtime's attributes, and no builtin's vector
    // variant; cosf's runtime function beside it has them.
    fminus::Diagnostics diagnostics("test.fm");
    const auto parsed = fminus::frontend::parse("float fminus_rt_sinf(float x) { return x * 2.0f; }\n"
                                                "float4 fminus_rt_sin_f4(float4 x) { return x; }\n"
                                                "float f(float x) { return fminus_rt_sinf(x) + cosf(x); }\n",
                                                diagnostics);
    llvm::LLVMContext context;
    const std::unique_ptr<llvm::Module> module =
        parsed ? fminus::ir::lower(*parsed, context, diagnostics) : nullptr;
    INFO(diagnostics.format());
    REQUIRE(module);
    for (const char* name : {"fminus_rt_sinf", "fminus_rt_sin_f4"})
    {
        const llvm::Function* own = module->getFunction(name);
        REQUIRE(own != nullptr);
        INFO(name);
        CHECK_FALSE(own->isDeclaration());
        CHECK_FALSE(own->doesNotAccessMemory());
        CHECK_FALSE(own->willReturn());
        CHECK_FALSE(own->hasFnAttribute(llvm::Attribute::Speculatable));
        CHECK_FALSE(own->hasFnAttribute("vector-function-abi-variant"));
    }
    const llvm::Function* cosine = module->getFunction("fminus_rt_cosf");
    REQUIRE(cosine != nullptr);
    CHECK(cosine->doesNotAccessMemory());
    CHECK(cosine->hasFnAttribute(llvm::Attribute::Speculatable));
    CHECK(cosine->getFnAttribute("vector-function-abi-variant").getValueAsString()
          == "_ZGV_LLVM_N4v_fminus_rt_cosf(fminus_rt_cos_f4)");
}
