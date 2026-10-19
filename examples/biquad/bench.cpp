// examples/biquad/bench.cpp: the biquad example's harness. It JIT-compiles biquad.fm, runs the filter over noise with
// its cutoff swept every sample, compares the result with a C++ version of the same formulas, and times both. With
// --check (what ctest runs) it only compares, and fails when the two differ by more than --tolerance.
//
// As FMinus grows, move work from here into biquad.fm (README.md suggests an order), and watch the numbers:
//     build-release/bin/example-biquad               (time in the release preset)
//     build-dev/bin/example-biquad -O0 --samples 480000
//
// The C++ version is built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, it never
// fuses a multiply and an add.
#include "frontend/Frontend.h"
#include "ir/CodeGen.h"
#include "jit/Jit.h"

#include <CLI/CLI.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    // FMinus's float4 at the C boundary: clang's ext_vector_type, in one register (docs/simd.md).
    using float4 = float __attribute__((ext_vector_type(4)));

    // The FMinus kernel. One sample is one call, which returns the output and the two new states as (y, s1, s2, 0).
    struct Kernel
    {
        float4 (*lowpass)(float x, float s1, float s2, float cutoff, float q, float rate) = nullptr;
    };

    struct Signal
    {
        std::vector<float> input;    // white noise in [-1, 1)
        std::vector<float> cutoff;   // in Hz, one per sample
    };

    // Noise from a fixed 32-bit LCG, and a cutoff swept exponentially between 200 Hz and 8 kHz by a 3 Hz sine.
    Signal makeSignal(std::size_t samples, float rate)
    {
        Signal signal{std::vector<float>(samples), std::vector<float>(samples)};
        std::uint32_t state = 0x12345678u;
        for (std::size_t i = 0; i < samples; ++i)
        {
            state = state * 1664525u + 1013904223u;
            signal.input[i] = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;   // 24 bits: [0, 2) - 1
            const double phase = 2.0 * std::numbers::pi * 3.0 * static_cast<double>(i) / static_cast<double>(rate);
            signal.cutoff[i] = static_cast<float>(200.0 * std::pow(40.0, 0.5 + 0.5 * std::sin(phase)));
        }
        return signal;
    }

    // biquad.fm's formulas, in the same order, in T: float for the comparison, double for float's own error.
    template <typename T>
    struct Reference
    {
        static T omega(T cutoff, T rate)
        {
            return T(2) * std::numbers::pi_v<T> * cutoff / rate;
        }
        static T alpha(T cutoff, T q, T rate)
        {
            return std::sin(omega(cutoff, rate)) / (T(2) * q);
        }
        static T b0(T cutoff, T q, T rate)
        {
            const T c = std::cos(omega(cutoff, rate));
            return (T(1) - c) / T(2) / (T(1) + alpha(cutoff, q, rate));
        }
        static T b1(T cutoff, T q, T rate)
        {
            const T c = std::cos(omega(cutoff, rate));
            return (T(1) - c) / (T(1) + alpha(cutoff, q, rate));
        }
        static T a1(T cutoff, T q, T rate)
        {
            return T(-2) * std::cos(omega(cutoff, rate)) / (T(1) + alpha(cutoff, q, rate));
        }
        static T a2(T cutoff, T q, T rate)
        {
            const T a = alpha(cutoff, q, rate);
            return (T(1) - a) / (T(1) + a);
        }

        static void run(const Signal& signal, T q, T rate, std::vector<T>& output)
        {
            T s1 = 0;
            T s2 = 0;
            for (std::size_t i = 0; i < output.size(); ++i)
            {
                const T x = static_cast<T>(signal.input[i]);
                const T cutoff = static_cast<T>(signal.cutoff[i]);
                const T y = b0(cutoff, q, rate) * x + s1;
                s1 = b1(cutoff, q, rate) * x - a1(cutoff, q, rate) * y + s2;
                s2 = b0(cutoff, q, rate) * x - a2(cutoff, q, rate) * y;
                output[i] = y;
            }
        }
    };

    void runKernel(const Kernel& kernel, const Signal& signal, float q, float rate, std::vector<float>& output)
    {
        float s1 = 0.0f;
        float s2 = 0.0f;
        for (std::size_t i = 0; i < output.size(); ++i)
        {
            const float x = signal.input[i];
            const float cutoff = signal.cutoff[i];
            const float4 step = kernel.lowpass(x, s1, s2, cutoff, q, rate);
            output[i] = step.x;
            s1 = step.y;
            s2 = step.z;
        }
    }

    // The largest |a - b|; infinity when either is not finite.
    template <typename A, typename B>
    double maxDifference(const std::vector<A>& a, const std::vector<B>& b)
    {
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const double difference = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
            if (!std::isfinite(difference))
                return std::numeric_limits<double>::infinity();
            worst = std::max(worst, difference);
        }
        return worst;
    }

    // The best of `repeats` runs, in nanoseconds per sample.
    template <typename Run>
    double nanosecondsPerSample(Run&& run, std::size_t samples, int repeats)
    {
        double best = std::numeric_limits<double>::infinity();
        for (int r = 0; r < repeats; ++r)
        {
            const auto start = std::chrono::steady_clock::now();
            run();
            const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
            best = std::min(best, elapsed.count());
        }
        return best / static_cast<double>(samples);
    }

    int fail(llvm::Error error)
    {
        llvm::errs() << "example-biquad: " << llvm::toString(std::move(error)) << "\n";
        return 1;
    }

    // Compiles `path` at `level` into `jit`, and finds the kernel.
    llvm::Error compile(const std::string& path, llvm::OptimizationLevel level, std::unique_ptr<fminus::jit::Jit>& jit,
                        Kernel& kernel)
    {
        auto source = llvm::MemoryBuffer::getFile(path);
        if (!source)
            return llvm::createStringError(source.getError(), "cannot read " + path);
        fminus::Diagnostics diagnostics(path);
        const auto program = fminus::frontend::parse((*source)->getBuffer(), diagnostics);
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module = program ? fminus::ir::lower(*program, *context, diagnostics) : nullptr;
        if (!module)
            return llvm::createStringError("\n" + diagnostics.format());

        auto created = fminus::jit::Jit::create(level);
        if (!created)
            return created.takeError();
        jit = std::move(*created);
        if (llvm::Error error = jit->add(std::move(module), std::move(context)))
            return error;

        auto find = [&](const char* name, auto& function) -> llvm::Error {
            auto address = jit->lookup(name);
            if (!address)
                return address.takeError();
            function = address->toPtr<std::remove_reference_t<decltype(function)>>();
            return llvm::Error::success();
        };
        return find("lowpass", kernel.lowpass);
    }
} // namespace

