// examples/voices/bench.cpp: the voices example's harness, a host of FMinus's public API alone (fminus/FMinus.h). It
// compiles voices.fm with the Synth's sample rate frozen, allocates the Synth module in its own memory (the size and
// alignment the program gives, zeroed, its parameters at their defaults), plays a score of notes through it, with the
// cutoff knob turned on the way, compares the result with the same modules written as C++ structs, and times both.
// With --check (what ctest runs) it only compares, and fails when the two differ at all, when calling process a block
// at a time changes the output, when the knob's turn is not heard, when the synth is silent, when process is not
// [[clang::nonblocking]], when the C++ structs are not laid out as FMinus lays out the modules, or when a program with
// the rate frozen at 44.1 kHz is not C++ at that rate (the frozen value reaching the code).
//
//     build-dev/bin/example-voices
//     build-dev/bin/example-voices --block 64
//
// The C++ version is built with -ffp-contract=off (examples/CMakeLists.txt), so that, like FMinus's IR, it never fuses
// a multiply and an add, and it takes expf from fminus::math, which computes it as voices.fm does (FMinus's default
// math mode).
#include "fminus/FMinus.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
#include <vector>

namespace
{
    // voices.fm's modules in C++, field for field and line for line: the same fields in the same order, which C++
    // lays out as FMinus does (--check compares the two).
    struct Osc
    {
        float phase = 0.0f;
        float step = 0.0f;

        void start(float frequency, float rate)
        {
            phase = 0.0f;
            step = frequency / rate;
        }

        float tick()
        {
            const float out = 2.0f * phase - 1.0f;
            phase += step;
            if (phase >= 1.0f)
                phase -= 1.0f;
            return out;
        }
    };

    struct OnePole
    {
        float y = 0.0f;
        float k = 0.0f;

        float tick(float x)
        {
            y += k * (x - y);
            return y;
        }
    };

    struct Envelope
    {
        float level = 0.0f;
        float up = 0.0f;
        float down = 0.0f;
        bool held = false;

        float tick()
        {
            level = held ? std::fmin(1.0f, level + up) : std::fmax(0.0f, level - down);
            return level;
        }
    };

    struct Voice
    {
        Osc osc;
        OnePole filter;
        Envelope envelope;

        float tick()
        {
            return envelope.tick() * filter.tick(osc.tick());
        }
    };

    // A parameter's slot, as FMinus lays one out: the stream's pointer, then the value (fminus::Program::Parameter).
    struct Slot
    {
        const float* stream = nullptr;
        float value = 0.0f;
    };

    struct Synth
    {
        Slot cutoff;
        Slot attack;
        Slot release;
        Slot gain;
        Slot rate;
        std::array<Voice, 8> voices;

        // Each parameter at its fminus::default, as the program describes it: what setDefaults sets.
        explicit Synth(const fminus::Program& program)
        {
            auto initial = [&](const char* name) {
                const fminus::Program::Parameter* parameter = program.parameter(name);
                return parameter != nullptr ? static_cast<float>(parameter->defaultValue.value_or(0.0)) : 0.0f;
            };
            cutoff.value = initial("Synth::cutoff");
            attack.value = initial("Synth::attack");
            release.value = initial("Synth::release");
            gain.value = initial("Synth::gain");
            rate.value = initial("Synth::rate");
        }

        void noteOn(int v, float frequency)
        {
            voices[static_cast<std::size_t>(v)].osc.start(frequency, rate.value);
            voices[static_cast<std::size_t>(v)].envelope.held = true;
        }

        void noteOff(int v)
        {
            voices[static_cast<std::size_t>(v)].envelope.held = false;
        }

        void process(float* __restrict out, int n)
        {
            const float k = 1.0f - fminus::math::exp(-6.28318531f * cutoff.value / rate.value);
            const float up = 1.0f / (attack.value * rate.value);
            const float down = 1.0f / (release.value * rate.value);
            for (Voice& voice : voices)
            {
                voice.filter.k = k;
                voice.envelope.up = up;
                voice.envelope.down = down;
            }
            for (int i = 0; i < n; ++i)
            {
                float mix = 0.0f;
                for (Voice& voice : voices)
                    mix += voice.tick();
                out[i] = gain.value * mix;
            }
        }
    };

    // A note's start or end, or a turn of the cutoff knob, at a sample.
    struct Event
    {
        std::size_t at;
        int voice;
        float frequency;      // 0: the note ends
        float cutoff = 0.0f;  // not 0: the cutoff knob goes to it, and the event is no note's
    };

