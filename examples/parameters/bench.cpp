// examples/parameters/bench.cpp: the parameters example's harness. It compiles parameters.fm through fminus::Program
// three ways, and runs noise through its filter, the cutoff swept by its LFO:
//   knobs            the host sets the filter's cutoff once a call, to the LFO's value at the call's start (a
//                    plugin's block-rate control);
//   cutoff a stream  the LFO's buffer is the cutoff, a value a sample (sample-accurate);
//   frozen           no sweep: the filter at its defaults, compiled in.
// It compares each with the same modules written in C++, and times them, and the fixed filter as knobs too. With
// --check (what ctest runs) it only compares, and fails when an output is not C++'s bit for bit, when the
// sample-accurate output changes with the calls' length, or when the frozen filter is not the knobs' at the same
// values, bit for bit.
//
//     build-dev/bin/example-parameters
//     build-dev/bin/example-parameters --block 256
//     build-dev/bin/example-parameters --math fast       (timed with LLVM's intrinsics for sinf and expf)
//
// The C++ versions are built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, they never
// fuse a multiply and an add, and they take sinf and expf from fminus::math, which computes them as parameters.fm does
// (FMinus's default math mode).
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
#include <map>
#include <memory>
#include <string>
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

    using fminus::Program;

    // parameters.fm's Lfo in C++, line for line, its parameters arguments.
    struct CppLfo
    {
        float phase = 0.0f;

        void process(float frequency, float depth, float centre, float sampleRate, float* out, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                out[i] = centre * fminus::math::exp(0.6931472f * depth * fminus::math::sin(6.2831853f * phase));
                phase += frequency / sampleRate;
                if (phase >= 1.0f)
                    phase -= 1.0f;
            }
        }
    };

    // parameters.fm's Filter in C++, the cutoff a value a sample.
    struct CppFilter
    {
        float low = 0.0f;
        float band = 0.0f;

        void process(const float* in, const float* cutoff, float damping, int shape, float sampleRate, float* out,
                     int n)
        {
            const float w = 3.14159265f / sampleRate;
            for (int i = 0; i < n; ++i)
            {
                const float f = 2.0f * fminus::math::sin(w * cutoff[i]);
                low += f * band;
                const float high = in[i] - low - damping * band;
                band += f * high;
                out[i] = shape == 0 ? low : shape == 1 ? band : high;
            }
        }
    };

    struct Free
    {
        void operator()(void* memory) const
        {
            alignedFree(memory);
        }
    };

    using Memory = std::unique_ptr<void, Free>;

    // An instance of a module, as a host makes one: aligned and zeroed (every field starts at 0), then each parameter
    // at its fminus::default. aligned_alloc takes an alignment of at least a pointer's on macOS, and a size that
    // is a multiple of it.
    Memory instance(const Program::Module& module)
    {
        const std::size_t alignment = std::max(module.alignment, alignof(std::max_align_t));
        Memory memory(alignedAlloc(alignment, (module.size + alignment - 1) / alignment * alignment));
        if (memory)
        {
            std::memset(memory.get(), 0, module.size);
            Program::setDefaults(memory.get(), module);
        }
        return memory;
    }

    // One compilation of parameters.fm, and what the host needs of it.
    struct Patch
    {
        std::unique_ptr<Program> program;
        const Program::Module* lfo = nullptr;
        const Program::Module* filter = nullptr;
        const Program::Parameter* cutoff = nullptr;
        void (*runLfo)(void*, float*, int) = nullptr;
        void (*runFilter)(void*, const float*, float*, int) = nullptr;
    };

    Patch compile(const std::string& source, const std::string& path, int level, Program::Math math,
                  std::map<std::string, Program::Binding> parameters, std::string& errors)
    {
        Program::Options options;
        options.optimisation = level;
        options.math = math;
        options.fileName = path;
        options.parameters = std::move(parameters);
        Patch patch;
        patch.program = Program::compile(source, errors, options);
        if (!patch.program)
            return patch;
        patch.lfo = patch.program->module("Lfo");
        patch.filter = patch.program->module("Filter");
        patch.cutoff = patch.program->parameter("Filter::cutoff");
        patch.runLfo = patch.program->function<void(void*, float*, int)>("Lfo::process", errors);
        patch.runFilter = patch.program->function<void(void*, const float*, float*, int)>("Filter::process", errors);
        if (patch.lfo == nullptr || patch.filter == nullptr || patch.cutoff == nullptr || patch.runLfo == nullptr
            || patch.runFilter == nullptr)
            patch.program.reset();
        return patch;
    }

    // How the host moves the filter's cutoff.
    enum class Sweep
    {
        None,        // not at all: the filter at its defaults
        BlockRate,   // once a call, to the LFO's value at the call's start: a knob
        Sample,      // a value a sample: the LFO's buffer, a stream
    };

    // The filter over `in`, in calls of `block` samples, the LFO sweeping its cutoff as `sweep` says.
    void play(const Patch& patch, Sweep sweep, const std::vector<float>& in, std::vector<float>& out,
              std::size_t block)
    {
        Memory lfo = instance(*patch.lfo);
        Memory filter = instance(*patch.filter);
        std::vector<float> cutoff(block);
        for (std::size_t start = 0; start < in.size(); start += block)
        {
            const int n = static_cast<int>(std::min(block, in.size() - start));
            if (sweep == Sweep::BlockRate)
            {
                patch.runLfo(lfo.get(), cutoff.data(), n);
                Program::setValue(filter.get(), *patch.cutoff, static_cast<double>(cutoff[0]));
            }
            else if (sweep == Sweep::Sample)
            {
                patch.runLfo(lfo.get(), cutoff.data(), n);
                Program::setStream(filter.get(), *patch.cutoff, cutoff.data());
            }
            patch.runFilter(filter.get(), in.data() + start, out.data() + start, n);
        }
    }

    // A parameter's default, as the program describes it: the C++ reference plays with the same values.
    float defaultOf(const Program& program, const char* name)
    {
        const Program::Parameter* parameter = program.parameter(name);
        return parameter != nullptr ? static_cast<float>(parameter->defaultValue.value_or(0.0)) : 0.0f;
    }

    // The same, in C++.
    void playCpp(const Program& program, Sweep sweep, const std::vector<float>& in, std::vector<float>& out,
                 std::size_t block)
    {
        const float frequency = defaultOf(program, "Lfo::frequency");
        const float depth = defaultOf(program, "Lfo::depth");
        const float centre = defaultOf(program, "Lfo::centre");
        const float lfoRate = defaultOf(program, "Lfo::sampleRate");
        const float damping = defaultOf(program, "Filter::damping");
        const int shape = static_cast<int>(defaultOf(program, "Filter::shape"));
        const float filterRate = defaultOf(program, "Filter::sampleRate");
        CppLfo lfo;
        CppFilter filter;
        std::vector<float> swept(block);
        std::vector<float> cutoff(block, defaultOf(program, "Filter::cutoff"));
        for (std::size_t start = 0; start < in.size(); start += block)
        {
            const int n = static_cast<int>(std::min(block, in.size() - start));
            if (sweep != Sweep::None)
            {
                lfo.process(frequency, depth, centre, lfoRate, swept.data(), n);
                if (sweep == Sweep::Sample)
                    std::copy(swept.begin(), swept.end(), cutoff.begin());
                else
                    std::fill(cutoff.begin(), cutoff.end(), swept[0]);
            }
            filter.process(in.data() + start, cutoff.data(), damping, shape, filterRate, out.data() + start, n);
        }
    }

    // White noise in [-1, 1) from a fixed 32-bit LCG.
    std::vector<float> makeNoise(std::size_t samples, std::uint32_t state)
    {
        std::vector<float> noise(samples);
        for (float& x : noise)
        {
            state = state * 1664525u + 1013904223u;
            x = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
        }
        return noise;
    }

    // The largest difference between two signals, relative to the first's peak.
    double difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        double error = 0.0;
        double peak = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            error = std::max(error, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
            peak = std::max(peak, std::abs(static_cast<double>(a[i])));
        }
        return peak > 0.0 ? error / peak : error;
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

    std::string describe(const Program::Parameter& parameter)
    {
        const char* binding = parameter.binding.kind == Program::Binding::Kind::Stream   ? "a stream"
                              : parameter.binding.kind == Program::Binding::Kind::Frozen ? "frozen"
                                                                                         : "a knob";
        std::string text = std::format("  {:<20}{:<6}", parameter.module + "::" + parameter.name, parameter.type);
        if (parameter.minimum && parameter.maximum)
            text += std::format(" {:>16}", std::format("[{:g}, {:g}]", *parameter.minimum, *parameter.maximum));
        else
            text += std::format(" {:>16}", "");
        text += parameter.defaultValue ? std::format("  default {:<7g}", *parameter.defaultValue)
                                       : std::format("  {:<15}", "");
        if (parameter.binding.kind == Program::Binding::Kind::Frozen)
            return text + std::format("  frozen at {:g}", parameter.binding.value);
        return text + std::format("  {}, slot at +{}", binding, parameter.streamOffset);
    }

    int fail(const std::string& message)
    {
        std::fprintf(stderr, "example-parameters: %s\n", message.c_str());
        return 1;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The parameters example: parameters.fm's filter, its cutoff swept by its LFO through a knob or a "
                 "stream, or frozen, against C++.",
                 "example-parameters"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/parameters.fm";
    std::size_t samples = 96000;
    std::size_t block = 64;
    int repeats = 20;
    int level = 2;
    bool check = false;
    std::string math = "vector";
    app.add_option("source", path, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{1024}, std::size_t{1} << 26))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call")
        ->check(CLI::Range(std::size_t{1}, std::size_t{1} << 16))
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when an output is not what it should be");
    app.add_option("--math", math, "How FMinus computes sinf and expf: vector, libm or fast (--check expects vector)")
        ->check(CLI::IsMember({"vector", "libm", "fast"}))
        ->capture_default_str();
    CLI11_PARSE(app, argc, argv);
    const Program::Math mode = math == "libm"   ? Program::Math::Libm
                               : math == "fast" ? Program::Math::Fast
                                                : Program::Math::Vector;

    std::ifstream file(path);
    if (!file)
        return fail("cannot read " + path);
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string errors;
    const Patch knobs = compile(source, path, level, mode, {}, errors);
    if (!knobs.program)
        return fail("\n" + errors);
    const Patch streamed =
        compile(source, path, level, mode, {{"Filter::cutoff", Program::Binding::stream()}}, errors);
    if (!streamed.program)
        return fail("\n" + errors);
    // The filter's defaults, frozen.
    std::map<std::string, Program::Binding> fixed;
    for (const Program::Parameter& parameter : knobs.filter->parameters)
        fixed[parameter.module + "::" + parameter.name] = Program::Binding::frozen(parameter.defaultValue.value_or(0));
    const Patch frozen = compile(source, path, level, mode, fixed, errors);
    if (!frozen.program)
        return fail("\n" + errors);

    const std::vector<float> input = makeNoise(samples, 0x12345678u);
    std::vector<float> blockRate(samples);
    std::vector<float> sampleAccurate(samples);
    std::vector<float> still(samples);
    std::vector<float> constant(samples);
    std::vector<float> cpp(samples);
    play(knobs, Sweep::BlockRate, input, blockRate, block);
    play(streamed, Sweep::Sample, input, sampleAccurate, block);
    play(knobs, Sweep::None, input, still, block);
    play(frozen, Sweep::None, input, constant, block);

    if (check)
    {
        playCpp(*knobs.program, Sweep::BlockRate, input, cpp, block);
        const bool blockSame = blockRate == cpp;
        playCpp(*knobs.program, Sweep::Sample, input, cpp, block);
        const bool sampleSame = sampleAccurate == cpp;
        playCpp(*knobs.program, Sweep::None, input, cpp, block);
        const bool stillSame = still == cpp;
        // Sample-accurate is the same however the host splits its calls; block-rate is not.
        bool sameInCalls = true;
        std::vector<float> other(samples);
        for (const std::size_t size : {std::size_t{61}, std::size_t{256}, samples})
        {
            play(streamed, Sweep::Sample, input, other, size);
            sameInCalls = sameInCalls && other == sampleAccurate;
        }
        // Frozen, the coefficient, 2 sin(pi 1000 / 48000), is a constant's sine, which the knobs' filter computes from
        // the knob: the same function in both, the runtime's (LLVM folds no call of it), so the same output.
        const bool frozenSame = constant == still;
        bool nonBlocking = true;
        for (const Patch* patch : {&knobs, &streamed, &frozen})
            nonBlocking = nonBlocking && patch->program->nonBlocking("Lfo::process")
                       && patch->program->nonBlocking("Filter::process");
        std::printf("%s", std::format("example-parameters: calls of {}: the same as C++ with the cutoff a knob: {}, a "
                                      "stream: {}, fixed: {}; the stream's in calls of 61, 256 and {}: {}; frozen, "
                                      "the knobs' output: {}; nonblocking: {}\n",
                                      block, blockSame ? "yes" : "no", sampleSame ? "yes" : "no",
                                      stillSame ? "yes" : "no", samples, sameInCalls ? "yes" : "no",
                                      frozenSame ? "yes" : "no", nonBlocking ? "yes" : "no")
                              .c_str());
        return blockSame && sampleSame && stillSame && sameInCalls && frozenSame && nonBlocking ? 0 : 1;
    }

    std::printf("parameters.fm's parameters, as fminus::Program describes them:\n");
    for (const Patch* patch : {&knobs, &streamed, &frozen})
    {
        std::printf("%s", patch == &knobs      ? "with knobs\n"
                          : patch == &streamed ? "with the cutoff a stream\n"
                                               : "frozen\n");
        for (const Program::Module& module : patch->program->modules())
        {
            if (patch == &frozen && module.name == "Lfo")
                continue;
            for (const Program::Parameter& parameter : module.parameters)
                std::printf("%s\n", describe(parameter).c_str());
        }
    }

    const auto time = [&](auto&& run) { return nanosecondsPerSample(run, samples, repeats); };
    const double streamTime = time([&] { play(streamed, Sweep::Sample, input, sampleAccurate, block); });
    const double knobTime = time([&] { play(knobs, Sweep::BlockRate, input, blockRate, block); });
    const double cppStreamTime = time([&] { playCpp(*knobs.program, Sweep::Sample, input, cpp, block); });
    const double cppKnobTime = time([&] { playCpp(*knobs.program, Sweep::BlockRate, input, cpp, block); });
    const double stillTime = time([&] { play(knobs, Sweep::None, input, still, block); });
    const double frozenTime = time([&] { play(frozen, Sweep::None, input, constant, block); });
    const double cppStillTime = time([&] { playCpp(*knobs.program, Sweep::None, input, cpp, block); });
    std::printf("%s", std::format("\nthe filter over {} samples of noise, in calls of {}, -O{}\n"
                                  "  swept, sample-accurate: the cutoff a stream   {:6.2f} ns/sample   C++ {:6.2f}\n"
                                  "  swept once a call: the cutoff a knob          {:6.2f} ns/sample   C++ {:6.2f}\n"
                                  "  fixed: knobs                                  {:6.2f} ns/sample   C++ {:6.2f}\n"
                                  "  fixed: frozen                                 {:6.2f} ns/sample\n"
                                  "  a sweep once a call against a sweep a sample: {:.3g} of the peak apart\n",
                                  samples, block, level, streamTime, cppStreamTime, knobTime, cppKnobTime, stillTime,
                                  cppStillTime, frozenTime, difference(sampleAccurate, blockRate))
                          .c_str());
    return 0;
}
