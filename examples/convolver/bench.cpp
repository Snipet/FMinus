// examples/convolver/bench.cpp: the convolver example's harness. It compiles convolver.fm through fminus::Program,
// makes a convolver for a decaying noise burst (a small room's response), runs noise through it, and compares the
// output with a direct convolution in double, and with the same overlap-add written in C++ on the same FFT (the
// runtime's, runtime/Runtime.h). It times FMinus's, C++'s, and a direct convolution in C++. With --check (what ctest
// runs) it only compares, and fails when FMinus's output is not C++'s bit for bit, is not the direct convolution's to
// within float's rounding, is not `block` samples late, or changes when process is called a block of 64 or 61 at a
// time.
//
//     build-dev/bin/example-convolver
//     build-dev/bin/example-convolver --taps 8192 --hop 512
//
// The C++ versions are built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, they never
// fuse a multiply and an add.
#include "fminus/FMinus.h"
#include "runtime/Runtime.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // convolver.fm's fft_size.
    int fftSize(int need)
    {
        for (int n = (need + 31) / 32 * 32;; n += 32)
        {
            int m = n;
            while (m % 2 == 0)
                m /= 2;
            while (m % 3 == 0)
                m /= 3;
            while (m % 5 == 0)
                m /= 5;
            if (m == 1)
                return n;
        }
    }

    // convolver.fm's Convolver, line for line, on the runtime's FFT: the reference FMinus's output must equal.
    class CppConvolver
    {
    public:
        CppConvolver(const float* ir, int length, int hop)
            : block_(hop), size_(fftSize(hop + length - 1)), plan_(fminus_rt_fft_create(size_)),
              response_(2 * static_cast<std::size_t>(size_ / 2 + 1)), gathered_(static_cast<std::size_t>(hop)),
              ready_(static_cast<std::size_t>(hop)), frame_(static_cast<std::size_t>(size_)),
              bins_(response_.size()), result_(frame_.size()), tail_(frame_.size())
        {
            std::copy(ir, ir + length, frame_.begin());
            fminus_rt_fft_forward_real(plan_, frame_.data(), response_.data());
            std::fill(frame_.begin(), frame_.begin() + length, 0.0f);
            const float scale = 1.0f / static_cast<float>(size_);
            for (float& v : response_)
                v *= scale;
        }
        ~CppConvolver() { fminus_rt_fft_destroy(plan_); }
        CppConvolver(const CppConvolver&) = delete;
        CppConvolver& operator=(const CppConvolver&) = delete;

        void process(const float* in, float* out, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                out[i] = ready_[static_cast<std::size_t>(filled_)];
                gathered_[static_cast<std::size_t>(filled_)] = in[i];
                if (++filled_ == block_)
                {
                    convolve();
                    filled_ = 0;
                }
            }
        }

    private:
        void convolve()
        {
            const auto b = static_cast<std::size_t>(block_);
            const auto n = static_cast<std::size_t>(size_);
            std::copy(gathered_.begin(), gathered_.end(), frame_.begin());
            fminus_rt_fft_forward_real(plan_, frame_.data(), bins_.data());
            for (std::size_t k = 0; k <= n / 2; ++k)
            {
                const float ax = bins_[2 * k];
                const float ay = bins_[2 * k + 1];
                const float bx = response_[2 * k];
                const float by = response_[2 * k + 1];
                bins_[2 * k] = ax * bx - ay * by;
                bins_[2 * k + 1] = ax * by + ay * bx;
            }
            fminus_rt_fft_inverse_real(plan_, bins_.data(), result_.data());
            for (std::size_t i = 0; i < n; ++i)
                tail_[i] += result_[i];
            std::copy(tail_.begin(), tail_.begin() + static_cast<std::ptrdiff_t>(b), ready_.begin());
            std::copy(tail_.begin() + static_cast<std::ptrdiff_t>(b), tail_.end(), tail_.begin());
            std::fill(tail_.end() - static_cast<std::ptrdiff_t>(b), tail_.end(), 0.0f);
        }

        int block_;
        int size_;
        int filled_ = 0;
        void* plan_;
        std::vector<float> response_, gathered_, ready_, frame_, bins_, result_, tail_;
    };

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

    // A small room's impulse response: noise under an exponential decay, 60 dB down by its end.
    std::vector<float> makeResponse(int taps)
    {
        std::vector<float> response = makeNoise(static_cast<std::size_t>(taps), 0x2468ace0u);
        for (int k = 0; k < taps; ++k)
        {
            const float age = static_cast<float>(k) / static_cast<float>(taps);
            response[static_cast<std::size_t>(k)] *= std::exp(-6.9077553f * age);   // e^-6.9 is -60 dB
        }
        return response;
    }

    // The convolution, directly and in double: what the others are measured against.
    std::vector<double> directDouble(const std::vector<float>& x, const std::vector<float>& h)
    {
        std::vector<double> y(x.size());
        for (std::size_t t = 0; t < x.size(); ++t)
        {
            double sum = 0.0;
            for (std::size_t k = 0; k < h.size() && k <= t; ++k)
                sum += static_cast<double>(h[k]) * static_cast<double>(x[t - k]);
            y[t] = sum;
        }
        return y;
    }

    // The convolution, directly in float, as a C++ FIR would compute it: one tap at a time over the whole signal (a
    // loop the vectoriser can widen, where a dot product per sample would be a reduction it may not reorder).
    void directFloat(const std::vector<float>& x, const std::vector<float>& h, std::vector<float>& y)
    {
        std::fill(y.begin(), y.end(), 0.0f);
        const std::size_t n = x.size();
        for (std::size_t k = 0; k < h.size() && k < n; ++k)
        {
            const float tap = h[k];
            float* __restrict out = y.data() + k;
            const float* __restrict in = x.data();
            for (std::size_t t = 0; t < n - k; ++t)
                out[t] += tap * in[t];
        }
    }

    // Calls of `process` of at most `block` samples (0: one call for all of them).
    template <typename Process>
    void play(Process&& process, const std::vector<float>& in, std::vector<float>& out, std::size_t block)
    {
        const std::size_t step = block == 0 ? in.size() : block;
        for (std::size_t start = 0; start < in.size(); start += step)
        {
            const std::size_t n = std::min(step, in.size() - start);
            process(in.data() + start, out.data() + start, static_cast<int>(n));
        }
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
        std::fprintf(stderr, "example-convolver: %s\n", message.c_str());
        return 1;
    }
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The convolver example: convolver.fm's overlap-add against C++'s and a direct convolution.",
                 "example-convolver"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/convolver.fm";
    std::size_t samples = 96000;
    int taps = 2048;
    int hop = 256;
    std::size_t block = 0;
    int repeats = 20;
    int level = 2;
    bool check = false;
    app.add_option("source", path, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{1024}, std::size_t{1} << 26))
        ->capture_default_str();
    app.add_option("--taps", taps, "The impulse response's length")
        ->check(CLI::Range(1, 1 << 20))
        ->capture_default_str();
    app.add_option("--hop", hop, "Samples a block: the latency")->check(CLI::Range(1, 1 << 16))->capture_default_str();
    app.add_option("--block", block, "Samples per call of process (0: one call for the whole run)")
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus's output is not what it should be");
    CLI11_PARSE(app, argc, argv);

    std::ifstream file(path);
    if (!file)
        return fail("cannot read " + path);
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string errors;
    fminus::Program::Options options;
    options.optimisation = level;
    options.fileName = path;
    std::unique_ptr<fminus::Program> program = fminus::Program::compile(source, errors, options);
    if (!program)
        return fail("\n" + errors);
    auto* create = program->function<void*(const float*, int, int)>("convolver_create", errors);
    auto* process = program->function<void(void*, const float*, float*, int)>("Convolver::process", errors);
    auto* destroy = program->function<void(void*)>("convolver_destroy", errors);
    if (create == nullptr || process == nullptr || destroy == nullptr)
        return fail(errors);

    const std::vector<float> response = makeResponse(taps);
    const std::vector<float> input = makeNoise(samples, 0x12345678u);
    std::vector<float> fminusOutput(samples);
    std::vector<float> cppOutput(samples);
    auto runFminus = [&](std::size_t size, std::vector<float>& out) {
        void* convolver = create(response.data(), taps, hop);
        if (convolver == nullptr)
            return false;
        play([&](const float* in, float* o, int n) { process(convolver, in, o, n); }, input, out, size);
        destroy(convolver);
        return true;
    };
    auto runCpp = [&] {
        CppConvolver convolver(response.data(), taps, hop);
        play([&](const float* in, float* o, int n) { convolver.process(in, o, n); }, input, cppOutput, block);
    };
    if (!runFminus(block, fminusOutput))
        return fail("convolver_create gave no convolver");
    runCpp();

    const int size = fftSize(hop + taps - 1);
    if (check)
    {
        // Against the direct convolution in double, `hop` samples later; silence before that.
        const std::vector<double> exact = directDouble(input, response);
        double error = 0.0;
        double peak = 0.0;
        bool silentFirst = true;
        for (std::size_t t = 0; t < samples; ++t)
        {
            if (t < static_cast<std::size_t>(hop))
            {
                silentFirst = silentFirst && fminusOutput[t] == 0.0f;
                continue;
            }
            const double want = exact[t - static_cast<std::size_t>(hop)];
            error = std::max(error, std::abs(static_cast<double>(fminusOutput[t]) - want));
            peak = std::max(peak, std::abs(want));
        }
        constexpr double kTolerance = 1e-6;   // relative to the output's peak: float's rounding, through two FFTs
        bool sameInBlocks = true;
        std::vector<float> other(samples);
        for (const std::size_t size2 : {std::size_t{64}, std::size_t{61}})
        {
            runFminus(size2, other);
            sameInBlocks = sameInBlocks && other == fminusOutput;
        }
        const bool same = fminusOutput == cppOutput;
        const bool nonBlocking = program->nonBlocking("Convolver::process");
        std::printf("%s", std::format("example-convolver: {} taps, blocks of {}, FFTs of {}: FMinus and C++ the same: "
                                      "{}; against the direct convolution, {} samples late: {:.3g} of the peak (at "
                                      "most {:.0e}), silent before: {}; in calls of 64 and 61: {}; process "
                                      "nonblocking: {}\n",
                                      taps, hop, size, same ? "yes" : "no", hop, error / peak, kTolerance,
                                      silentFirst ? "yes" : "no", sameInBlocks ? "yes" : "no",
                                      nonBlocking ? "yes" : "no")
                              .c_str());
        return same && error / peak <= kTolerance && silentFirst && sameInBlocks && nonBlocking ? 0 : 1;
    }

    std::vector<float> direct(samples);
    const double fminusTime = nanosecondsPerSample([&] { runFminus(block, fminusOutput); }, samples, repeats);
    const double cppTime = nanosecondsPerSample(runCpp, samples, repeats);
    const double directTime =
        nanosecondsPerSample([&] { directFloat(input, response, direct); }, samples, std::max(1, repeats / 4));
    std::printf("%s", std::format("convolution with {} taps, by overlap-add in blocks of {} (FFTs of {} points), {} "
                                  "samples, {}\n"
                                  "  FMinus (JIT, -O{})  {:8.2f} ns/sample   the Convolver module, on the runtime's "
                                  "FFT\n"
                                  "  C++ (clang)        {:8.2f} ns/sample   the same, on the same FFT\n"
                                  "  C++, directly      {:8.2f} ns/sample   one multiply and add a tap a sample\n"
                                  "  FMinus / C++       {:8.2f}x;  direct / FMinus {:.1f}x\n"
                                  "  FMinus's output and C++'s are {}\n",
                                  taps, hop, size, samples,
                                  block == 0 ? std::string("1 call of process for the whole run")
                                             : std::format("calls of {} samples", block),
                                  level, fminusTime, cppTime, directTime, fminusTime / cppTime,
                                  directTime / fminusTime, fminusOutput == cppOutput ? "bit-identical" : "different")
                          .c_str());
    return 0;
}
