// examples/meter/bench.cpp: the meter example's harness. It compiles meter.fm through FMinus's library API
// (include/fminus/FMinus.h), as a plugin would, runs its four kernels over noise, one reading a block, compares the
// readings with C++, and times them; and the same for its module, Meter, whose held peak falls by its parameter,
// decay. With --check (what ctest runs) it only compares, and fails when:
//   - peak, rms or peak_poly differs from its C++ version by more than --tolerance;
//   - rms_poly differs from a C++ emulation of its order of additions, at any width from 1 to 64 lanes;
//   - peak_poly's reading changes with the width;
//   - Meter's fields, after each block, are not a C++ meter's in the same order, over fading noise; or, after an
//     impulse and a silent block, its held peak is not --decay (the knob not reaching the module);
//   - a kernel is not [[clang::nonblocking]].
// It checks with one reading for the whole signal, and one a block of 64 and of 61 (blocks whose last poly float is
// partial), and with blocks of 0 to 3 samples.
//
//     build-release/bin/example-meter                        (time in the release preset)
//     build-dev/bin/example-meter --poly-lanes 32            (a poly float of 32 lanes, whatever the machine's)
//     build-dev/bin/example-meter --block 256                (one reading a block of 256)
//
// The C++ versions are built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, they never
// fuse a multiply and an add.
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
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

    // Every kernel's C type: a reading of the n samples at input.
    using Measure = float(const float* input, int n);

    // meter.fm's kernels, as FMinus compiled them, and their C++ versions.
    struct Kernel
    {
        const char* name;          // the FMinus function
        const char* description;   // a line of the timing table
        Measure* fminus = nullptr;
        Measure* cpp = nullptr;   // nullptr for rms_poly: its C++ version depends on the width (rmsInLanes)
    };

    float cppPeak(const float* __restrict input, int n)
    {
        float largest = 0;
        for (int i = 0; i < n; ++i)
            largest = std::fmax(largest, std::fabs(input[i]));
        return largest;
    }

    float cppRms(const float* __restrict input, int n)
    {
        float sum = 0;
        for (int i = 0; i < n; ++i)
            sum += input[i] * input[i];
        return n > 0 ? std::sqrt(sum / static_cast<float>(n)) : 0.0f;
    }

    // rms_poly's arithmetic at `lanes` lanes, in its order: lane l adds the squares of samples l, l + lanes,
    // l + 2 lanes, ... (the last step adds 0 in the lanes past n, which changes nothing), and reduce_add adds the lanes
    // in order, from -0.
    float rmsInLanes(const float* input, int n, unsigned lanes)
    {
        std::vector<float> sums(lanes, 0.0f);
        for (int i = 0; i < n; ++i)
            sums[static_cast<std::size_t>(i) % lanes] += input[i] * input[i];
        float sum = -0.0f;
        for (const float lane : sums)
            sum += lane;
        return n > 0 ? std::sqrt(sum / static_cast<float>(n)) : 0.0f;
    }

    struct Free
    {
        void operator()(void* memory) const
        {
            alignedFree(memory);
        }
    };

    // An instance of meter.fm's Meter, and where its readings are.
    struct Meter
    {
        std::unique_ptr<void, Free> memory;
        const fminus::Program::Module* module = nullptr;
        const fminus::Program::Parameter* decay = nullptr;
        std::size_t heldOffset = 0;
        std::size_t levelOffset = 0;
        void (*measure)(void* meter, const float* input, int n) = nullptr;

        // Zeroed, as a host makes it, its decay at `value`.
        void reset(float value) const
        {
            std::memset(memory.get(), 0, module->size);
            fminus::Program::setDefaults(memory.get(), *module);
            fminus::Program::setValue(memory.get(), *decay, static_cast<double>(value));
        }

        float field(std::size_t offset) const
        {
            float value = 0.0f;
            std::memcpy(&value, static_cast<const unsigned char*>(memory.get()) + offset, sizeof value);
            return value;
        }
    };

    // The program's Meter, in memory a host allocates: aligned, and big enough. aligned_alloc takes an
    // alignment of at least a pointer's on macOS, and a size that is a multiple of it.
    Meter makeMeter(fminus::Program& program, std::string& errors)
    {
        Meter meter;
        meter.module = program.module("Meter");
        meter.decay = program.parameter("Meter::decay");
        meter.measure = program.function<void(void*, const float*, int)>("Meter::measure", errors);
        if (meter.module == nullptr || meter.decay == nullptr || meter.measure == nullptr)
            return meter;
        for (const fminus::Program::Module::Field& field : meter.module->fields)
        {
            if (field.name == "held")
                meter.heldOffset = field.offset;
            else if (field.name == "level")
                meter.levelOffset = field.offset;
        }
        const std::size_t alignment = std::max(meter.module->alignment, alignof(std::max_align_t));
        meter.memory.reset(
            alignedAlloc(alignment, (meter.module->size + alignment - 1) / alignment * alignment));
        return meter;
    }

    // Meter, a block of `block` samples at a time (0: one for the whole input), from zero: its two fields after each
    // block, held peaks then levels.
    std::vector<float> runMeter(const Meter& meter, const std::vector<float>& input, float decay, std::size_t block)
    {
        meter.reset(decay);
        std::vector<float> fields;
        const std::size_t step = block == 0 ? input.size() : block;
        for (std::size_t start = 0; start < input.size(); start += step)
        {
            meter.measure(meter.memory.get(), input.data() + start,
                          static_cast<int>(std::min(step, input.size() - start)));
            fields.push_back(meter.field(meter.heldOffset));
            fields.push_back(meter.field(meter.levelOffset));
        }
        return fields;
    }

    // The same meter in C++, its RMS added in rms_poly's order at `lanes` lanes.
    std::vector<float> cppMeter(const std::vector<float>& input, float decay, std::size_t block, unsigned lanes)
    {
        float held = 0.0f;
        std::vector<float> fields;
        const std::size_t step = block == 0 ? input.size() : block;
        for (std::size_t start = 0; start < input.size(); start += step)
        {
            const int n = static_cast<int>(std::min(step, input.size() - start));
            held = std::fmax(cppPeak(input.data() + start, n), held * decay);
            fields.push_back(held);
            fields.push_back(rmsInLanes(input.data() + start, n, lanes));
        }
        return fields;
    }

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

    // One reading a block of `block` samples (0: one for the whole input), as a meter reads once a host buffer.
    template <typename Read>
    void run(Read&& read, const std::vector<float>& input, std::size_t block, std::vector<float>& readings)
    {
        const std::size_t step = block == 0 ? input.size() : block;
        readings.resize((input.size() + step - 1) / step);
        for (std::size_t start = 0, r = 0; start < input.size(); start += step, ++r)
            readings[r] = read(input.data() + start, static_cast<int>(std::min(step, input.size() - start)));
    }

    // The largest |a - b|; infinity when either is not finite or the sizes differ.
    double maxDifference(const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return std::numeric_limits<double>::infinity();
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

    // meter.fm compiled at -O`level` with poly floats of `lanes` (0: this machine's), and its four kernels; or nullptr,
    // with the errors.
    std::unique_ptr<fminus::Program> compile(const std::string& path, const std::string& source, int level,
                                             unsigned lanes, std::array<Kernel, 4>& kernels, std::string& errors)
    {
        fminus::Program::Options options;
        options.optimisation = level;
        options.polyLanes = lanes;
        options.fileName = path;
        std::unique_ptr<fminus::Program> program = fminus::Program::compile(source, errors, options);
        if (!program)
            return nullptr;
        for (Kernel& kernel : kernels)
        {
            kernel.fminus = program->function<Measure>(kernel.name, errors);
            if (kernel.fminus == nullptr)
                return nullptr;
        }
        return program;
    }

    // Everything --check checks, at every width. Prints a line, and returns whether it all held.
    bool check(const std::string& path, const std::string& source, int level, const std::vector<float>& input,
               float decay, double tolerance)
    {
        double worstAgainstCpp = 0.0;
        bool rmsInOrder = true;
        bool samePeak = true;
        bool meterInOrder = true;
        bool decayHeld = true;
        bool nonBlocking = true;
        // For the meter, the noise fades out, so its held peak falls by its decay; and an impulse then silence, after
        // which the held peak is the decay itself, then its square (1 times decay, exactly).
        const std::vector<float> impulse = [] {
            std::vector<float> one(4 * 64, 0.0f);
            one[0] = 1.0f;
            return one;
        }();
        std::vector<float> fading(input.size());
        for (std::size_t i = 0; i < input.size(); ++i)
            fading[i] = input[i] * (1.0f - static_cast<float>(i) / static_cast<float>(input.size()));
        std::vector<float> readings;
        std::vector<float> expected;
        for (unsigned lanes = 1; lanes <= 64; lanes *= 2)
        {
            std::array<Kernel, 4> kernels{Kernel{"peak", "", nullptr, cppPeak},
                                          Kernel{"peak_poly", "", nullptr, cppPeak}, Kernel{"rms", "", nullptr, cppRms},
                                          Kernel{"rms_poly", "", nullptr, nullptr}};
            std::string errors;
            const auto program = compile(path, source, level, lanes, kernels, errors);
            if (!program)
            {
                std::cerr << "example-meter: " << errors;
                return false;
            }
            for (const Kernel& kernel : kernels)
            {
                nonBlocking = nonBlocking && program->nonBlocking(kernel.name);
                // Blocks of 0 to 3 samples: the loops' empty and partial steps.
                for (int n = 0; n <= 3; ++n)
                {
                    const float reading = kernel.fminus(input.data(), n);
                    if (kernel.cpp != nullptr)
                        worstAgainstCpp = std::max(
                            worstAgainstCpp, static_cast<double>(std::abs(reading - kernel.cpp(input.data(), n))));
                    else
                        rmsInOrder = rmsInOrder && reading == rmsInLanes(input.data(), n, lanes);
                }
            }
            for (const std::size_t block : {std::size_t{0}, std::size_t{64}, std::size_t{61}})
            {
                for (const Kernel& kernel : kernels)
                {
                    run(kernel.fminus, input, block, readings);
                    if (kernel.cpp != nullptr)
                    {
                        run(kernel.cpp, input, block, expected);
                        worstAgainstCpp = std::max(worstAgainstCpp, maxDifference(readings, expected));
                    }
                    else
                    {
                        run([&](const float* p, int n) { return rmsInLanes(p, n, lanes); }, input, block, expected);
                        rmsInOrder = rmsInOrder && readings == expected;
                    }
                }
                // A maximum is the same in any order: peak_poly reads what peak does, at every width.
                std::vector<float> scalar;
                run(kernels[0].fminus, input, block, scalar);
                run(kernels[1].fminus, input, block, readings);
                samePeak = samePeak && readings == scalar;
            }
            // The module: its fields after every block, bit for bit a C++ meter's.
            const Meter meter = makeMeter(*program, errors);
            if (!meter.memory)
            {
                std::cerr << "example-meter: " << path << " has no module Meter with a parameter decay, and fields "
                          << "held and level: " << errors;
                return false;
            }
            nonBlocking = nonBlocking && program->nonBlocking("Meter::measure");
            for (const std::size_t block : {std::size_t{0}, std::size_t{64}, std::size_t{61}})
            {
                const std::vector<float> fields = runMeter(meter, fading, decay, block);
                meterInOrder = meterInOrder && fields == cppMeter(fading, decay, block, lanes);
            }
            const std::vector<float> held = runMeter(meter, impulse, decay, 64);   // held, level, a block at a time
            decayHeld = decayHeld && held[0] == 1.0f && held[2] == decay && held[4] == decay * decay;
        }
        std::cout << std::format("example-meter: max |FMinus - C++| = {:.3g} (tolerance {:.3g}); at 1 to 64 lanes, "
                                 "rms_poly adds in its lanes' order: {}, peak_poly reads what peak does: {}, and "
                                 "Meter's fields are a C++ meter's at decay {}, over fading noise: {} (after an "
                                 "impulse, its held peak the decay: {}); every kernel nonblocking: {}\n",
                                 worstAgainstCpp, tolerance, rmsInOrder ? "yes" : "no", samePeak ? "yes" : "no", decay,
                                 meterInOrder ? "yes" : "no", decayHeld ? "yes" : "no", nonBlocking ? "yes" : "no");
        return worstAgainstCpp <= tolerance && rmsInOrder && samePeak && meterInOrder && decayHeld && nonBlocking;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The meter example: meter.fm against C++, compared and timed.", "example-meter"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/meter.fm";
    std::size_t samples = 48000;
    std::size_t block = 0;
    int repeats = 200;
    int level = 2;
    unsigned lanes = 0;
    bool checkOnly = false;
    double tolerance = 1e-6;
    float decay = 0.8f;   // not meter.fm's default (0.9): --check then fails if setValue misses its slot
    app.add_option("source", path, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{3}, static_cast<std::size_t>(std::numeric_limits<int>::max())))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per reading, a call to FMinus (0: one for the whole run)")
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--poly-lanes", lanes, "A poly float's lanes: 1, 2, 4, ... 64 (0: this machine's)")
        ->check(CLI::IsMember({0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u}))
        ->capture_default_str();
    app.add_option("--decay", decay, "Meter's decay: what is left of the held peak a block on")
        ->check(CLI::Range(0.0f, 1.0f))
        ->capture_default_str();
    app.add_flag("--check", checkOnly, "Compare only: exit 1 when a comparison fails");
    app.add_option("--tolerance", tolerance, "The largest |FMinus - C++| --check accepts")->capture_default_str();
    CLI11_PARSE(app, argc, argv);

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::cerr << "example-meter: cannot read " << path << "\n";
        return 1;
    }
    std::ostringstream text;
    text << file.rdbuf();
    const std::string source = text.str();
    const std::vector<float> input = makeNoise(samples);

    if (checkOnly)
        return check(path, source, level, input, decay, tolerance) ? 0 : 1;

    std::array<Kernel, 4> kernels{
        Kernel{"peak", "peak, one sample at a time", nullptr, cppPeak},
        Kernel{"peak_poly", "peak, a poly float at a time", nullptr, nullptr},
        Kernel{"rms", "RMS, one sample at a time", nullptr, cppRms},
        Kernel{"rms_poly", "RMS, a poly float at a time", nullptr, nullptr},
    };
    std::string errors;
    const std::unique_ptr<fminus::Program> program = compile(path, source, level, lanes, kernels, errors);
    if (!program)
    {
        std::cerr << "example-meter: " << errors;
        return 1;
    }

    // The exact readings, in double, and how far each kernel's are from them.
    std::vector<float> readings;
    std::vector<double> exactPeak;
    std::vector<double> exactRms;
    const std::size_t step = block == 0 ? samples : block;
    for (std::size_t start = 0; start < samples; start += step)
    {
        double largest = 0.0;
        double sum = 0.0;
        for (std::size_t i = start; i < std::min(start + step, samples); ++i)
        {
            largest = std::max(largest, std::abs(static_cast<double>(input[i])));
            sum += static_cast<double>(input[i]) * static_cast<double>(input[i]);
        }
        exactPeak.push_back(largest);
        exactRms.push_back(std::sqrt(sum / static_cast<double>(std::min(step, samples - start))));
    }
    auto relativeError = [](const std::vector<float>& got, const std::vector<double>& exact) {
        double worst = 0.0;
        for (std::size_t i = 0; i < got.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<double>(got[i]) - exact[i]) / exact[i]);
        return worst;
    };

    std::cout << std::format("level meter: {} samples of noise, {}; a poly float has {} lanes{}\n", samples,
                             block == 0 || block >= samples ? "one reading for all of them"
                                                            : std::format("one reading a block of {}", block),
                             program->polyLanes(), lanes == 0 ? " (this machine's)" : "");
    std::cout << std::format("  {:<32}{:>16}{:>16}{:>18}\n", "ns/sample", std::format("FMinus -O{}", level), "C++",
                             "relative error");
    for (const Kernel& kernel : kernels)
    {
        const double fminusTime =
            nanosecondsPerSample([&] { run(kernel.fminus, input, block, readings); }, samples, repeats);
        const bool isPeak = std::string_view(kernel.name).starts_with("peak");
        const double error = relativeError(readings, isPeak ? exactPeak : exactRms);
        const std::string cppTime =
            kernel.cpp == nullptr
                ? std::string("")
                : std::format("{:.3f}",
                              nanosecondsPerSample([&] { run(kernel.cpp, input, block, readings); }, samples, repeats));
        std::cout << std::format("  {:<32}{:>16.3f}{:>16}{:>18.2g}\n", kernel.description, fminusTime, cppTime, error);
    }
    const Meter meter = makeMeter(*program, errors);
    if (meter.memory)
    {
        meter.reset(decay);
        const double meterTime = nanosecondsPerSample(
            [&] {
                const std::size_t size = block == 0 ? samples : block;
                for (std::size_t start = 0; start < samples; start += size)
                    meter.measure(meter.memory.get(), input.data() + start,
                                  static_cast<int>(std::min(size, samples - start)));
            },
            samples, repeats);
        std::cout << std::format("  {:<32}{:>16.3f}\n", "Meter::measure: peak and rms_poly", meterTime);
    }
    std::cout << "  (relative error: the largest |reading - exact| / exact, against the reading in double)\n";
    return 0;
}
