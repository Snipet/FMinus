// examples/saturator/bench.cpp: the saturator example's harness. It compiles saturator.fm through FMinus's library API
// (include/fminus/FMinus.h), as a plugin would, makes an instance of its module, Saturator, sets its drive (a knob),
// runs its four methods over noise, compares them with C++ versions of the same formulas, and times them all. With
// --check (what ctest runs) it only compares, and fails when:
//   - a method and its C++ version differ by more than --tolerance;
//   - a poly float method's output changes with the width, or is not the plain loop's (both are compiled at every
//     width from 1 to 64 lanes, and run on the same instance);
//   - calling a kernel a block at a time changes its output;
//   - the rational tanh is further from tanh (in double) than its comment promises;
//   - a method is not [[clang::nonblocking]].
// It checks over noise at --drive, and over a ramp from -10 to 10, which reaches past the rational tanh's clamp.
//
//     build-release/bin/example-saturator                    (time in the release preset)
//     build-dev/bin/example-saturator --poly-lanes 16        (a poly float of 16 lanes, whatever the machine's)
//     build-dev/bin/example-saturator -O1
//     build-dev/bin/example-saturator --math fast            (timed with LLVM's intrinsics for tanhf)
//
// The C++ versions are built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, they never
// fuse a multiply and an add, and the tanhf one calls fminus::math::tanh, tanhf as FMinus's default math mode computes
// it, wherever it does.
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    // C's aligned_alloc, and the free that goes with it. The Windows C runtime has no aligned_alloc: its aligned memory
    // comes from _aligned_malloc and goes back to _aligned_free, not free (as the FMinus runtime's does).
    void* alignedAlloc(std::size_t alignment, std::size_t size)
    {
#if defined(_WIN32)
        return _aligned_malloc(size, alignment);
#else
        return std::aligned_alloc(alignment, size);
#endif
    }

    void alignedFree(void* memory)
    {
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }

    // A C++ version's type: it saturates n samples from input into output, at `drive`.
    using Process = void(const float* input, float* output, int n, float drive);

    // A method's: it saturates n samples of the instance it runs on, at the instance's drive.
    using Method = void(void* saturator, const float* input, float* output, int n);

    // saturator.fm's methods, as FMinus compiled them, and their C++ versions (none for the poly floats: the C++ loops
    // are the same whichever way FMinus writes them).
    struct Kernel
    {
        const char* name;          // the FMinus method
        const char* description;   // a line of the timing table
        Method* fminus = nullptr;
        Process* cpp = nullptr;
    };

    struct Free
    {
        void operator()(void* memory) const
        {
            alignedFree(memory);
        }
    };

    // An instance of Saturator, as a host makes one: aligned and zeroed, its drive at its default. Every program this
    // harness compiles lays the module out alike (its one slot, the drive's), so one instance serves them all.
    // aligned_alloc takes an alignment of at least a pointer's on macOS, and a size that is a multiple of it.
    struct Instance
    {
        std::unique_ptr<void, Free> memory;
        const fminus::Program::Parameter* drive = nullptr;
    };

    Instance makeInstance(const fminus::Program& program)
    {
        const fminus::Program::Module* module = program.module("Saturator");
        Instance instance;
        if (module == nullptr || (instance.drive = program.parameter("Saturator::drive")) == nullptr)
            return instance;
        const std::size_t alignment = std::max(module->alignment, alignof(std::max_align_t));
        instance.memory.reset(alignedAlloc(alignment, (module->size + alignment - 1) / alignment * alignment));
        if (instance.memory)
        {
            std::memset(instance.memory.get(), 0, module->size);
            fminus::Program::setDefaults(instance.memory.get(), *module);
        }
        return instance;
    }

    // saturator.fm's rational_tanh, in the same order.
    float rationalTanh(float x)
    {
        const float clamped = std::fmin(std::fmax(x, -7.90531110763549805f), 7.90531110763549805f);
        const float x2 = clamped * clamped;
        float p = -2.76076847742355e-16f;
        p = p * x2 + 2.00018790482477e-13f;
        p = p * x2 + -8.60467152213735e-11f;
        p = p * x2 + 5.12229709037114e-08f;
        p = p * x2 + 1.48572235717979e-05f;
        p = p * x2 + 6.37261928875436e-04f;
        p = p * x2 + 4.89352455891786e-03f;
        float q = 1.19825839466702e-06f;
        q = q * x2 + 1.18534705686654e-04f;
        q = q * x2 + 2.26843463243900e-03f;
        q = q * x2 + 4.89352518554385e-03f;
        return clamped * p / q;
    }

    void cppTanhf(const float* __restrict input, float* __restrict output, int n, float drive)
    {
        for (int i = 0; i < n; ++i)
            output[i] = fminus::math::tanh(drive * input[i]);
    }

    void cppRational(const float* __restrict input, float* __restrict output, int n, float drive)
    {
        for (int i = 0; i < n; ++i)
            output[i] = rationalTanh(drive * input[i]);
    }

    // The largest absolute error saturator.fm's comment allows rational_tanh.
    constexpr double kRationalError = 3.5e-7;

    // How far apart the tanhf kernels' outputs may be when a block's end moves, or a poly float's width (check(),
    // below): none. FMinus's default math mode gives a float one tanhf wherever it is computed, in the plain loop's
    // vectorised body or in its remainder, in a poly float of any width (Program::Math, include/fminus/FMinus.h). In
    // the fast mode, whose vectorised body calls libsystem_m's _simd_tanh_f4 and whose remainder calls tanhf, it would
    // be 3 ulp, the most the two differ by over every finite float (macOS 27, an M5).
    constexpr std::int64_t kMaxTanhfUlps = 0;

    // White noise in [-1, 1) from a fixed 32-bit LCG.
    std::vector<float> makeNoise(std::size_t samples)
    {
        std::vector<float> noise(samples);
        std::uint32_t state = 0x12345678u;
        for (float& x : noise)
        {
            state = state * 1664525u + 1013904223u;
            x = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;   // 24 bits: [0, 2) - 1
        }
        return noise;
    }

    // From -10 to 10 in equal steps: through 0, and past the rational tanh's clamp at +-7.9 on both sides.
    std::vector<float> makeRamp(std::size_t samples)
    {
        std::vector<float> ramp(samples);
        for (std::size_t i = 0; i < samples; ++i)
            ramp[i] = static_cast<float>(-10.0 + 20.0 * static_cast<double>(i) / static_cast<double>(samples - 1));
        return ramp;
    }

    // tanh of each of the kernels' arguments, drive * x rounded to float as they round it, in double.
    std::vector<double> exactTanh(const std::vector<float>& input, float drive)
    {
        std::vector<double> exact(input.size());
        for (std::size_t i = 0; i < input.size(); ++i)
            exact[i] = std::tanh(static_cast<double>(drive * input[i]));
        return exact;
    }

    // A C++ version over the whole input, one call a block of `block` samples (0: one call for all of it), as a
    // plugin calls it once a host buffer.
    void run(Process* process, const std::vector<float>& input, float drive, std::size_t block,
             std::vector<float>& output)
    {
        const std::size_t step = block == 0 ? input.size() : block;
        for (std::size_t start = 0; start < input.size(); start += step)
        {
            const std::size_t n = std::min(step, input.size() - start);
            process(input.data() + start, output.data() + start, static_cast<int>(n), drive);
        }
    }

    // A method, the same way, on `instance` with its drive set to `drive` (the knob, which each call reads once).
    void run(Method* process, const Instance& instance, const std::vector<float>& input, float drive,
             std::size_t block, std::vector<float>& output)
    {
        fminus::Program::setValue(instance.memory.get(), *instance.drive, static_cast<double>(drive));
        const std::size_t step = block == 0 ? input.size() : block;
        for (std::size_t start = 0; start < input.size(); start += step)
        {
            const std::size_t n = std::min(step, input.size() - start);
            process(instance.memory.get(), input.data() + start, output.data() + start, static_cast<int>(n));
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

    // The largest distance between a and b in units in the last place: how many floats lie between the two, +0 and -0
    // the same. Their bit patterns, made to count up with the float through 0 (negative floats count down in theirs),
    // are integers that far apart.
    std::int64_t maxUlps(const std::vector<float>& a, const std::vector<float>& b)
    {
        const auto ordered = [](float x) {
            const auto bits = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(x));
            return bits < 0 ? std::int64_t{std::numeric_limits<std::int32_t>::min()} - bits : bits;
        };
        std::int64_t worst = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            worst = std::max(worst, std::abs(ordered(a[i]) - ordered(b[i])));
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

    // saturator.fm compiled at -O`level` with poly floats of `lanes` (0: this machine's) and tanhf in the math mode
    // `math`, and its four methods; or nullptr, with the errors.
    std::unique_ptr<fminus::Program> compile(const std::string& path, const std::string& source, int level,
                                             fminus::Program::Math math, unsigned lanes, std::array<Kernel, 4>& kernels,
                                             std::string& errors)
    {
        fminus::Program::Options options;
        options.optimisation = level;
        options.polyLanes = lanes;
        options.math = math;
        options.fileName = path;
        std::unique_ptr<fminus::Program> program = fminus::Program::compile(source, errors, options);
        if (!program)
            return nullptr;
        for (Kernel& kernel : kernels)
        {
            kernel.fminus = program->function<Method>(kernel.name, errors);
            if (kernel.fminus == nullptr)
                return nullptr;
        }
        return program;
    }

    // Everything --check checks, over one input. Prints a line, and returns whether it all held.
    bool check(const std::string& path, const std::string& source, int level, fminus::Program::Math math,
               const std::array<Kernel, 4>& kernels, const Instance& instance, const std::string& what,
               const std::vector<float>& input, float drive, double tolerance)
    {
        const std::size_t samples = input.size();
        std::vector<float> fminusOutput(samples);
        std::vector<float> cppOutput(samples);
        std::vector<float> other(samples);

        double worstAgainstCpp = 0.0;
        bool sameInBlocks = true;
        std::int64_t tanhfBlockUlps = 0;
        for (const Kernel& kernel : kernels)
        {
            run(kernel.fminus, instance, input, drive, 0, fminusOutput);
            run(kernel.cpp, input, drive, 0, cppOutput);
            worstAgainstCpp = std::max(worstAgainstCpp, maxDifference(fminusOutput, cppOutput));
            // Calls of 64 samples, then of 61 (blocks that end mid-way through a poly float), give the output one
            // call does, bit for bit: the rational tanh is FMinus's own arithmetic, and tanhf the runtime's, whose
            // vector function gives each lane what its scalar one gives a float (the default math mode). The loop
            // vectoriser runs the plain loop's body on the one and its remainder on the other, so in the fast mode,
            // with the C library's two, where a block ends would decide which one a sample meets.
            const bool library = kernel.cpp == cppTanhf;
            for (const std::size_t block : {std::size_t{64}, std::size_t{61}})
            {
                run(kernel.fminus, instance, input, drive, block, other);
                const std::int64_t ulps = maxUlps(other, fminusOutput);
                if (library)
                    tanhfBlockUlps = std::max(tanhfBlockUlps, ulps);
                sameInBlocks = sameInBlocks && ulps <= (library ? kMaxTanhfUlps : 0);
            }
        }

        // The rational tanh's poly floats are lane-independent: every width gives the scalar loop's output. So are the
        // tanhf poly loop's: at every width, C++'s fminus::math::tanh (within kMaxTanhfUlps, none).
        std::vector<float> scalar(samples);
        run(kernels[2].fminus, instance, input, drive, 0, scalar);
        std::vector<float> libraryTanh(samples);
        run(cppTanhf, input, drive, 0, libraryTanh);
        bool sameAtEveryWidth = true;
        std::int64_t tanhfWidthUlps = 0;
        for (unsigned lanes = 1; lanes <= 64; lanes *= 2)
        {
            std::array<Kernel, 4> atWidth = kernels;
            std::string errors;
            const auto program = compile(path, source, level, math, lanes, atWidth, errors);
            if (!program)
            {
                std::cerr << "example-saturator: " << errors;
                return false;
            }
            run(atWidth[3].fminus, instance, input, drive, 0, other);
            sameAtEveryWidth = sameAtEveryWidth && other == scalar;
            run(atWidth[3].fminus, instance, input, drive, 61, other);
            sameAtEveryWidth = sameAtEveryWidth && other == scalar;
            run(atWidth[1].fminus, instance, input, drive, 61, other);
            tanhfWidthUlps = std::max(tanhfWidthUlps, maxUlps(other, libraryTanh));
        }

        const std::vector<double> exact = exactTanh(input, drive);
        const double rationalError = maxDifference(scalar, exact);
        run(kernels[0].fminus, instance, input, drive, 0, fminusOutput);
        const double tanhfError = maxDifference(fminusOutput, exact);

        std::cout << std::format("example-saturator: {}: max |FMinus - C++| = {:.3g} (tolerance {:.3g}); "
                                 "the same output in blocks of 64 and 61: {} (tanhf's within {} ulp: {}), and at 1 "
                                 "to 64 lanes: {} (tanhf's poly within {} ulp of C++'s tanhf: {}); max |tanh - "
                                 "exact|: tanhf {:.3g}, rational {:.3g} (at most {:.3g})\n",
                                 what, worstAgainstCpp, tolerance, sameInBlocks ? "yes" : "no", kMaxTanhfUlps,
                                 tanhfBlockUlps, sameAtEveryWidth ? "yes" : "no", kMaxTanhfUlps, tanhfWidthUlps,
                                 tanhfError, rationalError, kRationalError);
        return worstAgainstCpp <= tolerance && sameInBlocks && sameAtEveryWidth && tanhfWidthUlps <= kMaxTanhfUlps
               && rationalError <= kRationalError;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The saturator example: saturator.fm against C++, compared and timed.", "example-saturator"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/saturator.fm";
    std::size_t samples = 48000;
    std::size_t block = 0;
    int repeats = 50;
    int level = 2;
    unsigned lanes = 0;
    float drive = 4.0f;
    bool checkOnly = false;
    double tolerance = 1e-6;
    std::string math = "vector";
    app.add_option("source", path, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{2}, static_cast<std::size_t>(std::numeric_limits<int>::max())))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call to FMinus (0: one call for the whole run)")
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--poly-lanes", lanes, "A poly float's lanes: 1, 2, 4, ... 64 (0: this machine's)")
        ->check(CLI::IsMember({0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u}))
        ->capture_default_str();
    app.add_option("--drive", drive, "The gain before the tanh")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--math", math, "How FMinus computes tanhf: vector, libm or fast (--check expects vector)")
        ->check(CLI::IsMember({"vector", "libm", "fast"}))
        ->capture_default_str();
    app.add_flag("--check", checkOnly, "Compare only: exit 1 when a comparison fails");
    app.add_option("--tolerance", tolerance, "The largest |FMinus - C++| --check accepts")->capture_default_str();
    CLI11_PARSE(app, argc, argv);
    // --check holds the vector mode's promise, which the fast mode breaks.
    const fminus::Program::Math mode = math == "libm"   ? fminus::Program::Math::Libm
                                       : math == "fast" ? fminus::Program::Math::Fast
                                                        : fminus::Program::Math::Vector;

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::cerr << "example-saturator: cannot read " << path << "\n";
        return 1;
    }
    std::ostringstream text;
    text << file.rdbuf();
    const std::string source = text.str();

    std::array<Kernel, 4> kernels{
        Kernel{"Saturator::process_tanhf", "tanhf, one sample at a time", nullptr, cppTanhf},
        Kernel{"Saturator::process_tanhf_poly", "tanhf, a poly float at a time", nullptr, cppTanhf},
        Kernel{"Saturator::process", "rational tanh, one sample at a time", nullptr, cppRational},
        Kernel{"Saturator::process_poly", "rational tanh, a poly float at a time", nullptr, cppRational},
    };
    std::string errors;
    const std::unique_ptr<fminus::Program> program = compile(path, source, level, mode, lanes, kernels, errors);
    if (!program)
    {
        std::cerr << "example-saturator: " << errors;
        return 1;
    }
    const Instance instance = makeInstance(*program);
    if (!instance.memory)
    {
        std::cerr << "example-saturator: " << path << " has no module Saturator with a parameter drive\n";
        return 1;
    }

    if (checkOnly)
    {
        bool nonBlocking = true;
        for (const Kernel& kernel : kernels)
            nonBlocking = nonBlocking && program->nonBlocking(kernel.name);
        const bool noise = check(path, source, level, mode, kernels, instance,
                                 std::format("noise at drive {}", drive), makeNoise(samples), drive, tolerance);
        const bool ramp = check(path, source, level, mode, kernels, instance, "ramp from -10 to 10",
                                makeRamp(samples), 1.0f, tolerance);
        if (!nonBlocking)
            std::cout << "example-saturator: a method is not [[clang::nonblocking]]\n";
        return noise && ramp && nonBlocking ? 0 : 1;
    }

    const std::vector<float> input = makeNoise(samples);
    const std::vector<double> exact = exactTanh(input, drive);
    std::vector<float> output(samples);
    std::cout << std::format(
        "tanh saturator, drive {}: {} samples of noise, {}; a poly float has {} lanes{}\n", drive, samples,
        block == 0 || block >= samples ? "one call for all of them" : std::format("one call a block of {}", block),
        program->polyLanes(), lanes == 0 ? " (this machine's)" : "");
    std::cout << std::format("  {:<40}{:>16}{:>16}{:>14}\n", "ns/sample", std::format("FMinus -O{}", level), "C++",
                             "max |error|");
    for (const Kernel& kernel : kernels)
    {
        const double fminusTime =
            nanosecondsPerSample([&] { run(kernel.fminus, instance, input, drive, block, output); }, samples, repeats);
        const double error = maxDifference(output, exact);
        // The C++ loops are the same for a kernel and its poly float version: time them once.
        const bool poly = std::string_view(kernel.name).ends_with("_poly");   // the same C++ as the one above
        const std::string cppTime =
            poly ? std::string("")
                 : std::format("{:.2f}", nanosecondsPerSample([&] { run(kernel.cpp, input, drive, block, output); },
                                                              samples, repeats));
        std::cout << std::format("  {:<40}{:>16.2f}{:>16}{:>14.2g}\n", kernel.description, fminusTime, cppTime, error);
    }
    std::cout << "  (max |error|: FMinus's, against tanh in double)\n";
    return 0;
}
