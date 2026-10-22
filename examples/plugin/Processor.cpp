// examples/plugin/Processor.cpp: FMinus Filter's processor (Processor.h).
//
// The hand-over. prepareToPlay writes the program, its function pointer, the instances and the cutoff's buffer, and
// processBlock reads them, on the audio thread, through plain members. That is safe because the two never run at once,
// and whatever keeps them apart also orders prepareToPlay's writes before processBlock's reads:
//   - the host's contract, where JUCE prepares a plugin that is not processing: an AU renders only once initialised,
//     and JUCE prepares it in Initialize; a VST3 processes only while active, and JUCE prepares it in setActive(true)
//     (setupProcessing does not prepare);
//   - a lock, where JUCE prepares a plugin that may be processing: when a host switches an AU's offline rendering, JUCE
//     prepares it again holding the processor's callback lock, which the AU's render holds around processBlock too;
//     the Standalone's AudioProcessorPlayer holds its own lock around prepareToPlay and its audio callback; and in FL
//     Studio, JUCE's VST3 wrapper holds a mutex around setActive and process.
// The program is destroyed and compiled again only in prepareToPlay, so no thread can be running its code then. A
// plugin that compiled while the audio plays (to freeze a setting, say) would hand the new program over through an
// atomic pointer instead, and destroy the old one only once the audio thread has let go of it (README.md).
//
// reset is the exception: the AU's Reset (which hosts call when they stop or locate, and JUCE when the AU's bypass is
// switched) and the VST3's setProcessing(false) call it without a lock, possibly while processBlock runs. So it only
// sets an atomic flag, and processBlock, at its next block, starts the filter again from rest itself.
#include "Processor.h"

#include "FilterSource.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

namespace fminus_filter
{
    namespace
    {
        // The cutoff's control: 20 Hz to 20 kHz, logarithmic (each octave the same distance), 1 kHz by default, as
        // filter.fm's fminus::range and fminus::default say.
        juce::NormalisableRange<float> cutoffRange()
        {
            return {20.0f, 20000.0f,
                    [](float start, float end, float proportion)
                    { return start * std::pow(end / start, proportion); },
                    [](float start, float end, float value)
                    { return std::log(value / start) / std::log(end / start); },
                    [](float start, float end, float value) { return std::clamp(value, start, end); }};
        }

