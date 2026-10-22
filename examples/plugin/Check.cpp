// examples/plugin/Check.cpp: fminus-plugin-check, the plugin example's headless check. It makes FMinus Filter's
// processor as a host does, prepares it at 48 kHz and at 44.1 kHz, runs signals through processBlock in stereo, and
// exits 1 unless:
//   - the parameters are the program's: the same ranges and defaults as filter.fm's fminus::range and fminus::default;
//   - prepareToPlay compiles once a rate: again at a new rate, not at the same one;
//   - every output is finite, the extremes included (resonance 1, the cutoff at 20 Hz and 20 kHz);
//   - the low-pass at 1 kHz takes a 10 kHz sine down by 20 dB or more and passes a 100 Hz one within 1 dB; the
//     high-pass the reverse; the band-pass, resonant, peaks at the cutoff at 1/k, as the analog filter does (which a
//     sample rate frozen wrong would move);
//   - the output is the same, bit for bit, in blocks of 64, 61, 1 and 1000 samples (more than prepareToPlay was told),
//     with the parameters moving at the same samples in every run, so the smoother's ramps land on the same samples;
//   - a moved cutoff glides: no step between samples larger than a bound (below);
//   - a switch between the modes is a crossfade: at resonance 1, no step larger than a bound, which the same switches
//     without filter.fm's glide break;
//   - reset (a host's stop or locate) starts the filter from rest: a ring stops, and the output is then a fresh
//     processor's, bit for bit; prepareToPlay again at the same rate (JUCE's AU, when a host switches offline
//     rendering) goes on as if it had not been called; releaseResources then prepareToPlay starts from rest;
//   - the tail the processor reports covers the longest ring, to 60 dB below its peak;
//   - one sample that is NaN, an infinity or huge does not stick: every output is finite, and the filter is soon back
//     on a clean run's output; without filter.fm's test of its input, the test of its states after a call still
//     brings it back, from the next block;
//   - a state saved and restored into another processor gives the same output, bit for bit;
//   - a program that does not compile, and one whose process is not [[clang::nonblocking]], are refused: the audio
//     passes through untouched, and the error is logged once.
// It prints each check, and what processBlock costs.
//
//     ctest --test-dir build-plugin
//     build-plugin/fminus-plugin-check_artefacts/Release/fminus-plugin-check
#include "Processor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace
{
    using fminus_filter::Processor;

    constexpr int kChannels = 2;
    constexpr int kLargestBlock = 512;   // what prepareToPlay is told
    constexpr double kPi = 3.141592653589793;

    enum Mode
    {
        lowPass = 0,
        bandPass = 1,
        highPass = 2,
    };

    struct Settings
    {
        float cutoff = 1000.0f;
        float resonance = 0.3f;
        int mode = lowPass;
    };

    // Settings that take effect at sample `at` of a run.
    struct Change
    {
        int at = 0;
        Settings settings;
    };

    struct Output
    {
        std::vector<float> left;
        std::vector<float> right;
        bool finite = true;
        float peak = 0.0f;
    };

    int failures = 0;

    void expect(bool ok, const std::string& what)
    {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        if (!ok)
            ++failures;
    }

    // As a host prepares a plugin.
    void prepare(Processor& processor, double rate)
    {
        processor.setRateAndBufferSizeDetails(rate, kLargestBlock);
        processor.prepareToPlay(rate, kLargestBlock);
    }

    // The parameters, as a host sets them: normalised, through each parameter.
    void apply(Processor& processor, const Settings& settings)
    {
        const auto set = [&processor](const char* id, float value)
        {
            juce::RangedAudioParameter* parameter = processor.state().getParameter(id);
            parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
        };
        set("cutoff", settings.cutoff);
        set("resonance", settings.resonance);
        set("mode", static_cast<float>(settings.mode));
    }

    // `input` through both channels of `processor`, in blocks of `block` samples, the settings changing as `changes`
    // say: a block ends where a change starts, so a change lands on the same sample whatever the block size.
    Output run(Processor& processor, const std::vector<float>& input, int block, const std::vector<Change>& changes)
    {
        const int total = static_cast<int>(input.size());
        Output output;
        output.left.resize(input.size());
        output.right.resize(input.size());
        juce::AudioBuffer<float> buffer(kChannels, block);
        juce::MidiBuffer midi;
        std::size_t next = 0;
        for (int position = 0; position < total;)
        {
            while (next < changes.size() && changes[next].at <= position)
                apply(processor, changes[next++].settings);
            const int end = next < changes.size() ? std::min(changes[next].at, total) : total;
            const int n = std::min(block, end - position);
            buffer.setSize(kChannels, n, false, false, true);
            for (int channel = 0; channel < kChannels; ++channel)
                std::copy_n(input.data() + position, n, buffer.getWritePointer(channel));
            processor.processBlock(buffer, midi);
            std::copy_n(buffer.getReadPointer(0), n, output.left.data() + position);
            std::copy_n(buffer.getReadPointer(1), n, output.right.data() + position);
            position += n;
        }
        for (const std::vector<float>* channel : {&output.left, &output.right})
            for (const float x : *channel)
            {
                output.finite = output.finite && std::isfinite(x);
                output.peak = std::max(output.peak, std::abs(x));
            }
        return output;
    }

    std::vector<float> sine(double rate, double frequency, double amplitude, int samples)
    {
        std::vector<float> signal(static_cast<std::size_t>(samples));
        for (std::size_t i = 0; i < signal.size(); ++i)
            signal[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / rate));
        return signal;
    }

    // White noise in [-amplitude, amplitude) from a fixed 32-bit LCG.
    std::vector<float> noise(int samples, float amplitude, std::uint32_t state)
    {
        std::vector<float> signal(static_cast<std::size_t>(samples));
        for (float& x : signal)
        {
            state = state * 1664525u + 1013904223u;
            x = amplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
        }
        return signal;
    }

    double rms(const std::vector<float>& signal, std::size_t from, std::size_t to)
    {
        double sum = 0.0;
        for (std::size_t i = from; i < to; ++i)
            sum += static_cast<double>(signal[i]) * static_cast<double>(signal[i]);
        return std::sqrt(sum / static_cast<double>(to - from));
    }

    bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

    bool sameBits(const Output& a, const Output& b)
    {
        return sameBits(a.left, b.left) && sameBits(a.right, b.right);
    }

    bool allZero(const Output& output)
    {
        const auto zero = [](float x) { return juce::exactlyEqual(x, 0.0f); };
        return std::all_of(output.left.begin(), output.left.end(), zero)
            && std::all_of(output.right.begin(), output.right.end(), zero);
    }

    float peakOf(const std::vector<float>& signal)
    {
        float peak = 0.0f;
        for (const float x : signal)
            peak = std::max(peak, std::abs(x));
        return peak;
    }

    // The gain of a sine at `frequency` through `processor` at `settings`, in dB: half a second of it, the last quarter
    // measured against the input (whole periods of every frequency used here, at both rates), after the filter and the
    // smoother have settled.
    double gainDb(Processor& processor, double rate, const Settings& settings, double frequency, bool& finite)
    {
        const int samples = static_cast<int>(rate / 2);
        const std::vector<float> input = sine(rate, frequency, 0.5, samples);
        const Output output = run(processor, input, kLargestBlock, {{0, settings}});
        finite = finite && output.finite;
        const auto from = static_cast<std::size_t>(samples / 2);
        const auto to = static_cast<std::size_t>(samples);
        return 20.0 * std::log10(rms(output.left, from, to) / rms(input, from, to));
    }

    // The largest difference between neighbouring samples of `signal` in [from, to).
    double largestStep(const std::vector<float>& signal, std::size_t from, std::size_t to)
    {
        double step = 0.0;
        for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i)
            step = std::max(step, std::abs(static_cast<double>(signal[i]) - static_cast<double>(signal[i - 1])));
        return step;
    }

    // Counts what the processor logs (it logs FMinus's errors once).
    class CountingLogger final : public juce::Logger
    {
    public:
        int messages = 0;

    private:
        void logMessage(const juce::String&) override
        {
            ++messages;
        }
    };

    // ---- the checks -------------------------------------------------------------------------------------------------

    void checkParameters()
    {
        std::printf("the parameters against filter.fm's\n");
        std::string errors;
        const std::unique_ptr<fminus::Program> program = fminus::Program::compile(Processor::embeddedSource(), errors);
        expect(program != nullptr, "filter.fm compiles, its parameters knobs" + (errors.empty() ? "" : ":\n" + errors));
        if (!program)
            return;
        Processor processor;
        for (const char* id : {"cutoff", "resonance", "mode"})
        {
            const fminus::Program::Parameter* declared = program->parameter(std::string("Filter::") + id);
            juce::RangedAudioParameter* parameter = processor.state().getParameter(id);
            if (declared == nullptr || parameter == nullptr || !declared->minimum || !declared->maximum)
            {
                expect(false, std::format("{}: in filter.fm, with a range, and in the plugin", id));
                continue;
            }
            const juce::NormalisableRange<float>& range = parameter->getNormalisableRange();
            const double ours = parameter->convertFrom0to1(parameter->getDefaultValue());
            const double theirs = declared->defaultValue.value_or(0.0);   // no default: 0, as an instance starts
            const auto near = [](double x, double y) { return std::abs(x - y) <= 1e-4 * std::max(1.0, std::abs(y)); };
            expect(near(range.start, *declared->minimum) && near(range.end, *declared->maximum) && near(ours, theirs),
                   std::format("{}: the plugin's [{:g}, {:g}], default {:g}; filter.fm's [{:g}, {:g}], default {:g}",
                               id, range.start, range.end, ours, *declared->minimum, *declared->maximum, theirs));
        }
    }

    // The low-, band- and high-pass's responses on `processor`, prepared at `rate`.
    void checkResponses(Processor& processor, double rate)
    {
        bool finite = true;
        const Settings low{1000.0f, 0.3f, lowPass};
        const Settings high{1000.0f, 0.3f, highPass};
        const Settings band{1000.0f, 0.95f, bandPass};
        const double lowAt100 = gainDb(processor, rate, low, 100.0, finite);
        const double lowAt10k = gainDb(processor, rate, low, 10000.0, finite);
        const double highAt100 = gainDb(processor, rate, high, 100.0, finite);
        const double highAt10k = gainDb(processor, rate, high, 10000.0, finite);
        const double bandAtCutoff = gainDb(processor, rate, band, 1000.0, finite);
        const double peak = 20.0 * std::log10(1.0 / (2.0 - 1.96 * static_cast<double>(band.resonance)));
        expect(lowAt10k <= -20.0 && std::abs(lowAt100) <= 1.0,
               std::format("low-pass at 1 kHz: 10 kHz at {:.1f} dB (at most -20), 100 Hz at {:+.2f} dB (within 1)",
                           lowAt10k, lowAt100));
        expect(highAt100 <= -20.0 && std::abs(highAt10k) <= 1.0,
               std::format("high-pass at 1 kHz: 100 Hz at {:.1f} dB (at most -20), 10 kHz at {:+.2f} dB (within 1)",
                           highAt100, highAt10k));
        expect(std::abs(bandAtCutoff - peak) <= 0.5,
               std::format("band-pass at 1 kHz, resonance {:g}: 1 kHz at {:+.2f} dB, 1/k {:+.2f} dB (within 0.5)",
                           static_cast<double>(band.resonance), bandAtCutoff, peak));
        expect(finite, "the responses' outputs are finite");
    }

    // The same run in blocks of 64, 61, 1 and 1000 samples, on fresh processors: the parameters move at the same
    // samples in each (every 4800 samples, across the modes and to the extremes), so the outputs are the same, bit for
    // bit. Each channel has its own instance, fed the same input: the two channels are the same too.
    void checkBlockSizes(double rate)
    {
        constexpr int kSegment = 4800;
        const std::vector<Change> changes{
            {0 * kSegment, {500.0f, 0.3f, lowPass}},       {1 * kSegment, {8000.0f, 0.3f, lowPass}},
            {2 * kSegment, {2000.0f, 0.9f, lowPass}},      {3 * kSegment, {300.0f, 0.9f, bandPass}},
            {4 * kSegment, {12000.0f, 0.5f, highPass}},    {5 * kSegment, {20000.0f, 1.0f, highPass}},
            {6 * kSegment, {20.0f, 1.0f, lowPass}},        {7 * kSegment, {20000.0f, 1.0f, bandPass}},
            {8 * kSegment, {20.0f, 0.0f, highPass}},
        };
        std::vector<float> input = noise(9 * kSegment, 0.5f, 0x12345678u);
        const std::vector<float> tone = sine(rate, 220.0, 0.3, 9 * kSegment);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] += tone[i];

        Processor reference;
        prepare(reference, rate);
        const Output expected = run(reference, input, 64, changes);
        expect(expected.finite, std::format("blocks of 64: every output is finite (the peak {:.3g})",
                                            static_cast<double>(expected.peak)));
        expect(sameBits(expected.left, expected.right), "blocks of 64: the two channels' outputs are the same");
        for (const int block : {61, 1, 1000})
        {
            Processor processor;
            prepare(processor, rate);
            const Output output = run(processor, input, block, changes);
            expect(output.finite && sameBits(output, expected),
                   std::format("blocks of {}: the same output as in blocks of 64, bit for bit", block));
        }
    }

    // A high-pass whose cutoff moves from 50 Hz to 5 kHz, under a 100 Hz sine of amplitude A = 0.5: from passing it to
    // taking it down by about 70 dB. A sine moves by at most 2 A sin(pi f / fs) between samples (0.0065 at 48 kHz); the
    // glide adds the change in the filter's gain, at most its 12 dB an octave times 6.6 octaves in 20 ms, under 1% of A
    // a sample, and the two peak at different phases of the sine. The bound is twice the sine's own step, 4 A sin(pi f
    // / fs). A cutoff that stepped instead (Processor::kCutoffRampSeconds at 0) moves the output by 0.14 in one sample
    // here, ten times the bound.
    void checkGlide(Processor& processor, double rate)
    {
        constexpr double kFrequency = 100.0;
        constexpr double kAmplitude = 0.5;
        const int samples = static_cast<int>(0.4 * rate);
        const int moved = static_cast<int>(0.2 * rate);
        const std::vector<float> input = sine(rate, kFrequency, kAmplitude, samples);
        const Output output =
            run(processor, input, kLargestBlock, {{0, {50.0f, 0.3f, highPass}}, {moved, {5000.0f, 0.3f, highPass}}});
        const double own = 2.0 * kAmplitude * std::sin(kPi * kFrequency / rate);
        const double bound = 2.0 * own;
        const auto at = static_cast<std::size_t>(moved);
        const double before = largestStep(output.left, at / 2, at);
        const double during = largestStep(output.left, at, at + static_cast<std::size_t>(0.05 * rate));
        const auto last = static_cast<std::size_t>(samples) - 4800;
        const double after = 20.0 * std::log10(rms(output.left, last, input.size()) / rms(input, last, input.size()));
        expect(output.finite && during <= bound,
               std::format("the cutoff moved, 50 Hz to 5 kHz: the largest step {:.4f} (the sine's own {:.4f}, before "
                           "the move {:.4f}; at most {:.4f})",
                           during, own, before, bound));
        expect(after <= -40.0, std::format("and it got there: 100 Hz at {:.1f} dB after the glide", after));
    }

    // The modes, switched at resonance 1 under a sine at the cutoff (1 kHz, amplitude A = 0.25), through every switch:
    // low to band, band to high, high to low, low to high, high to band, band to low. There the three outputs are as
    // large, A/k = 25 A, and a quarter or half a period apart, so a switch at once steps by up to twice that. filter.fm
    // crossfades them instead: an output's step is then a weighted mean of the two outputs' own steps, 2 (A/k) sin(pi f
    // / fs) (0.82 at 48 kHz), plus the weights' change a sample, 50 / fs, times the outputs' difference, at most 2 A/k:
    // 0.013 at 48 kHz, under 2% of their own step. The bound is 1.1 times their own step. The same switches with the
    // glide taken out of filter.fm (its step a whole weight) must break it, or the bound would show nothing.
    double largestSwitchStep(Processor& processor, double rate, bool& finite)
    {
        const int segment = static_cast<int>(0.1 * rate);   // a whole number of the sine's periods, at both rates
        std::vector<Change> changes;
        int at = 0;
        for (const Mode mode : {lowPass, bandPass, highPass, lowPass, highPass, bandPass, lowPass})
        {
            changes.push_back({at, {1000.0f, 1.0f, mode}});
            at += segment;
        }
        const std::vector<float> input = sine(rate, 1000.0, 0.25, at);
        const Output output = run(processor, input, kLargestBlock, changes);
        finite = finite && output.finite;
        // From half a segment on: the filter has settled (its envelope's time constant is 2 Q / w, 8 ms, here).
        return largestStep(output.left, static_cast<std::size_t>(segment / 2), input.size());
    }

    void checkModeSwitches(double rate)
    {
        const double own = 2.0 * (0.25 / 0.04) * std::sin(kPi * 1000.0 / rate);
        const double bound = 1.1 * own;
        bool finite = true;
        Processor processor;
        prepare(processor, rate);
        const double gliding = largestSwitchStep(processor, rate, finite);
        expect(finite && gliding <= bound,
               std::format("the mode switched six ways at resonance 1: the largest step {:.3f} (the outputs' own "
                           "{:.3f}; at most {:.3f})",
                           gliding, own, bound));

        std::string instant = Processor::embeddedSource();
        const std::string glide = "const float glide = 50.0f / sampleRate[i];";
        const std::size_t found = instant.find(glide);
        if (found == std::string::npos)
        {
            expect(false, "filter.fm's glide, to take out: " + glide);
            return;
        }
        instant.replace(found, glide.size(), "const float glide = 1.0f;");
        Processor switching(instant);
        prepare(switching, rate);
        const double stepped = switching.running() ? largestSwitchStep(switching, rate, finite) : 0.0;
        expect(stepped > bound, std::format("and without filter.fm's glide, the largest step {:.3f}: the bound "
                                            "catches it",
                                            stepped));
    }

    // reset, prepareToPlay and releaseResources, between blocks, around a ring: a low-pass at 20 Hz and resonance 1
    // rings for seconds after a burst.
    void checkReset(double rate)
    {
        const std::vector<float> burst = sine(rate, 20.0, 0.5, static_cast<int>(0.1 * rate));
        const std::vector<float> silence(static_cast<std::size_t>(0.5 * rate), 0.0f);
        const std::vector<float> after = noise(static_cast<int>(0.2 * rate), 0.5f, 0x6c8e9cf5u);
        const Settings ringing{20.0f, 1.0f, lowPass};
        const Settings next{5000.0f, 0.5f, highPass};

        // A reset stops the ring at once: silence in, every output 0.
        Processor processor;
        apply(processor, ringing);
        prepare(processor, rate);
        run(processor, burst, 64, {});
        const float ring = run(processor, std::vector<float>(silence.begin(), silence.begin() + 4800), 64, {}).peak;
        processor.reset();
        const bool stopped = allZero(run(processor, silence, 64, {}));
        expect(ring > 0.1f && stopped,
               std::format("reset stops a ring (its peak {:.3f} before): silence in, every output 0 after", ring));

        // A reset in the middle of the cutoff's glide: the output from then on is a fresh processor's, bit for bit.
        apply(processor, next);
        // 300 samples, 6 or 7 ms of the 20 ms in which the cutoff glides from 20 Hz to 5 kHz and the mode crossfades.
        run(processor, std::vector<float>(burst.begin(), burst.begin() + 300), 64, {});
        processor.reset();
        const Output afterReset = run(processor, after, 64, {});
        Processor fresh;
        apply(fresh, next);
        prepare(fresh, rate);
        const Output afterPrepare = run(fresh, after, 64, {});
        expect(sameBits(afterReset, afterPrepare),
               "and after a reset in the middle of a glide, the same output, bit for bit, as a fresh processor's");

        // prepareToPlay again at the rate, mid-ring: the filter goes on, as if it had not been called.
        Processor prepared;
        Processor uninterrupted;
        for (Processor* each : {&prepared, &uninterrupted})
        {
            apply(*each, ringing);
            prepare(*each, rate);
            run(*each, burst, 64, {});
        }
        prepare(prepared, rate);
        const Output goneOn = run(prepared, silence, 64, {});
        expect(prepared.compilations() == 1 && goneOn.peak > 0.1f
                   && sameBits(goneOn, run(uninterrupted, silence, 64, {})),
               "prepareToPlay again at the same rate, mid-ring: the same output, bit for bit, as without it");

        // releaseResources, then prepareToPlay at the same rate: from rest, as a fresh processor.
        Processor released;
        apply(released, ringing);
        prepare(released, rate);
        run(released, burst, 64, {});
        released.releaseResources();
        prepare(released, rate);
        Processor again;
        apply(again, ringing);
        prepare(again, rate);
        expect(sameBits(run(released, after, 64, {}), run(again, after, 64, {})),
               "releaseResources then prepareToPlay: the same output, bit for bit, as a fresh processor's");
    }

    // The longest ring: the cutoff at 20 Hz, resonance 1, in each mode, after two periods of a 20 Hz sine. The last
    // sample above 60 dB under the output's peak comes within the tail the processor reports (Processor.cpp derives
    // 2.75 s; it reports 3).
    void checkTail(double rate)
    {
        const auto stop = static_cast<std::size_t>(0.1 * rate);
        std::vector<float> input = sine(rate, 20.0, 0.5, static_cast<int>((Processor::kTailSeconds + 1.1) * rate));
        std::fill(input.begin() + static_cast<std::ptrdiff_t>(stop), input.end(), 0.0f);
        double longest = 0.0;
        double tail = 0.0;
        bool finite = true;
        for (const Mode mode : {lowPass, bandPass, highPass})
        {
            Processor processor;
            apply(processor, {20.0f, 1.0f, mode});
            prepare(processor, rate);
            tail = processor.getTailLengthSeconds();
            const Output output = run(processor, input, kLargestBlock, {});
            finite = finite && output.finite;
            const float floor = 1e-3f * peakOf(output.left);
            std::size_t last = stop;
            for (std::size_t i = stop; i < output.left.size(); ++i)
                if (std::abs(output.left[i]) > floor)
                    last = i;
            longest = std::max(longest, static_cast<double>(last + 1 - stop) / rate);
        }
        expect(finite && longest <= tail,
               std::format("the longest ring (20 Hz, resonance 1) falls 60 dB under its peak {:.2f} s after its input "
                           "stops: within the tail reported, {:g} s",
                           longest, tail));
    }

    // One sample that is NaN, an infinity or huge (1e37: it would take the integrators' states past 1e30) in a 220 Hz
    // sine, through a low-pass at 1 kHz and resonance 1. filter.fm takes it as 0: every output is finite, that sample's
    // too, and 0.2 s later (25 times the filter's time constant, 2 Q / w = 8 ms) the output is within 1e-4 of the clean
    // sine's. Then the same NaN with filter.fm's test of its input taken out: the rest of its block is NaN, and the
    // test of the states after each call brings the filter back from the next block on.
    void checkNonFinite(double rate)
    {
        constexpr std::size_t kAt = 1000;
        constexpr int kBlock = 64;
        const auto settled = kAt + static_cast<std::size_t>(0.2 * rate);
        const Settings settings{1000.0f, 1.0f, lowPass};
        const std::vector<float> clean = sine(rate, 220.0, 0.5, static_cast<int>(0.5 * rate));
        Processor reference;
        apply(reference, settings);
        prepare(reference, rate);
        const Output expected = run(reference, clean, kBlock, {});
        const auto distance = [&expected, settled](const Output& output)
        {
            double off = 0.0;
            for (const std::vector<float>* channel : {&output.left, &output.right})
                for (std::size_t i = settled; i < channel->size(); ++i)
                    off = std::max(off, std::abs(static_cast<double>((*channel)[i])
                                                 - static_cast<double>(expected.left[i])));
            return off;
        };
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity(), 1e37f})
        {
            std::vector<float> input = clean;
            input[kAt] = bad;
            Processor processor;
            apply(processor, settings);
            prepare(processor, rate);
            const Output output = run(processor, input, kBlock, {});
            const double off = distance(output);
            expect(output.finite && off <= 1e-4,
                   std::format("one sample of {:g} in: every output finite, and 0.2 s later within {:.1e} of the clean "
                               "run's (at most 1e-4)",
                               static_cast<double>(bad), off));
        }

        std::string unguarded = Processor::embeddedSource();
        const std::string test = "const float v0 = fabsf(x) < 1e30f ? x : 0.0f;";
        const std::size_t found = unguarded.find(test);
        if (found == std::string::npos)
        {
            expect(false, "filter.fm's test of its input, to take out: " + test);
            return;
        }
        unguarded.replace(found, test.size(), "const float v0 = x;");
        std::vector<float> input = clean;
        input[kAt] = std::numeric_limits<float>::quiet_NaN();
        Processor processor(unguarded);
        apply(processor, settings);
        prepare(processor, rate);
        const Output output = run(processor, input, kBlock, {});
        const std::size_t next = (kAt / kBlock + 1) * kBlock;   // the next block's first sample
        std::size_t lost = 0;
        bool finite = processor.running();
        for (std::size_t i = 0; i < output.left.size(); ++i)
        {
            const bool here = std::isfinite(output.left[i]) && std::isfinite(output.right[i]);
            if (i >= kAt && i < next)
                lost += here ? 0 : 1;
            else
                finite = finite && here;
        }
        const double off = distance(output);
        expect(lost == next - kAt && finite && off <= 1e-4,
               std::format("without filter.fm's test of its input, a NaN makes the rest of its block NaN ({} of {} "
                           "samples), and the filter is back from the next block (0.2 s later within {:.1e})",
                           lost, next - kAt, off));
    }

    void checkState(double rate)
    {
        const std::vector<float> input = noise(24000, 0.5f, 0x9e3779b9u);
        Processor saved;
        apply(saved, {3000.0f, 0.7f, bandPass});
        juce::MemoryBlock state;
        saved.getStateInformation(state);
        Processor restored;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        bool same = true;
        for (const char* id : {"cutoff", "resonance", "mode"})
            same = same
                && juce::exactlyEqual(saved.state().getRawParameterValue(id)->load(),
                                      restored.state().getRawParameterValue(id)->load());
        expect(same, std::format("a saved state ({} bytes) restores every parameter's value exactly", state.getSize()));

        Processor fresh;
        for (Processor* processor : {&saved, &restored, &fresh})
            prepare(*processor, rate);
        const Output a = run(saved, input, 128, {});
        const Output b = run(restored, input, 128, {});
        const Output c = run(fresh, input, 128, {});
        expect(sameBits(a, b) && !sameBits(a, c),
               "and the same output, bit for bit, as the processor it was saved from (and not a fresh one's)");
    }

    void checkRefusals()
    {
        CountingLogger logger;
        juce::Logger::setCurrentLogger(&logger);
        const std::vector<float> input = noise(4000, 0.5f, 0x2545f491u);
        Output passed;
        passed.left = input;
        passed.right = input;

        Processor broken("module Filter { float x }");   // a syntax error
        prepare(broken, 48000.0);
        prepare(broken, 44100.0);
        const std::string error = broken.errors().substr(0, broken.errors().find('\n'));
        expect(!broken.running() && broken.compilations() == 2 && error.find("error") != std::string::npos,
               "a program that does not compile is refused, at each rate: " + error);
        expect(logger.messages == 1, std::format("and logged once ({} message)", logger.messages));
        expect(sameBits(run(broken, input, 64, {}), passed), "and the audio passes through, bit for bit");

        std::string blocking = Processor::embeddedSource();
        const std::string marker = " [[clang::nonblocking]]";
        blocking.erase(blocking.find(marker), marker.size());
        Processor unchecked(blocking);
        prepare(unchecked, 48000.0);
        expect(!unchecked.running() && unchecked.errors().find("nonblocking") != std::string::npos,
               "filter.fm without [[clang::nonblocking]] compiles, and is refused: " + unchecked.errors());
        expect(sameBits(run(unchecked, input, 64, {}), passed) && logger.messages == 2,
               "and the audio passes through, bit for bit, the reason logged once");
        juce::Logger::setCurrentLogger(nullptr);
    }

    // What processBlock costs: 10 seconds of stereo noise in blocks of 512, the best of five runs.
    void timeIt(double rate)
    {
        const std::vector<float> input = noise(static_cast<int>(10 * rate), 0.5f, 0x5bd1e995u);
        Processor processor;
        prepare(processor, rate);
        apply(processor, {});
        double best = 1e30;
        for (int repeat = 0; repeat < 5; ++repeat)
        {
            const auto start = std::chrono::steady_clock::now();
            run(processor, input, kLargestBlock, {});
            const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
            best = std::min(best, elapsed.count());
        }
        std::printf("  processBlock, the copies in and out included: %.1f ns a sample a channel (%.2f%% of one core "
                    "at %g kHz, in stereo)\n",
                    best / (static_cast<double>(input.size()) * kChannels),
                    100.0 * best * 1e-9 / (static_cast<double>(input.size()) / rate), rate / 1000);
    }
} // namespace