    // Eight notes: a chord of four, its lower half let go half-way, and four more that come in over it; the cutoff
    // knob turned down to 600 Hz three quarters of the way.
    std::vector<Event> score(std::size_t samples)
    {
        const std::array<float, 8> notes{130.81f, 164.81f, 196.0f, 246.94f, 261.63f, 329.63f, 392.0f, 493.88f};
        std::vector<Event> events;
        for (int v = 0; v < 4; ++v)
            events.push_back({0, v, notes[static_cast<std::size_t>(v)]});
        events.push_back({samples / 2, 0, 0.0f});
        events.push_back({samples / 2, 1, 0.0f});
        for (int v = 4; v < 8; ++v)
            events.push_back({samples / 2 + static_cast<std::size_t>(v) * samples / 32, v,
                              notes[static_cast<std::size_t>(v)]});
        events.push_back({samples * 3 / 4, 0, 0.0f, 600.0f});
        return events;
    }

    // Plays `events` through a synth: process up to each event, then apply it, in calls of at most `block` samples
    // (0: as long as the gap between events). The same driver runs FMinus's modules and the C++ structs.
    template <typename Process, typename NoteOn, typename NoteOff, typename Cutoff>
    void play(const std::vector<Event>& events, std::size_t block, std::vector<float>& out, Process&& process,
              NoteOn&& noteOn, NoteOff&& noteOff, Cutoff&& cutoff)
    {
        std::size_t done = 0;
        auto until = [&](std::size_t end) {
            while (done < end)
            {
                const std::size_t n = block == 0 ? end - done : std::min(block, end - done);
                process(out.data() + done, static_cast<int>(n));
                done += n;
            }
        };
        for (const Event& event : events)
        {
            until(event.at);
            if (event.cutoff > 0.0f)
                cutoff(event.cutoff);
            else if (event.frequency > 0.0f)
                noteOn(event.voice, event.frequency);
            else
                noteOff(event.voice);
        }
        until(out.size());
    }

    // FMinus's Synth: its methods, and memory the host owns for one.
    class FminusSynth
    {
    public:
        FminusSynth(fminus::Program& program, const fminus::Program::Module& module, std::string& errors)
            : module_(module)
            , cutoff_(program.parameter("Synth::cutoff"))
        {
            noteOn_ = program.function<void(void*, int, float)>("Synth::note_on", errors);
            noteOff_ = program.function<void(void*, int)>("Synth::note_off", errors);
            process_ = program.function<void(void*, float*, int)>("Synth::process", errors);
        }

        bool found() const
        {
            return cutoff_ != nullptr && noteOn_ != nullptr && noteOff_ != nullptr && process_ != nullptr;
        }

        // A new synth: aligned as the module needs, zeroed, as every module starts, then its parameters at their
        // defaults. (C's aligned_alloc takes an alignment of at least a max_align_t's on some systems, and a size
        // that is a multiple of it: both are rounded up.)
        void reset()
        {
            const std::size_t alignment = std::max(module_.alignment, alignof(std::max_align_t));
            const std::size_t size = (module_.size + alignment - 1) / alignment * alignment;
            memory_.reset(static_cast<unsigned char*>(std::aligned_alloc(alignment, size)));
            if (!memory_)
                throw std::bad_alloc();
            std::memset(memory_.get(), 0, size);
            fminus::Program::setDefaults(memory_.get(), module_);
        }

        void process(float* out, int n) { process_(memory_.get(), out, n); }
        void noteOn(int voice, float frequency) { noteOn_(memory_.get(), voice, frequency); }
        void noteOff(int voice) { noteOff_(memory_.get(), voice); }
        void setCutoff(float hertz) { fminus::Program::setValue(memory_.get(), *cutoff_, static_cast<double>(hertz)); }

    private:
        struct Free
        {
            void operator()(unsigned char* p) const { std::free(p); }
        };

        const fminus::Program::Module& module_;
        const fminus::Program::Parameter* cutoff_;
        void (*noteOn_)(void*, int, float) = nullptr;
        void (*noteOff_)(void*, int) = nullptr;
        void (*process_)(void*, float*, int) = nullptr;
        std::unique_ptr<unsigned char, Free> memory_;
    };

