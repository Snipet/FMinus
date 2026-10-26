// examples/ensemble/bench.cpp: the ensemble example's harness, a host of FMinus's public API alone (fminus/FMinus.h).
// It compiles ensemble.fm twice, with its regions on every core (Options::threads = 0, or --threads) and on the
// calling thread alone (threads = 1), plays a chord of sixteen notes through each, a block at a time, compares the two
// outputs bit for bit, and times both. With --check (what ctest runs) it only compares, and fails when the two differ
// at all, when calling process in blocks of another size changes the output, when a channel is silent, or when process
// is not [[clang::nonblocking]].
//
//     build-dev/bin/example-ensemble
//     build-dev/bin/example-ensemble --block 64 --threads 4
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>   // _aligned_malloc, _aligned_free
#endif

namespace
{
    // The most samples a call of process takes: a voice's buffer (ensemble.fm).
    constexpr std::size_t kLongestBlock = 1024;

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

    // An Ensemble in the host's memory, and the program's functions that run it.
    class Ensemble
    {
    public:
        Ensemble(fminus::Program& program, std::string& errors) : module_(program.module("Ensemble"))
        {
            noteOn_ = program.function<void(void*, int, float, float)>("Ensemble::note_on", errors);
            process_ = program.function<void(void*, float*, float*, int)>("Ensemble::process", errors);
        }

        bool found() const
        {
            return module_ != nullptr && noteOn_ != nullptr && process_ != nullptr;
        }

        // A new ensemble: aligned as the module needs, zeroed, its parameters at their defaults, then a chord of
        // sixteen notes, an A major chord over four octaves, the higher ones softer.
        void reset()
        {
            const std::size_t alignment = std::max(module_->alignment, alignof(std::max_align_t));
            const std::size_t size = (module_->size + alignment - 1) / alignment * alignment;
            memory_.reset(static_cast<unsigned char*>(alignedAlloc(alignment, size)));
            if (!memory_)
                throw std::bad_alloc();
            std::memset(memory_.get(), 0, size);
            fminus::Program::setDefaults(memory_.get(), *module_);
            constexpr std::array<float, 4> kChord{1.0f, 1.25f, 1.5f, 2.0f};   // A, C#, E, A
            for (int v = 0; v < 16; ++v)
            {
                const int octave = v / 4;
                const float frequency =
                    55.0f * static_cast<float>(1 << octave) * kChord[static_cast<std::size_t>(v % 4)];
                noteOn_(memory_.get(), v, frequency, 1.0f / static_cast<float>(1 + octave));
            }
        }

        // `samples` of the chord, in calls of `block` samples.
        void play(std::size_t block, std::vector<float>& left, std::vector<float>& right)
        {
            reset();
            for (std::size_t done = 0; done < left.size(); done += block)
            {
                const std::size_t n = std::min(block, left.size() - done);
                process_(memory_.get(), left.data() + done, right.data() + done, static_cast<int>(n));
            }
        }

    private:
        struct Free
        {
            void operator()(unsigned char* p) const
            {
                alignedFree(p);
            }
        };

        const fminus::Program::Module* module_;
        void (*noteOn_)(void*, int, float, float) = nullptr;
        void (*process_)(void*, float*, float*, int) = nullptr;
        std::unique_ptr<unsigned char, Free> memory_;
    };

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

    bool silent(const std::vector<float>& channel)
    {
        return std::all_of(channel.begin(), channel.end(), [](float x) { return x == 0.0f; });
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The ensemble example: sixteen voices rendered at once (parallel for), against one thread.",
                 "example-ensemble"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/ensemble.fm";
    std::size_t samples = 48000;
    std::size_t block = 512;
    int repeats = 10;
    int level = 2;
    unsigned threads = 0;
    bool check = false;
    app.add_option("source", path, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{64}, std::size_t{1} << 24))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call of process")
        ->check(CLI::Range(std::size_t{1}, kLongestBlock))
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--threads", threads, "The most threads a region runs on (0: one for each core)")
        ->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when the threads change the output at all");
    CLI11_PARSE(app, argc, argv);

    std::ifstream file(path);
    if (!file)
    {
        std::fprintf(stderr, "example-ensemble: cannot read %s\n", path.c_str());
        return 1;
    }
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string errors;
    fminus::Program::Options options;
    options.optimisation = level;
    options.fileName = path;
    options.threads = threads;
    std::unique_ptr<fminus::Program> together = fminus::Program::compile(source, errors, options);
    options.threads = 1;
    std::unique_ptr<fminus::Program> alone = together ? fminus::Program::compile(source, errors, options) : nullptr;
    if (!alone)
    {
        std::fprintf(stderr, "example-ensemble: %s", errors.c_str());
        return 1;
    }
    Ensemble parallel(*together, errors);
    Ensemble serial(*alone, errors);
    if (!parallel.found() || !serial.found())
    {
        std::fprintf(stderr, "example-ensemble: %s", errors.empty() ? "ensemble.fm has no module Ensemble\n"
                                                                    : errors.c_str());
        return 1;
    }

    std::vector<float> left(samples), right(samples), leftAlone(samples), rightAlone(samples);
    parallel.play(block, left, right);
    serial.play(block, leftAlone, rightAlone);
    const bool same = left == leftAlone && right == rightAlone;

    if (check)
    {
        // In blocks of 61, ending mid-way through the others: the same output, every voice's phase carried over.
        std::vector<float> leftOdd(samples), rightOdd(samples);
        parallel.play(61, leftOdd, rightOdd);
        const bool sameInBlocks = leftOdd == left && rightOdd == right;
        const bool sounds = !silent(left) && !silent(right);
        const bool nonBlocking = together->nonBlocking("Ensemble::process");
        std::printf("%s", std::format("example-ensemble: {} samples in blocks of {}, on every thread and on one: the "
                                      "same, bit for bit: {}; in blocks of 61: {}; both channels sound: {}; process "
                                      "nonblocking: {}\n",
                                      samples, block, same ? "yes" : "no", sameInBlocks ? "yes" : "no",
                                      sounds ? "yes" : "no", nonBlocking ? "yes" : "no")
                              .c_str());
        return same && sameInBlocks && sounds && nonBlocking ? 0 : 1;
    }

    const double parallelTime = nanosecondsPerSample([&] { parallel.play(block, left, right); }, samples, repeats);
    const double serialTime =
        nanosecondsPerSample([&] { serial.play(block, leftAlone, rightAlone); }, samples, repeats);
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    std::printf("%s",
                std::format("sixteen voices (a sawtooth of up to 32 partials each), {} samples in blocks of {}, -O{}\n"
                            "  parallel for, {:<10} {:8.1f} ns/sample\n"
                            "  one thread            {:8.1f} ns/sample\n"
                            "  one thread / parallel {:8.2f}x\n"
                            "  the outputs are {}\n",
                            samples, block, level,
                            threads == 0 ? std::format("{} threads", cores) : std::format("{} threads", threads),
                            parallelTime, serialTime, serialTime / parallelTime,
                            same ? "bit-identical" : "different")
                    .c_str());
    return 0;
}
