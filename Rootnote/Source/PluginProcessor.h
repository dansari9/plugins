#pragma once

#include "Parameters.h"
#include "analysis/AnalysisWorker.h"
#include "dsp/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

class RootnoteProcessor : public juce::AudioProcessor
{
public:
    /**
        What the plug-in is doing right now, for an editor to draw. The notes
        follow MIDI and glide. Before the first audio block they are the knob
        notes.
    */
    struct DisplayState
    {
        double sampleRate = 48000.0;
        double boostHz = 0.0;
        double cutHz = 0.0;
        bool boostFromMidi = false; // the note comes from MIDI, not from the knob
        bool cutFromMidi = false;
        uint32_t midiNoteOns = 0;   // counts up; a change means MIDI activity
        float limiterGain = 1.0f;   // linear; 1 = not limiting

        // Every band with a gain, at its live frequency. Cut bands are empty while the cut is off.
        rootnote::BandView boostBands[rootnote::kMaxSlots];
        rootnote::BandView cutBands[rootnote::kMaxSlots];
        int numBoostBands = 0;
        int numCutBands = 0;
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

    // ---- Editor interface ---------------------------------------------------------------
    // All an editor needs besides the parameters in getApvts(). Call from the
    // message thread. Nothing here blocks, locks or allocates on the audio
    // thread: the audio thread publishes through atomics and a FIFO.

    /**
        Drains complete audio snapshots, returns the newest, or a knob preview
        before audio arrives. Actual band gains include smoothing. One message-
        thread consumer; retains the last complete frame if none is available.
    */
    DisplayState displayState() noexcept;

    /** The drum tuner's reading of the input. Only updated while a display is active. */
    rootnote::TunerReading tunerReading() const { return analysis.reading(); }

    /**
        Counted. While at least one editor is open the tuner runs and the
        output peaks and the spectrum FIFO are fed; otherwise none of them
        costs anything. Call with true when an editor opens and false when it closes.
    */
    void setDisplayActive(bool active) noexcept;

    /** Mono output samples for a spectrum display, oldest first. One reader only. */
    size_t readSpectrumSamples(float* dest, size_t maxCount) noexcept { return spectrumFifo.read(dest, maxCount); }

    /** Output peak of a channel (0 or 1) since the last call, linear. */
    float takeOutputPeak(int channel) noexcept { return outputPeak[channel].exchange(0.0f, std::memory_order_relaxed); }

    /** Puts the cut on the pitch of the detected hit and switches it on. False without a reading. */
    bool matchCutToHit();

    /** Moves the boost note to the scale note nearest the detected hit. False without a reading. */
    bool tuneBoostToKey();

    /** Factory presets: "Default" and the eight of the plan. */
    static int numPresets() noexcept;
    static juce::String presetName(int index);

    /** The preset loaded last, kept in the state. Changing parameters afterwards does not clear it. */
    int currentPreset() const;

    /**
        Sets every parameter to the preset's value or its default, except the
        ones that belong to the song: notes, fine tune, key, scale, key lock,
        A4 and bypass.
    */
    void loadPreset(int index);

    /**
        Two settings to compare, A (0) and B (1). Switching stores the current
        parameters and preset in the slot being left and recalls the other
        one; B starts as a copy of A. Bypass is shared. Not saved in the state.
    */
    int abSlot();
    void selectAbSlot(int slot);

    /** Editor size kept in the plug-in state; {0, 0} until an editor has stored one. */
    juce::Point<int> storedEditorSize() const;
    void storeEditorSize(int width, int height);

private:
    void processSpan(float* const* channels, int numChannels, int from, int to) noexcept;
    void publishDisplay(const juce::AudioBuffer<float>& buffer, int numChannels) noexcept;
    void setParameterValue(const char* id, double value);

    void resetAbIfRestored(); // message thread only

    struct AbSlot
    {
        std::vector<float> values; // normalised, in parameter order; empty until the slot has been left once
        int preset = 0;
    };
    AbSlot abSlots[2];
    int currentAbSlot = 0;

    juce::AudioProcessorValueTreeState apvts;
    rootnote::ParameterReader parameters { apvts };
    juce::AudioParameterBool* bypassParameter = nullptr;

    rootnote::Engine engine;
    rootnote::AnalysisWorker analysis;
    rootnote::SpscFifo spectrumFifo { static_cast<size_t>(1) << 15 };

    std::atomic<bool> stateRestored { false }; // set by setStateInformation, consumed by the audio thread
    std::atomic<int> displayUsers { 0 };
    // Bounded SPSC queue: audio writes whole frames; the message thread reads.
    // A full queue drops a new frame. Neither side can access the other's slot.
    static constexpr size_t kDisplayFrames = 4;
    DisplayState displayFrames[kDisplayFrames];
    std::atomic<size_t> displayWrite { 0 }, displayRead { 0 };
    DisplayState lastDisplay;
    bool haveDisplay = false; // message thread only
    std::atomic<double> displaySampleRate { 48000.0 };
    std::atomic<bool> resetAbPending { false };
    std::atomic<float> outputPeak[2] { { 0.0f }, { 0.0f } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RootnoteProcessor)
};