    // Whether a module's layout, as the program gives it, is the C++ struct's: its size and alignment, and each
    // field's offset (`offsets`, in declaration order).
    bool sameLayout(const fminus::Program::Module& module, std::size_t size, std::size_t alignment,
                    const std::vector<std::size_t>& offsets)
    {
        if (module.size != size || module.alignment != alignment || module.fields.size() != offsets.size())
            return false;
        for (std::size_t i = 0; i < offsets.size(); ++i)
        {
            if (module.fields[i].offset != offsets[i])
                return false;
        }
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
} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"The voices example: voices.fm's modules against C++ structs, compared and timed.", "example-voices"};
    std::string path = std::string(FMINUS_EXAMPLE_DIR) + "/voices.fm";
    std::size_t samples = 96000;
    std::size_t block = 0;
    int repeats = 20;
    int level = 2;
    float rate = 48000.0f;
    bool check = false;
    app.add_option("source", path, "The FMinus source")->capture_default_str();
    app.add_option("--samples", samples, "Samples per run")
        ->check(CLI::Range(std::size_t{64}, static_cast<std::size_t>(std::numeric_limits<int>::max())))
        ->capture_default_str();
    app.add_option("--block", block, "Samples per call of process at most (0: from one event to the next)")
        ->capture_default_str();
    app.add_option("--repeat", repeats, "Runs to time (the best counts)")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    app.add_option("-O", level, "FMinus's optimisation level, 0 to 3")->check(CLI::Range(0, 3))->capture_default_str();
    app.add_option("--rate", rate, "The sample rate, Hz")->check(CLI::PositiveNumber)->capture_default_str();
    app.add_flag("--check", check, "Compare only: exit 1 when FMinus and C++ differ at all");
    CLI11_PARSE(app, argc, argv);

    std::ifstream file(path);
    if (!file)
    {
        std::fprintf(stderr, "example-voices: cannot read %s\n", path.c_str());
        return 1;
    }
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string errors;
    fminus::Program::Options options;
    options.optimisation = level;
    options.fileName = path;
    options.parameters["Synth::rate"] = fminus::Program::Binding::frozen(static_cast<double>(rate));   // known at start
    std::unique_ptr<fminus::Program> program = fminus::Program::compile(source, errors, options);
    const fminus::Program::Module* module = program ? program->module("Synth") : nullptr;
    if (module == nullptr)
    {
        std::fprintf(stderr, "example-voices: %s", program ? "voices.fm has no module Synth\n" : errors.c_str());
        return 1;
    }
    FminusSynth fminusSynth(*program, *module, errors);
    if (!fminusSynth.found())
    {
        std::fprintf(stderr, "example-voices: %s", errors.c_str());
        return 1;
    }

    const std::vector<Event> events = score(samples);
    std::vector<float> fminusOutput(samples);
    std::vector<float> cppOutput(samples);
    auto runFminus = [&](std::size_t size, std::vector<float>& out) {
        fminusSynth.reset();
        play(
            events, size, out, [&](float* o, int n) { fminusSynth.process(o, n); },
            [&](int v, float f) { fminusSynth.noteOn(v, f); }, [&](int v) { fminusSynth.noteOff(v); },
            [&](float hertz) { fminusSynth.setCutoff(hertz); });
    };
    auto runCpp = [&] {
        Synth synth(*program);
        synth.rate.value = rate;
        play(
            events, block, cppOutput, [&](float* o, int n) { synth.process(o, n); },
            [&](int v, float f) { synth.noteOn(v, f); }, [&](int v) { synth.noteOff(v); },
            [&](float hertz) { synth.cutoff.value = hertz; });
    };
    runFminus(block, fminusOutput);
    runCpp();

    if (check)
    {
        // The modules' layouts are the C++ structs': what lets a host read a field, or mirror the module in C++.
        const fminus::Program::Module* voice = program->module("Voice");
        const fminus::Program::Module* envelope = program->module("Envelope");
        const bool layouts =
            voice != nullptr && envelope != nullptr
            && sameLayout(*module, sizeof(Synth), alignof(Synth),
                          {offsetof(Synth, cutoff), offsetof(Synth, attack), offsetof(Synth, release),
                           offsetof(Synth, gain), offsetof(Synth, rate), offsetof(Synth, voices)})
            && sameLayout(*voice, sizeof(Voice), alignof(Voice),
                          {offsetof(Voice, osc), offsetof(Voice, filter), offsetof(Voice, envelope)})
            && sameLayout(*envelope, sizeof(Envelope), alignof(Envelope),
                          {offsetof(Envelope, level), offsetof(Envelope, up), offsetof(Envelope, down),
                           offsetof(Envelope, held)});
        // Calls of 64 samples, then of 61 (blocks that end mid-way through), must give the output that calls from
        // one event to the next do.
        bool sameInBlocks = true;
        std::vector<float> blockOutput(samples);
        for (const std::size_t size : {std::size_t{64}, std::size_t{61}})
        {
            runFminus(size, blockOutput);
            sameInBlocks = sameInBlocks && blockOutput == fminusOutput;
        }
        const bool same = fminusOutput == cppOutput;
        const bool sounds = std::any_of(fminusOutput.begin(), fminusOutput.end(), [](float x) { return x != 0.0f; });
        const bool nonBlocking = program->nonBlocking("Synth::process");
        // The knob's turn is heard: the same score with the cutoff left alone differs from three quarters on.
        std::vector<float> untouched(samples);
        {
            fminusSynth.reset();
            std::vector<Event> notes = events;
            std::erase_if(notes, [](const Event& event) { return event.cutoff > 0.0f; });
            play(
                notes, block, untouched, [&](float* o, int n) { fminusSynth.process(o, n); },
                [&](int v, float f) { fminusSynth.noteOn(v, f); }, [&](int v) { fminusSynth.noteOff(v); },
                [](float) {});
        }
        // The frozen rate reaches the code: compiled at 44.1 kHz, the synth plays as C++ does at 44.1 kHz.
        constexpr float kOtherRate = 44100.0f;
        fminus::Program::Options otherOptions = options;
        otherOptions.parameters["Synth::rate"] = fminus::Program::Binding::frozen(static_cast<double>(kOtherRate));
        std::unique_ptr<fminus::Program> otherProgram = fminus::Program::compile(source, errors, otherOptions);
        bool otherRate = otherProgram != nullptr && otherProgram->module("Synth") != nullptr;
        if (otherRate)
        {
            FminusSynth otherSynth(*otherProgram, *otherProgram->module("Synth"), errors);
            otherRate = otherSynth.found();
            if (otherRate)
            {
                std::vector<float> fminusAt(samples);
                std::vector<float> cppAt(samples);
                otherSynth.reset();
                play(
                    events, block, fminusAt, [&](float* o, int n) { otherSynth.process(o, n); },
                    [&](int v, float f) { otherSynth.noteOn(v, f); }, [&](int v) { otherSynth.noteOff(v); },
                    [&](float hertz) { otherSynth.setCutoff(hertz); });
                Synth synth(*program);
                synth.rate.value = kOtherRate;
                play(
                    events, block, cppAt, [&](float* o, int n) { synth.process(o, n); },
                    [&](int v, float f) { synth.noteOn(v, f); }, [&](int v) { synth.noteOff(v); },
                    [&](float hertz) { synth.cutoff.value = hertz; });
                otherRate = fminusAt == cppAt && fminusAt != fminusOutput;
            }
        }
        const std::size_t turn = samples * 3 / 4;
        const bool turned = std::equal(untouched.begin(), untouched.begin() + static_cast<std::ptrdiff_t>(turn),
                                       fminusOutput.begin())
                         && !std::equal(untouched.begin() + static_cast<std::ptrdiff_t>(turn), untouched.end(),
                                        fminusOutput.begin() + static_cast<std::ptrdiff_t>(turn));
        std::printf("%s", std::format("example-voices: a Synth of {} bytes, aligned to {}, laid out as the C++ "
                                      "structs: {}; process nonblocking: {}; FMinus and C++ the same over {} samples: "
                                      "{}; in blocks of 64 and 61: {}; the cutoff's turn heard from its sample on: "
                                      "{}; frozen at {} Hz, C++ at {} Hz: {}\n",
                                      module->size, module->alignment, layouts ? "yes" : "no",
                                      nonBlocking ? "yes" : "no", samples, same ? "yes" : "no",
                                      sameInBlocks ? "yes" : "no", turned ? "yes" : "no", kOtherRate, kOtherRate,
                                      otherRate ? "yes" : "no")
                              .c_str());
        return layouts && nonBlocking && same && sameInBlocks && sounds && turned && otherRate ? 0 : 1;
    }

    const double fminusTime = nanosecondsPerSample([&] { runFminus(block, fminusOutput); }, samples, repeats);
    const double cppTime = nanosecondsPerSample(runCpp, samples, repeats);
    std::printf("%s",
                std::format("eight voices (a sawtooth, a one-pole low-pass, an envelope each) at {} Hz, {} samples, "
                            "{}\n"
                            "  FMinus (JIT, -O{}) {:8.3f} ns/sample   modules, the Synth in the host's memory, its "
                            "knobs read once a call\n"
                            "  C++ (clang)       {:8.3f} ns/sample   the same, as structs\n"
                            "  FMinus / C++      {:8.2f}x\n"
                            "  the outputs are {}\n",
                            rate, samples,
                            block == 0 ? std::string("1 call from one event to the next")
                                       : std::format("calls of at most {} samples", block),
                            level, fminusTime, cppTime, fminusTime / cppTime,
                            fminusOutput == cppOutput ? "bit-identical" : "different")
                    .c_str());
    return 0;
}
