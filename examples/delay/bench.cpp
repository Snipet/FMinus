// examples/delay/bench.cpp: the delay example's harness. It compiles delay.fm (the echo as plain C functions) and
// echo.fm (the same echo as a module with parameters) through fminus::Program, creates the line of each (once, as a
// plugin's prepare() would), runs them over noise in one call, compares both with C++ doing the same, times all three,
// and frees the lines. With --check (what ctest runs) it only compares, and fails when any two outputs differ at all,
// when calling a kernel a block at a time changes its output, when a line is not the length or alignment it should
// be, or when a process is not [[clang::nonblocking]]. Its feedback and mix are not echo.fm's defaults, so a knob
// that never reaches the module fails the comparison.
//
//     build-release/bin/example-delay               (time in the release preset)
//     build-dev/bin/example-delay --block 64
//
// The C++ version is built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, it never fuses
// a multiply and an add.
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
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

    // delay.fm's functions: the ones that allocate and free the state, and the one that runs a block on it.
    struct Functions
    {
        std::unique_ptr<fminus::Program> program;
        int (*length)(float seconds, float rate) = nullptr;
        float* (*create)(int length) = nullptr;
        int* (*position)() = nullptr;
        void (*destroy)(float* line, int* position) = nullptr;
        void (*process)(float* line, int* position, int length, const float* input, float* output, int n,
                        float feedback, float mix) = nullptr;
    };

    // echo.fm's module, Echo: its layout and parameters, and its methods.
    struct Module
    {
        std::unique_ptr<fminus::Program> program;
        const fminus::Program::Module* echo = nullptr;
        const fminus::Program::Parameter* feedback = nullptr;
        const fminus::Program::Parameter* mix = nullptr;
        std::size_t lineOffset = 0;   // where the field `line` is in an instance
        bool (*init)(void* echo, float seconds, float rate) = nullptr;
        void (*process)(void* echo, const float* input, float* output, int n) = nullptr;
        void (*end)(void* echo) = nullptr;
    };

    // delay.fm's echo_process in C++, line for line: raw pointers, as a C++ kernel would have them (a std::vector's
    // operator[] is bounds-checked here, under the libc++ hardening LLVM's definitions turn on).
    void referenceProcess(float* __restrict line, int* __restrict position, int length,
                          const float* __restrict input, float* __restrict output, int n, float feedback, float mix)
    {
        int w = *position;
        for (int i = 0; i < n; ++i)
        {
            const float delayed = line[w];
            const float x = input[i];
            line[w] = x + feedback * delayed;
            output[i] = x + mix * delayed;
            if (++w == length)
                w = 0;
        }
        *position = w;
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

    // Calls of at most `block` samples (0: one call for all of them).
    template <typename Process>
    void play(Process&& process, const std::vector<float>& input, std::vector<float>& output, std::size_t block)
    {
        const std::size_t step = block == 0 ? output.size() : block;
        for (std::size_t start = 0; start < output.size(); start += step)
        {
            const std::size_t n = std::min(step, output.size() - start);
            process(input.data() + start, output.data() + start, static_cast<int>(n));
        }
    }

    // delay.fm over the whole signal, on a line it creates and frees. The position carries over from call to call.
    bool runFunctions(const Functions& kernel, const std::vector<float>& input, int length, float feedback, float mix,
                      std::size_t block, std::vector<float>& output)
    {
        float* line = kernel.create(length);
        int* position = kernel.position();
        if (line == nullptr || position == nullptr)
        {
            kernel.destroy(line, position);
            return false;
        }
        auto process = [&](const float* in, float* out, int n) {
            kernel.process(line, position, length, in, out, n, feedback, mix);
        };
        play(process, input, output, block);
        kernel.destroy(line, position);
        return true;
    }

    struct Free
    {
        void operator()(void* memory) const
        {
            alignedFree(memory);
        }
    };

    // An instance of Echo, as a host makes one: aligned and zeroed, its parameters at their defaults, then set.
    // aligned_alloc takes an alignment of at least a pointer's on macOS, and a size that is a multiple of it.
    std::unique_ptr<void, Free> makeEcho(const Module& kernel, float feedback, float mix)
    {
        const std::size_t alignment = std::max(kernel.echo->alignment, alignof(std::max_align_t));
        std::unique_ptr<void, Free> echo(
            alignedAlloc(alignment, (kernel.echo->size + alignment - 1) / alignment * alignment));
        if (echo)
        {
            std::memset(echo.get(), 0, kernel.echo->size);
            fminus::Program::setDefaults(echo.get(), *kernel.echo);
            fminus::Program::setValue(echo.get(), *kernel.feedback, static_cast<double>(feedback));
            fminus::Program::setValue(echo.get(), *kernel.mix, static_cast<double>(mix));
        }
        return echo;
    }

    // echo.fm over the whole signal: an instance, its line made by init and freed by end.
    bool runModule(const Module& kernel, const std::vector<float>& input, float seconds, float rate, float feedback,
                   float mix, std::size_t block, std::vector<float>& output)
    {
        const std::unique_ptr<void, Free> echo = makeEcho(kernel, feedback, mix);
        if (!echo || !kernel.init(echo.get(), seconds, rate))
            return false;
        play([&](const float* in, float* out, int n) { kernel.process(echo.get(), in, out, n); }, input, output, block);
        kernel.end(echo.get());
        return true;
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
        std::fprintf(stderr, "example-delay: %s\n", message.c_str());
        return 1;
    }

    std::unique_ptr<fminus::Program> compile(const std::string& path, int level, std::string& errors)
    {
        std::ifstream file(path);
        if (!file)
        {
            errors = "cannot read " + path;
            return nullptr;
        }
        const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        fminus::Program::Options options;
        options.optimisation = level;
        options.fileName = path;
        return fminus::Program::compile(source, errors, options);
    }

    // delay.fm's functions.
    bool find(const std::string& path, int level, Functions& kernel, std::string& errors)
    {
        kernel.program = compile(path, level, errors);
        if (!kernel.program)
            return false;
        fminus::Program& program = *kernel.program;
        kernel.length = program.function<int(float, float)>("echo_length", errors);
        kernel.create = kernel.length ? program.function<float*(int)>("echo_create", errors) : nullptr;
        kernel.position = kernel.create ? program.function<int*()>("echo_position", errors) : nullptr;
        kernel.destroy = kernel.position ? program.function<void(float*, int*)>("echo_destroy", errors) : nullptr;
        using Process = void(float*, int*, int, const float*, float*, int, float, float);
        kernel.process = kernel.destroy ? program.function<Process>("echo_process", errors) : nullptr;
        return kernel.process != nullptr;
    }

    // echo.fm's module.
    bool find(const std::string& path, int level, Module& kernel, std::string& errors)
    {
        kernel.program = compile(path, level, errors);
        if (!kernel.program)
            return false;
        fminus::Program& program = *kernel.program;
        kernel.echo = program.module("Echo");
        kernel.feedback = program.parameter("Echo::feedback");
        kernel.mix = program.parameter("Echo::mix");
        if (kernel.echo == nullptr || kernel.feedback == nullptr || kernel.mix == nullptr)
        {
            errors = path + " has no module Echo with the parameters feedback and mix";
            return false;
        }
        for (const fminus::Program::Module::Field& field : kernel.echo->fields)
        {
            if (field.name == "line")
                kernel.lineOffset = field.offset;
        }
        kernel.init = program.function<bool(void*, float, float)>("Echo::init", errors);
        kernel.process = kernel.init ? program.function<void(void*, const float*, float*, int)>("Echo::process", errors)
                                     : nullptr;
        kernel.end = kernel.process ? program.function<void(void*)>("Echo::end", errors) : nullptr;
        return kernel.end != nullptr;
    }

    // A line as FMinus allocates it: `length` zeros, on a 64-byte boundary.
    bool silentAndAligned(const float* line, int length)
    {
        return line != nullptr && std::all_of(line, line + length, [](float x) { return x == 0.0f; })
               && reinterpret_cast<std::uintptr_t>(line) % 64 == 0;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The delay example: delay.fm's functions and echo.fm's module against C++, compared and timed.",
                 "example-delay"};
    std::string functionsPath = std::string(FMINUS_EXAMPLE_DIR) + "/delay.fm";
    std::string modulePath = std::string(FMINUS_EXAMPLE_DIR) + "/echo.fm";
    std::size_t samples = 480000;
    std::size_t block = 0;
    int repeats = 20;
    int level = 2;
    float seconds = 0.25f;
    float rate = 48000.0f;
    float feedback = 0.6f;   // not echo.fm's defaults (0.5): --check then fails if setValue misses its slot
    float mix = 0.35f;
    bool check = false;
    app.add_option("--functions", functionsPath, "The FMinus source of the functions")->capture_default_str();
    app.add_option("--module", modulePath, "The FMinus source of the module")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{1}, static_cast<std::size_t>(std::numeric_limits<int>::max())))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call to FMinus (0: one call for the whole run)")
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--seconds", seconds, "The delay, in seconds")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--rate", rate, "The sample rate, Hz")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--feedback", feedback, "The feedback")->check(CLI::Range(0.0f, 0.99f))->capture_default_str();
    app.add_option("--mix", mix, "The mix")->check(CLI::Range(0.0f, 1.0f))->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus and C++ differ at all");
    CLI11_PARSE(app, argc, argv);

    std::string errors;
    Functions functions;
    if (!find(functionsPath, level, functions, errors))
        return fail("\n" + errors);
    Module module;
    if (!find(modulePath, level, module, errors))
        return fail("\n" + errors);

    const int length = functions.length(seconds, rate);
    if (length < 1)
        return fail(std::format("a delay of {} s at {} Hz is shorter than a sample", seconds, rate));
    const std::vector<float> input = makeNoise(samples);
    std::vector<float> functionsOutput(samples);
    std::vector<float> moduleOutput(samples);
    std::vector<float> blockOutput(samples);
    std::vector<float> cppOutput(samples);
    auto runFminus = [&] { return runFunctions(functions, input, length, feedback, mix, block, functionsOutput); };
    auto runEcho = [&] { return runModule(module, input, seconds, rate, feedback, mix, block, moduleOutput); };
    auto runCpp = [&] {
        float* line = static_cast<float*>(std::calloc(static_cast<std::size_t>(length), sizeof(float)));
        int position = 0;
        referenceProcess(line, &position, length, input.data(), cppOutput.data(), static_cast<int>(samples), feedback,
                         mix);
        std::free(line);
    };
    if (!runFminus())
        return fail("echo_create made no line");
    if (!runEcho())
        return fail("Echo::init made no line");
    runCpp();

    if (check)
    {
        // The lines FMinus allocates: the right length's worth of zeros, on a 64-byte boundary.
        float* line = functions.create(length);
        const bool functionsLine = silentAndAligned(line, length);
        functions.destroy(line, functions.position());
        const std::unique_ptr<void, Free> echo = makeEcho(module, feedback, mix);
        bool moduleLine = echo && module.init(echo.get(), seconds, rate);
        if (moduleLine)
        {
            float* field = nullptr;
            std::memcpy(&field, static_cast<unsigned char*>(echo.get()) + module.lineOffset, sizeof field);
            moduleLine = silentAndAligned(field, length);
            module.end(echo.get());
        }
        // Calls of 64 samples, then of 61 (blocks that end mid-way through), must give the output one call does.
        bool sameInBlocks = true;
        for (const std::size_t size : {std::size_t{64}, std::size_t{61}})
        {
            runFunctions(functions, input, length, feedback, mix, size, blockOutput);
            sameInBlocks = sameInBlocks && blockOutput == functionsOutput;
            runModule(module, input, seconds, rate, feedback, mix, size, blockOutput);
            sameInBlocks = sameInBlocks && blockOutput == moduleOutput;
        }
        const bool same = functionsOutput == cppOutput && moduleOutput == cppOutput;
        const bool nonBlocking = functions.program->nonBlocking("echo_process")
                              && module.program->nonBlocking("Echo::process");
        std::printf("%s", std::format("example-delay: lines of {} samples, silent and 64-byte aligned: {}; the "
                                      "functions, the module and C++ the same over {} samples: {}; in blocks of 64 "
                                      "and 61: {}; process nonblocking: {}\n",
                                      length, functionsLine && moduleLine ? "yes" : "no", samples,
                                      same ? "yes" : "no", sameInBlocks ? "yes" : "no", nonBlocking ? "yes" : "no")
                              .c_str());
        return functionsLine && moduleLine && same && sameInBlocks && nonBlocking
                       && length == static_cast<int>(seconds * rate)
                   ? 0
                   : 1;
    }

    const double functionsTime = nanosecondsPerSample(runFminus, samples, repeats);
    const double moduleTime = nanosecondsPerSample(runEcho, samples, repeats);
    const double cppTime = nanosecondsPerSample(runCpp, samples, repeats);
    const std::string calls = block == 0 || block >= samples ? std::format("1 call for all {} samples", samples)
                                                             : std::format("1 call a block of {}", block);
    std::printf("%s",
                std::format("feedback delay, {} s ({} samples) at {} Hz, its line on the heap: {} samples, feedback "
                            "{}, mix {}, {}\n"
                            "  FMinus, functions (-O{}) {:8.3f} ns/sample   delay.fm: plain C, the state two "
                            "allocations\n"
                            "  FMinus, module          {:8.3f} ns/sample   echo.fm: the state in the module, the "
                            "feedback and mix knobs\n"
                            "  C++ (clang)             {:8.3f} ns/sample   the same loop, its line calloc'd in C++\n"
                            "  the outputs are {}\n",
                            seconds, length, rate, samples, feedback, mix, calls, level, functionsTime, moduleTime,
                            cppTime,
                            functionsOutput == cppOutput && moduleOutput == cppOutput ? "bit-identical" : "different")
                    .c_str());
    return 0;
}
