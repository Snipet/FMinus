// examples/plugin/Processor.h: FMinus Filter's processor. Its DSP is filter.fm, an FMinus program embedded in the
// plugin, which fminus::Program compiles in prepareToPlay for the CPU the plugin runs on (README.md, "How FMinus is
// used").
//
//   prepareToPlay   compiles filter.fm, off the audio thread, with the cutoff a stream, resonance and mode knobs, and
//                   the sample rate frozen at the host's (compiled again only when the rate changes); refuses it unless
//                   Filter::process is [[clang::nonblocking]]; makes one instance of the module a channel (kept, with
//                   the filter's state, while the program and the channels stay the same), and the buffer the cutoff's
//                   samples go in. Everything processBlock touches is allocated here.
//   processBlock    sets the knobs from the parameters, once a block; fills the cutoff's buffer from a smoother, a
//                   value a sample, so a moved knob glides; runs Filter::process on each channel, in place. It neither
//                   allocates, locks nor logs. With no program (it did not compile, or was refused), the audio passes
//                   through as it came.
//   reset           (and releaseResources) ask processBlock to start the filter again from rest, before its next block:
//                   a host calls reset when it stops or moves the playhead, from any thread.
//
// The parameters are the program's, by the same names, ranges and defaults (fminus-plugin-check compares them), and
// the state is the AudioProcessorValueTreeState's.
#pragma once

#include "fminus/FMinus.h"

#include <atomic>
#include <cstdlib>
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>
#include <vector>

namespace fminus_filter
{
    class Processor final : public juce::AudioProcessor
    {
    public:
        // `source` is the FMinus program the processor runs: filter.fm, embedded, unless the check gives another (one
        // that does not compile, or whose process is not nonblocking).
        explicit Processor(std::string source = embeddedSource());
        ~Processor() override;

        // filter.fm, as the plugin embeds it (juce_add_binary_data).
        static std::string embeddedSource();

        // The cutoff's glide: how long the smoother takes to reach a new value.
        static constexpr double kCutoffRampSeconds = 0.02;

        // The longest the filter rings once its input stops, to 60 dB below its peak (getTailLengthSeconds, which
        // Processor.cpp derives).
        static constexpr double kTailSeconds = 3.0;

        void prepareToPlay(double sampleRate, int samplesPerBlock) override;
        void releaseResources() override;
        void reset() override;
        bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
        using AudioProcessor::processBlock;   // the double-precision one: JUCE's default (this plugin is float only)

        juce::AudioProcessorEditor* createEditor() override;
        bool hasEditor() const override;
        const juce::String getName() const override;
        bool acceptsMidi() const override;
        bool producesMidi() const override;
        bool isMidiEffect() const override;
        double getTailLengthSeconds() const override;
        int getNumPrograms() override;
        int getCurrentProgram() override;
        void setCurrentProgram(int index) override;
        const juce::String getProgramName(int index) override;
        void changeProgramName(int index, const juce::String& name) override;
        void getStateInformation(juce::MemoryBlock& destination) override;
        void setStateInformation(const void* data, int size) override;

        // The parameters: "cutoff" (Hz), "resonance" (0 to 1) and "mode" (a choice: low-, band-, high-pass).
        juce::AudioProcessorValueTreeState& state() noexcept
        {
            return state_;
        }

        // For the check, while the audio thread is stopped: whether a program runs, the rate it was last compiled
        // for, how many times prepareToPlay has compiled, and FMinus's errors (or why the program was refused).
        bool running() const noexcept
        {
            return process_ != nullptr;
        }

        double compiledRate() const noexcept
        {
            return compiledRate_;
        }

        int compilations() const noexcept
        {
            return compilations_;
        }

        const std::string& errors() const noexcept
        {
            return errors_;
        }

    private:
        struct Free
        {
            void operator()(void* memory) const noexcept
            {
                std::free(memory);
            }
        };

        using Instance = std::unique_ptr<void, Free>;

        void compile(double sampleRate);
        void refuse(std::string reason);
        void startFromRest(void* instance) const noexcept;   // an instance as new: zeroed, at its defaults

        const std::string source_;
        juce::AudioProcessorValueTreeState state_;
        // The parameters' values, as the APVTS keeps them (in their own units: Hz, 0 to 1, the choice's index).
        std::atomic<float>* cutoffValue_ = nullptr;
        std::atomic<float>* resonanceValue_ = nullptr;
        std::atomic<float>* modeValue_ = nullptr;

        // Written by prepareToPlay, read by processBlock, which a host never runs at once (Processor.cpp, "The
        // hand-over").
        std::unique_ptr<fminus::Program> program_;
        double compiledRate_ = 0.0;
        int compilations_ = 0;
        std::string errors_;
        bool errorsLogged_ = false;
        const fminus::Program::Module* filter_ = nullptr;
        const fminus::Program::Parameter* cutoff_ = nullptr;
        const fminus::Program::Parameter* resonance_ = nullptr;
        const fminus::Program::Parameter* mode_ = nullptr;
        void (*process_)(void* instance, float* samples, int n) = nullptr;   // Filter::process
        std::vector<Instance> instances_;                                    // a channel each
        std::vector<float> cutoffs_;                                         // a part of a block's cutoff, in Hz
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> cutoffSmoother_;

        // Set by reset and releaseResources, from any thread; taken by processBlock, which then starts the filter
        // again from rest. A flag and no data: relaxed is enough.
        std::atomic<bool> clear_{false};
        static_assert(std::atomic<bool>::is_always_lock_free);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
    };
} // namespace fminus_filter