        juce::AudioProcessorValueTreeState::ParameterLayout parameterLayout()
        {
            juce::AudioProcessorValueTreeState::ParameterLayout layout;
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{"cutoff", 1}, "Cutoff", cutoffRange(), 1000.0f,
                juce::AudioParameterFloatAttributes().withLabel("Hz").withStringFromValueFunction(
                    [](float value, int) { return juce::String(juce::roundToInt(value)); })));
            layout.add(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{"resonance", 1}, "Resonance", juce::NormalisableRange<float>(0.0f, 1.0f), 0.3f,
                juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                    [](float value, int) { return juce::String(value, 2); })));
            layout.add(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{"mode", 1}, "Mode", juce::StringArray{"Low-pass", "Band-pass", "High-pass"}, 0));
            return layout;
        }
    } // namespace

    Processor::Processor(std::string source)
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true))
        , source_(std::move(source))
        , state_(*this, nullptr, "FMinusFilter", parameterLayout())
        , cutoffValue_(state_.getRawParameterValue("cutoff"))
        , resonanceValue_(state_.getRawParameterValue("resonance"))
        , modeValue_(state_.getRawParameterValue("mode"))
    {
    }

    Processor::~Processor() = default;

    std::string Processor::embeddedSource()
    {
        return {FilterSource::filter_fm, static_cast<std::size_t>(FilterSource::filter_fmSize)};
    }

    // ---- compiling, off the audio thread ----------------------------------------------------------------------------

    void Processor::refuse(std::string reason)
    {
        errors_ = std::move(reason);
        process_ = nullptr;
        filter_ = nullptr;
        cutoff_ = resonance_ = mode_ = nullptr;
        program_.reset();
    }

    void Processor::compile(double sampleRate)
    {
        refuse({});   // the old program goes first: nothing runs it during prepareToPlay
        compiledRate_ = sampleRate;
        ++compilations_;

        // The bindings: the cutoff a stream, a value a sample; the sample rate frozen, a constant the optimiser folds
        // (so the next rate compiles again); resonance and mode, which the options don't name, knobs.
        fminus::Program::Options options;
        options.fileName = "filter.fm";
        options.parameters["Filter::cutoff"] = fminus::Program::Binding::stream();
        options.parameters["Filter::sampleRate"] = fminus::Program::Binding::frozen(sampleRate);
        std::string errors;
        std::unique_ptr<fminus::Program> program = fminus::Program::compile(source_, errors, options);
        if (!program)
            return refuse(errors);   // FMinus's errors, as clang words them

        // What this processor needs of the program: the module, its parameters with these types, and its process,
        // marked [[clang::nonblocking]], which FMinus has then checked cannot reach malloc, calloc, free or an
        // external function that is not nonblocking itself. Anything else stays off the audio thread.
        const fminus::Program::Module* filter = program->module("Filter");
        if (filter == nullptr)
            return refuse("filter.fm: no module Filter");
        if (std::find(filter->methods.begin(), filter->methods.end(), "Filter::process") == filter->methods.end())
            return refuse("filter.fm: Filter has no method process");
        if (!program->nonBlocking("Filter::process"))
            return refuse("filter.fm: Filter::process is not marked [[clang::nonblocking]], so nothing has checked "
                          "that it cannot reach the heap or a function that blocks: refused for the audio thread");
        const fminus::Program::Parameter* cutoff = program->parameter("Filter::cutoff");
        const fminus::Program::Parameter* resonance = program->parameter("Filter::resonance");
        const fminus::Program::Parameter* mode = program->parameter("Filter::mode");
        if (cutoff == nullptr || resonance == nullptr || mode == nullptr || resonance->type != "float"
            || mode->type != "int")
            return refuse("filter.fm: Filter needs the parameters float cutoff, float resonance and int mode");
        // The first look-up compiles the function to machine code, here, off the audio thread.
        auto* process = program->function<void(void*, float*, int)>("Filter::process", errors);
        if (process == nullptr)
            return refuse(errors);

        program_ = std::move(program);
        filter_ = filter;
        cutoff_ = cutoff;
        resonance_ = resonance;
        mode_ = mode;
        process_ = process;
        errors_.clear();
    }

    // As fminus::Program::Module describes a new instance: zeroed, then each parameter at its fminus::default. FMinus
    // runs no constructor: that is all an instance needs (and processBlock sets the knobs and the stream before each
    // call anyway). The filter's states and its mix start at rest; the mix at the mode's, in the first sample.
    void Processor::startFromRest(void* instance) const noexcept
    {
        std::memset(instance, 0, filter_->size);
        fminus::Program::setDefaults(instance, *filter_);
    }

    void Processor::prepareToPlay(double sampleRate, int samplesPerBlock)
    {
        bool fresh = false;   // a new program or new instances: the filter and the smoother start from rest
        if (compilations_ == 0 || !juce::exactlyEqual(sampleRate, compiledRate_))
        {
            compile(sampleRate);
            fresh = true;
        }

        // One instance of the module a channel, from the start: `size` bytes, aligned to `alignment` (at least a
        // pointer's, and the size a multiple of it, as macOS's aligned_alloc wants), as new. The instances, and the
        // filter's state in them, stay when the program and the channels do: JUCE's AU prepares again, mid-stream,
        // when a host switches its offline rendering, and the filter then goes on without a click. A stop clears them
        // (releaseResources, reset).
        const auto channels = static_cast<std::size_t>(std::max(getTotalNumOutputChannels(), 0));
        if (process_ == nullptr)
            instances_.clear();
        else if (fresh || instances_.size() != channels)
        {
            fresh = true;
            instances_.clear();
            const std::size_t alignment = std::max(filter_->alignment, alignof(std::max_align_t));
            const std::size_t size = (filter_->size + alignment - 1) / alignment * alignment;
            for (std::size_t channel = 0; channel < channels; ++channel)
            {
                Instance instance(std::aligned_alloc(alignment, size));
                if (!instance)
                {
                    instances_.clear();
                    refuse("FMinus Filter: out of memory for the filter's instances");
                    break;
                }
                startFromRest(instance.get());
                instances_.push_back(std::move(instance));
            }
        }

        // FMinus's errors, logged here, once, never from the audio thread. A program that failed fails again at the
        // next rate: it isn't logged twice.
        if (process_ == nullptr && !errorsLogged_)
        {
            juce::Logger::writeToLog("FMinus Filter: filter.fm is not running, and the audio passes through:\n"
                                     + juce::String(errors_));
            errorsLogged_ = true;
        }

        // The cutoff's buffer, for a part of a block: as long as the host's largest block, which a buggy host may
        // still exceed (processBlock then runs the block in parts). The smoother, at the cutoff, unless it is gliding
        // through a filter that goes on.
        const auto part = static_cast<std::size_t>(std::max(samplesPerBlock, 1));
        if (cutoffs_.size() != part)
            cutoffs_.assign(part, 0.0f);
        if (fresh)
        {
            cutoffSmoother_.reset(sampleRate, kCutoffRampSeconds);
            cutoffSmoother_.setCurrentAndTargetValue(cutoffValue_->load(std::memory_order_relaxed));
        }
    }

    // The audio has stopped: the next prepareToPlay and processBlock start from rest (the memory stays, for them).
    void Processor::releaseResources()
    {
        clear_.store(true, std::memory_order_relaxed);
    }

    void Processor::reset()
    {
        clear_.store(true, std::memory_order_relaxed);
    }

    bool Processor::isBusesLayoutSupported(const BusesLayout& layouts) const
    {
        const juce::AudioChannelSet& output = layouts.getMainOutputChannelSet();
        return (output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo())
               && layouts.getMainInputChannelSet() == output;
    }

    // ---- the audio thread -------------------------------------------------------------------------------------------

    void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
    {
        juce::ScopedNoDenormals noDenormals;
        const int samples = buffer.getNumSamples();
        for (int channel = getTotalNumInputChannels(); channel < getTotalNumOutputChannels(); ++channel)
            buffer.clear(channel, 0, samples);
        if (process_ == nullptr)
            return;   // no program: the audio passes through as it came

        // After a reset, from rest: each instance as new, and the cutoff at its value, without a glide from before.
        if (clear_.exchange(false, std::memory_order_relaxed))
        {
            for (const Instance& instance : instances_)
                startFromRest(instance.get());
            cutoffSmoother_.setCurrentAndTargetValue(cutoffValue_->load(std::memory_order_relaxed));
        }

        // The knobs, once a block, from the parameters' values: setValue is a relaxed atomic store into the instance,
        // which Filter::process reads when it starts. The cutoff's target, which the smoother glides to.
        const double resonance = static_cast<double>(resonanceValue_->load(std::memory_order_relaxed));
        const double mode = static_cast<double>(modeValue_->load(std::memory_order_relaxed));
        const int channels = std::min(buffer.getNumChannels(), static_cast<int>(instances_.size()));
        for (int channel = 0; channel < channels; ++channel)
        {
            void* instance = instances_[static_cast<std::size_t>(channel)].get();
            fminus::Program::setValue(instance, *resonance_, resonance);
            fminus::Program::setValue(instance, *mode_, mode);
        }
        cutoffSmoother_.setTargetValue(cutoffValue_->load(std::memory_order_relaxed));

        // The block, in parts no longer than the cutoff's buffer: the cutoff a sample from the smoother, which every
        // channel's instance reads as its stream (setStream before each call), then the filter on each channel's
        // samples, in place.
        const int part = static_cast<int>(cutoffs_.size());
        for (int start = 0; start < samples; start += part)
        {
            const int n = std::min(part, samples - start);
            for (int i = 0; i < n; ++i)
                cutoffs_[static_cast<std::size_t>(i)] = cutoffSmoother_.getNextValue();
            for (int channel = 0; channel < channels; ++channel)
            {
                void* instance = instances_[static_cast<std::size_t>(channel)].get();
                fminus::Program::setStream(instance, *cutoff_, cutoffs_.data());
                process_(instance, buffer.getWritePointer(channel, start), n);
            }
        }
    }

    // ---- the rest of an AudioProcessor ------------------------------------------------------------------------------

    juce::AudioProcessorEditor* Processor::createEditor()
    {
        return new juce::GenericAudioProcessorEditor(*this);
    }

    bool Processor::hasEditor() const
    {
        return true;
    }

    const juce::String Processor::getName() const
    {
        return "FMinus Filter";
    }

    bool Processor::acceptsMidi() const
    {
        return false;
    }

    bool Processor::producesMidi() const
    {
        return false;
    }

    bool Processor::isMidiEffect() const
    {
        return false;
    }

    // How long the filter rings once its input stops, at its longest: the cutoff at 20 Hz, resonance 1 (Q = 25). Its
    // poles' envelope decays as exp(-w t / (2 Q)), so it is 60 dB down (by ln 1000 = 6.91) after 6.91 * 2 Q / w =
    // 6.91 * 50 / (2 pi 20 Hz) = 2.75 s; fminus-plugin-check measures it. Hosts that stop processing a silent input, or
    // stop rendering at a region's end plus the tail, would cut the ring short with less. A constant, the worst case: a
    // tail that followed the parameters would need updateHostDisplay, which stays off the audio thread.
    double Processor::getTailLengthSeconds() const
    {
        return kTailSeconds;
    }

    int Processor::getNumPrograms()
    {
        return 1;
    }

    int Processor::getCurrentProgram()
    {
        return 0;
    }

    void Processor::setCurrentProgram(int)
    {
    }

    const juce::String Processor::getProgramName(int)
    {
        return {};
    }

    void Processor::changeProgramName(int, const juce::String&)
    {
    }

    void Processor::getStateInformation(juce::MemoryBlock& destination)
    {
        if (const std::unique_ptr<juce::XmlElement> xml = state_.copyState().createXml())
            copyXmlToBinary(*xml, destination);
    }

    void Processor::setStateInformation(const void* data, int size)
    {
        if (const std::unique_ptr<juce::XmlElement> xml = getXmlFromBinary(data, size))
            if (xml->hasTagName(state_.state.getType()))
                state_.replaceState(juce::ValueTree::fromXml(*xml));
    }
} // namespace fminus_filter

// The plugin's entry point, which JUCE's format wrappers call.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new fminus_filter::Processor();
}