int main(int argc, char** argv)
{
    llvm::InitLLVM initLLVM(argc, argv);

    CLI::App app{"The biquad example: biquad.fm against C++, compared and timed.", "example-biquad"};
    std::string source = std::string(FMINUS_EXAMPLE_DIR) + "/biquad.fm";
    std::size_t samples = 48000;
    int repeats = 20;
    int level = 2;
    float q = 0.7071f;
    float rate = 48000.0f;
    bool check = false;
    double tolerance = 1e-5;
    app.add_option("source", source, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--q", q, "The filter's Q")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--rate", rate, "The sample rate, Hz")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus and C++ differ by more than --tolerance");
    app.add_option("--tolerance", tolerance, "The largest |FMinus - C++| --check accepts")->capture_default_str();
    CLI11_PARSE(app, argc, argv);

    const std::array<llvm::OptimizationLevel, 4> levels{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                        llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
    std::unique_ptr<fminus::jit::Jit> jit;
    Kernel kernel;
    if (llvm::Error error = compile(source, levels[static_cast<std::size_t>(level)], jit, kernel))
        return fail(std::move(error));

    const Signal signal = makeSignal(samples, rate);
    std::vector<float> fminusOutput(samples);
    std::vector<float> cppOutput(samples);
    std::vector<double> doubleOutput(samples);
    auto runFminus = [&] { runKernel(kernel, signal, q, rate, fminusOutput); };
    auto runCpp = [&] { Reference<float>::run(signal, q, rate, cppOutput); };
    runFminus();
    runCpp();
    Reference<double>::run(signal, static_cast<double>(q), static_cast<double>(rate), doubleOutput);
    const double difference = maxDifference(fminusOutput, cppOutput);
    const double floatError = maxDifference(cppOutput, doubleOutput);

    if (check)
    {
        llvm::outs() << std::format("example-biquad: max |FMinus - C++| = {:.3g} over {} samples (tolerance {:.3g})\n",
                                    difference, samples, tolerance);
        return difference <= tolerance ? 0 : 1;
    }

    const double fminusTime = nanosecondsPerSample(runFminus, samples, repeats);
    const double cppTime = nanosecondsPerSample(runCpp, samples, repeats);
    llvm::outs() << std::format(
        "biquad low-pass, cutoff swept 200 Hz to 8 kHz every sample: {} samples at {} Hz, Q {}\n"
        "  FMinus (JIT, -O{}) {:8.2f} ns/sample   1 call a sample, returning (y, s1, s2) in a float4\n"
        "  C++ (clang)       {:8.2f} ns/sample   the same formulas, inlined into one loop\n"
        "  FMinus / C++      {:8.2f}x\n"
        "  max |FMinus - C++| = {:.3g}; float's own error (C++ float - double) = {:.3g}\n",
        samples, rate, q, level, fminusTime, cppTime, fminusTime / cppTime, difference, floatError);
    return 0;
}
