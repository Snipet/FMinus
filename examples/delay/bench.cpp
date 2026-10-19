// examples/delay/bench.cpp: the delay example's harness. It JIT-compiles delay.fm, creates its line (once, as a
// plugin's prepare() would), runs it over noise in one call, compares the result with C++ doing the same, times both,
// and frees the line. With --check (what ctest runs) it only compares, and fails when the two differ at all, when
// calling the kernel a block at a time changes its output, or when the line is not the length or alignment it should
// be.
//
//     build-release/bin/example-delay               (time in the release preset)
//     build-dev/bin/example-delay --block 64
//
// The C++ version is built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, it never fuses
// a multiply and an add.
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
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    // The FMinus kernel: the functions that allocate and free the state, and the one that runs a block on it.
    struct Kernel
    {
        int (*length)(float seconds, float rate) = nullptr;
        float* (*create)(int length) = nullptr;
        int* (*position)() = nullptr;
        void (*destroy)(float* line, int* position) = nullptr;
        void (*process)(float* line, int* position, int length, const float* input, float* output, int n,
                        float feedback, float mix) = nullptr;
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

    // The kernel over the whole signal, on a line it creates and frees: one call a block of `block` samples (0: one
    // call for all of it). The position carries over from one call to the next.
    void runKernel(const Kernel& kernel, const std::vector<float>& input, int length, float feedback, float mix,
                   std::size_t block, std::vector<float>& output)
    {
        float* line = kernel.create(length);
        int* position = kernel.position();
        const std::size_t step = block == 0 ? output.size() : block;
        for (std::size_t start = 0; start < output.size(); start += step)
        {
            const std::size_t n = std::min(step, output.size() - start);
            kernel.process(line, position, length, input.data() + start, output.data() + start, static_cast<int>(n),
                           feedback, mix);
        }
        kernel.destroy(line, position);
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
        llvm::errs() << "example-delay: " << llvm::toString(std::move(error)) << "\n";
        return 1;
    }

    // Compiles `path` at `level` into `jit`, and finds the kernel's functions.
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
        if (llvm::Error error = find("echo_length", kernel.length))
            return error;
        if (llvm::Error error = find("echo_create", kernel.create))
            return error;
        if (llvm::Error error = find("echo_position", kernel.position))
            return error;
        if (llvm::Error error = find("echo_destroy", kernel.destroy))
            return error;
        return find("echo_process", kernel.process);
    }
} // namespace

int main(int argc, char** argv)
{
    llvm::InitLLVM initLLVM(argc, argv);

    CLI::App app{"The delay example: delay.fm against C++, compared and timed.", "example-delay"};
    std::string source = std::string(FMINUS_EXAMPLE_DIR) + "/delay.fm";
    std::size_t samples = 480000;
    std::size_t block = 0;
    int repeats = 20;
    int level = 2;
    float seconds = 0.25f;
    float rate = 48000.0f;
    float feedback = 0.5f;
    float mix = 0.5f;
    bool check = false;
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
    app.add_option("--seconds", seconds, "The delay, in seconds")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_option("--rate", rate, "The sample rate, Hz")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus and C++ differ at all");
    CLI11_PARSE(app, argc, argv);

    const std::array<llvm::OptimizationLevel, 4> levels{llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
                                                        llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3};
    std::unique_ptr<fminus::jit::Jit> jit;
    Kernel kernel;
    if (llvm::Error error = compile(source, levels[static_cast<std::size_t>(level)], jit, kernel))
        return fail(std::move(error));

    const int length = kernel.length(seconds, rate);
    const std::vector<float> input = makeNoise(samples);
    std::vector<float> fminusOutput(samples);
    std::vector<float> blockOutput(samples);
    std::vector<float> cppOutput(samples);
    auto runFminus = [&] { runKernel(kernel, input, length, feedback, mix, block, fminusOutput); };
    auto runCpp = [&] {
        float* line = static_cast<float*>(std::calloc(static_cast<std::size_t>(length), sizeof(float)));
        int position = 0;
        referenceProcess(line, &position, length, input.data(), cppOutput.data(), static_cast<int>(samples), feedback,
                         mix);
        std::free(line);
    };
    runFminus();
    runCpp();

    if (check)
    {
        // The line FMinus allocates: the right length's worth of zeros, on a 64-byte boundary.
        float* line = kernel.create(length);
        const bool silent = std::all_of(line, line + length, [](float x) { return x == 0.0f; });
        const bool aligned = reinterpret_cast<std::uintptr_t>(line) % 64 == 0;
        kernel.destroy(line, kernel.position());
        // Calls of 64 samples, then of 61 (blocks that end mid-way through), must give the output one call does.
        bool sameInBlocks = true;
        for (const std::size_t size : {std::size_t{64}, std::size_t{61}})
        {
            runKernel(kernel, input, length, feedback, mix, size, blockOutput);
            sameInBlocks = sameInBlocks && blockOutput == fminusOutput;
        }
        const bool same = fminusOutput == cppOutput;
        llvm::outs() << std::format("example-delay: a line of {} samples, silent: {}, 64-byte aligned: {}; FMinus "
                                    "and C++ the same over {} samples: {}; in blocks of 64 and 61: {}\n",
                                    length, silent ? "yes" : "no", aligned ? "yes" : "no", samples,
                                    same ? "yes" : "no", sameInBlocks ? "yes" : "no");
        return silent && aligned && same && sameInBlocks && length == static_cast<int>(seconds * rate) ? 0 : 1;
    }

    const double fminusTime = nanosecondsPerSample(runFminus, samples, repeats);
    const double cppTime = nanosecondsPerSample(runCpp, samples, repeats);
    const std::string calls = block == 0 || block >= samples ? std::format("1 call for all {} samples", samples)
                                                             : std::format("1 call a block of {}", block);
    llvm::outs() << std::format(
        "feedback delay, {} s ({} samples) at {} Hz, its line on the heap: {} samples, feedback {}, mix {}\n"
        "  FMinus (JIT, -O{}) {:8.3f} ns/sample   {}, the line created and freed in FMinus\n"
        "  C++ (clang)       {:8.3f} ns/sample   the same loop, its line calloc'd in C++\n"
        "  FMinus / C++      {:8.2f}x\n"
        "  the outputs are {}\n",
        seconds, length, rate, samples, feedback, mix, level, fminusTime, calls, cppTime, fminusTime / cppTime,
        fminusOutput == cppOutput ? "bit-identical" : "different");
    return 0;
}
