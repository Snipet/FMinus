// examples/biquad/bench.cpp: the biquad example's harness. It compiles biquad.fm through fminus::Program, runs the
// filter over noise with its cutoff swept every sample, compares the result with a C++ version of the same formulas,
// and times both. With --check (what ctest runs) it only compares, and fails when the two differ by more than
// --tolerance, when calling the kernel a block at a time changes its output, when process is not
// [[clang::nonblocking]], or when a program compiled with other parameters (q 2 as a knob, the rate frozen at 44.1 kHz)
// is not C++ at those values: a knob or a frozen value that never reaches the kernel fails it.
//
// The filter is the module Biquad, in memory the harness allocates, as a plugin does for each instance. Its cutoff is
// a stream, the harness's buffer of one cutoff a sample; its q a knob, set before the first call; its sample rate
// frozen at --rate. The harness makes one call for the whole signal (or, with --block, one a block): the loop over the
// samples is the kernel's. As FMinus grows, move work from here into biquad.fm (README.md suggests an order), and
// watch the numbers:
//     build-release/bin/example-biquad               (time in the release preset)
//     build-dev/bin/example-biquad -O0 --samples 480000
//
// The C++ version is built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, it never
// fuses a multiply and an add.
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace
{
    // The FMinus kernel: Biquad::process filters n samples of an instance, whose state carries over to its next call.
    struct Kernel
    {
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* biquad = nullptr;
        const fminus::Program::Parameter* cutoff = nullptr;
        const fminus::Program::Parameter* q = nullptr;
        void (*process)(void* biquad, const float* input, float* output, int n) = nullptr;
    };

    struct Free
    {
        void operator()(void* memory) const
        {
            std::free(memory);
        }
    };

    // An instance, as a host makes one: aligned and zeroed (the state starts at 0), each parameter at its default.
    // std::aligned_alloc takes an alignment of at least a pointer's on macOS, and a size that is a multiple of it.
    std::unique_ptr<void, Free> instance(const fminus::Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        std::unique_ptr<void, Free> memory(
            std::aligned_alloc(alignment, (module.size + alignment - 1) / alignment * alignment));
        if (memory)
        {
            std::memset(memory.get(), 0, module.size);
            fminus::Program::setDefaults(memory.get(), module);
        }
        return memory;
    }

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

    // The kernel over the whole signal, one call a block of `block` samples (0: one call for all of it), as a plugin
    // calls it once a host buffer: the cutoff's stream at the block's cutoffs, then the call. The state carries over
    // from one call to the next.
    void runKernel(const Kernel& kernel, const Signal& signal, float q, std::size_t block, std::vector<float>& output)
    {
        const std::unique_ptr<void, Free> biquad = instance(*kernel.biquad);
        if (!biquad)
            return;
        fminus::Program::setValue(biquad.get(), *kernel.q, static_cast<double>(q));
        const std::size_t step = block == 0 ? output.size() : block;
        for (std::size_t start = 0; start < output.size(); start += step)
        {
            const std::size_t n = std::min(step, output.size() - start);
            fminus::Program::setStream(biquad.get(), *kernel.cutoff, signal.cutoff.data() + start);
            kernel.process(biquad.get(), signal.input.data() + start, output.data() + start, static_cast<int>(n));
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

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-biquad: %s\n", message.c_str());
        return 1;
    }

    // Compiles `path` at `level`, the cutoff a stream and the rate frozen, and finds the kernel.
    bool compile(const std::string& path, int level, float rate, Kernel& kernel, std::string& errors)
    {
        std::ifstream file(path);
        if (!file)
        {
            errors = "cannot read " + path;
            return false;
        }
        const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        fminus::Program::Options options;
        options.optimisation = level;
        options.fileName = path;
        options.parameters["Biquad::cutoff"] = fminus::Program::Binding::stream();
        options.parameters["Biquad::rate"] = fminus::Program::Binding::frozen(static_cast<double>(rate));
        kernel.program = fminus::Program::compile(source, errors, options);
        if (!kernel.program)
            return false;
        kernel.biquad = kernel.program->module("Biquad");
        kernel.cutoff = kernel.program->parameter("Biquad::cutoff");
        kernel.q = kernel.program->parameter("Biquad::q");
        kernel.process = kernel.program->function<void(void*, const float*, float*, int)>("Biquad::process", errors);
        if (kernel.biquad == nullptr || kernel.cutoff == nullptr || kernel.q == nullptr)
            errors = path + " has no module Biquad with the parameters cutoff and q";
        return kernel.biquad != nullptr && kernel.cutoff != nullptr && kernel.q != nullptr
               && kernel.process != nullptr;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The biquad example: biquad.fm against C++, compared and timed.", "example-biquad"};
    std::string source = std::string(FMINUS_EXAMPLE_DIR) + "/biquad.fm";
    std::size_t samples = 48000;
    std::size_t block = 0;
    int repeats = 20;
    int level = 2;
    float q = 0.7071f;
    float rate = 48000.0f;
    bool check = false;
    double tolerance = 1e-5;
    app.add_option("source", source, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{1}, static_cast<std::size_t>(std::numeric_limits<int>::max())))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call to FMinus (0: one call for the whole run)")
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--q", q, "The filter's Q")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--rate", rate, "The sample rate, Hz")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus and C++ differ by more than --tolerance");
    app.add_option("--tolerance", tolerance, "The largest |FMinus - C++| --check accepts")->capture_default_str();
    CLI11_PARSE(app, argc, argv);

    Kernel kernel;
    std::string errors;
    if (!compile(source, level, rate, kernel, errors))
        return fail("\n" + errors);

    const Signal signal = makeSignal(samples, rate);
    std::vector<float> fminusOutput(samples);
    std::vector<float> blockOutput(samples);
    std::vector<float> cppOutput(samples);
    std::vector<double> doubleOutput(samples);
    auto runFminus = [&] { runKernel(kernel, signal, q, block, fminusOutput); };
    auto runCpp = [&] { Reference<float>::run(signal, q, rate, cppOutput); };
    runFminus();
    runCpp();
    Reference<double>::run(signal, static_cast<double>(q), static_cast<double>(rate), doubleOutput);
    const double difference = maxDifference(fminusOutput, cppOutput);
    const double floatError = maxDifference(cppOutput, doubleOutput);

    if (check)
    {
        // Calls of 64 samples, then of 61 (blocks that end mid-way through), must give the output one call does.
        bool sameInBlocks = true;
        for (const std::size_t size : {std::size_t{64}, std::size_t{61}})
        {
            runKernel(kernel, signal, q, size, blockOutput);
            sameInBlocks = sameInBlocks && blockOutput == fminusOutput;
        }
        const bool nonBlocking = kernel.program->nonBlocking("Biquad::process");
        // The parameters reach the kernel: other values, through the same bindings, give C++'s output at them.
        constexpr float kOtherQ = 2.0f;
        constexpr float kOtherRate = 44100.0f;
        Kernel other;
        if (!compile(source, level, kOtherRate, other, errors))
            return fail("\n" + errors);
        runKernel(other, signal, kOtherQ, 0, blockOutput);
        Reference<float>::run(signal, kOtherQ, kOtherRate, cppOutput);
        const double otherDifference = maxDifference(blockOutput, cppOutput);
        std::printf("%s", std::format("example-biquad: max |FMinus - C++| = {:.3g} over {} samples (tolerance {:.3g}); "
                                      "in blocks of 64 and 61, the same output: {}; process nonblocking: {}; at Q {} "
                                      "and {} Hz, max |FMinus - C++| = {:.3g}\n",
                                      difference, samples, tolerance, sameInBlocks ? "yes" : "no",
                                      nonBlocking ? "yes" : "no", kOtherQ, kOtherRate, otherDifference)
                              .c_str());
        return difference <= tolerance && sameInBlocks && nonBlocking && otherDifference <= tolerance ? 0 : 1;
    }

    const double fminusTime = nanosecondsPerSample(runFminus, samples, repeats);
    const double cppTime = nanosecondsPerSample(runCpp, samples, repeats);
    const std::string calls = block == 0 || block >= samples ? std::format("1 call for all {} samples", samples)
                                                             : std::format("1 call a block of {}", block);
    std::printf("%s", std::format("biquad low-pass, cutoff swept 200 Hz to 8 kHz every sample: {} samples at {} Hz, "
                                  "Q {}\n"
                                  "  FMinus (JIT, -O{}) {:8.2f} ns/sample   {}, the loop FMinus's\n"
                                  "  C++ (clang)       {:8.2f} ns/sample   the same formulas, inlined into one loop\n"
                                  "  FMinus / C++      {:8.2f}x\n"
                                  "  max |FMinus - C++| = {:.3g}; float's own error (C++ float - double) = {:.3g}\n",
                                  samples, rate, q, level, fminusTime, calls, cppTime, fminusTime / cppTime,
                                  difference, floatError)
                          .c_str());
    return 0;
}
