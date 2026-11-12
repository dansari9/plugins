#pragma once

#include "Parameters.h"
#include "analysis/AnalysisWorker.h"
#include "dsp/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

class RootnoteProcessor : public juce::AudioProcessor
{
public:
    /** What the audio thread is doing right now, for the editor. */
    struct DisplayState
    {
        double boostHz = 0.0;       // follows MIDI and glides
        double cutHz = 0.0;
        bool boostFromMidi = false; // the note comes from MIDI, not from the knob
        bool cutFromMidi = false;
        uint32_t midiNoteOns = 0;   // counts up; a change means MIDI activity
        float limiterGain = 1.0f;   // 1 = not limiting
    };

    RootnoteProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParameter; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }

    // ---- Editor interface. Everything below is safe to call from the message thread. ----

    DisplayState displayState() const noexcept;
    rootnote::TunerReading tunerReading() const noexcept { return analysis.reading(); }

    /**
        Counted. While at least one editor is open the tuner runs and the
        output is fed to the spectrum FIFO; otherwise neither costs anything.
    */
    void setDisplayActive(bool active) noexcept;

    /** Mono output samples for the spectrum display. One reader only. */
    size_t readSpectrumSamples(float* dest, size_t maxCount) noexcept { return spectrumFifo.read(dest, maxCount); }

    /** Output peak of a channel (0 or 1) since the last call. */
    float takeOutputPeak(int channel) noexcept { return outputPeak[channel].exchange(0.0f, std::memory_order_relaxed); }

    /** Puts the cut on the pitch of the detected hit and switches it on. False without a reading. */
    bool matchCutToHit();

    /** Moves the boost note to the scale note nearest the detected hit. False without a reading. */
    bool tuneBoostToKey();

    /** Editor size kept in the plug-in state; {0, 0} until an editor has stored one. */
    juce::Point<int> storedEditorSize() const;
    void storeEditorSize(int width, int height);

private:
    void processSpan(float* const* channels, int numChannels, int from, int to) noexcept;
    void publishDisplay(const juce::AudioBuffer<float>& buffer, int numChannels) noexcept;
    void setParameterValue(const char* id, double value);

    juce::AudioProcessorValueTreeState apvts;
    rootnote::ParameterReader parameters { apvts };
    juce::AudioParameterBool* bypassParameter = nullptr;

    rootnote::Engine engine;
    rootnote::AnalysisWorker analysis;
    rootnote::SpscFifo spectrumFifo { static_cast<size_t>(1) << 15 };

    std::atomic<int> displayUsers { 0 };
    std::atomic<double> boostHz { 0.0 };
    std::atomic<double> cutHz { 0.0 };
    std::atomic<bool> boostFromMidi { false };
    std::atomic<bool> cutFromMidi { false };
    std::atomic<uint32_t> midiNoteOns { 0 };
    std::atomic<float> limiterGain { 1.0f };
    std::atomic<float> outputPeak[2] { { 0.0f }, { 0.0f } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RootnoteProcessor)
};