int main()
{
    // JUCE's message manager, which an AudioProcessorValueTreeState's timer wants. No message loop runs: the check
    // never needs one (copyState flushes the parameters itself).
    const juce::ScopedJuceInitialiser_GUI initialiser;
    std::printf("fminus-plugin-check: FMinus Filter's processor, on FMinus %s and LLVM %s\n", FMINUS_VERSION_STRING,
                FMINUS_LLVM_PACKAGE_VERSION);
    checkParameters();

    Processor processor;   // prepared at both rates in turn, as a host whose rate changes does
    for (const double rate : {48000.0, 44100.0})
    {
        std::printf("at %g kHz\n", rate / 1000);
        const int before = processor.compilations();
        prepare(processor, rate);
        prepare(processor, rate);
        expect(processor.running() && juce::exactlyEqual(processor.compiledRate(), rate)
                   && processor.compilations() == before + 1,
               std::format("prepareToPlay compiled filter.fm for {:g} Hz, once in two ({} in all){}", rate,
                           processor.compilations(), processor.errors().empty() ? "" : ": " + processor.errors()));
        if (!processor.running())
            continue;
        checkResponses(processor, rate);
        checkGlide(processor, rate);
        checkModeSwitches(rate);
        checkBlockSizes(rate);
        checkReset(rate);
        checkTail(rate);
        checkNonFinite(rate);
        checkState(rate);
    }
    std::printf("refusals\n");
    checkRefusals();
    std::printf("cost\n");
    timeIt(48000.0);

    std::printf("fminus-plugin-check: %s\n",
                failures == 0 ? "passed" : std::format("{} check(s) FAILED", failures).c_str());
    return failures == 0 ? 0 : 1;
}
