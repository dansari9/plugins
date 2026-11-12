#pragma once

#include "Parameters.h"
#include "analysis/AnalysisWorker.h"
#include "dsp/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <mutex>

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
        double boostNote = 36.0;    // the note each bank plays, knob or MIDI (key lock applied), without Fine or glide
        double cutNote = 36.0;
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

    // The factory presets are the host's programs. setCurrentProgram() can come
    // from any thread, the audio thread included: it only sets the parameters
    // (no gestures, no allocation, no state tree). Selecting the current
    // program again reloads it, as a host's preset menu means. Restoring a
    // session goes through setStateInformation() instead (the AU and VST3
    // wrappers never select a program for it), so it keeps the edits.
    int getNumPrograms() override { return numPresets(); }
    int getCurrentProgram() override { return currentPreset(); }
    void setCurrentProgram(int index) override { applyPreset(index, false); }
    const juce::String getProgramName(int index) override { return presetName(index); }
    void changeProgramName(int, const juce::String&) override {}

    /**
        The state is the parameters (plain values, by ID), the preset, both A/B
        slots and the active one, and the editor's size and page, under an
        explicit schema version. Restoring reads states of every 0.4.x
        release: missing values take their defaults, out-of-range ones are
        clamped, unknown ones are ignored, and a damaged state changes nothing.
        Any thread but the audio thread.
    */
    static constexpr int kStateSchema = 2; // 1: 0.4.0-0.4.2, without a version, A/B or page
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }

    // ---- Editor interface ---------------------------------------------------------------
    // All an editor needs besides the parameters in getApvts(). Call from the
    // message thread. Nothing here blocks, locks or allocates on the audio
    // thread: the audio thread publishes through atomics and a FIFO.

    /**
        The newest complete frame the audio thread published, with the live
        (smoothed) band gains. Without audio for a while (none yet, or the
        host stopped) it is a preview computed from the knobs. One consumer.
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

    /**
        Sets a bank's knob note (boost or cut) as a click on the plug-in's own
        interface does, and gives the bank back to the knob even when the value
        is already there: an explicit choice beats a held MIDI note.
    */
    void setKnobNote(rootnote::NoteControl::Target bank, double note);

    /** Puts the cut on the pitch of the detected hit and switches it on. False without a reading. */
    bool matchCutToHit();

    /** Moves the boost note to the scale note nearest the detected hit. False without a reading. */
    bool tuneBoostToKey();

    /** Factory presets: "Default" and the eight of the plan. */
    static int numPresets() noexcept;
    static juce::String presetName(int index);

    /** The preset loaded last, kept in the state. Changing parameters afterwards does not clear it. */
    int currentPreset() const noexcept { return presetIndex.load(std::memory_order_relaxed); }

    /**
        Sets every parameter to the preset's value or its default, except the
        ones that belong to the song: notes, fine tune, key, scale, key lock,
        A4 and bypass.
    */
    void loadPreset(int index);

    /**
        Two settings to compare, A (0) and B (1). Switching stores the current
        parameters and preset in the slot being left and recalls the other
        one; B starts as a copy of A. Bypass is shared. Saved in the state.
    */
    int abSlot();
    void selectAbSlot(int slot);

    /** Editor size kept in the plug-in state; {0, 0} until an editor has stored one. */
    juce::Point<int> storedEditorSize() const noexcept;
    void storeEditorSize(int width, int height) noexcept;

    /** Counts completed setStateInformation() calls: an open editor takes the restored page and size when it changes. */
    uint32_t restoreCount() const noexcept { return restores.load(std::memory_order_acquire); }

    /** Editor page kept in the plug-in state. */
    int storedEditorPage() const noexcept { return editorPage.load(std::memory_order_relaxed); }
    void storeEditorPage(int page) noexcept { editorPage.store(page, std::memory_order_relaxed); }

private:
    void processSpan(float* const* channels, int numChannels, int from, int to) noexcept;
    void publishDisplay(const juce::AudioBuffer<float>& buffer, int numChannels) noexcept;
    void setParameterValue(const char* id, double value);
    void applyPreset(int index, bool asGesture);

    struct AbSlot
    {
        std::vector<float> values; // normalised, in parameter order; empty until the slot has been left once
        int preset = 0;
    };
    // Saving, restoring and switching A/B are each one transaction under this
    // lock, parameter changes included, so none sees half of another (a host
    // may save or restore on its own thread while the editor switches).
    // Recursive: a parameter listener may save on the same thread. Never taken
    // on the audio thread.
    std::recursive_mutex stateMutex;
    AbSlot abSlots[2];
    int currentAbSlot = 0;

    juce::AudioProcessorValueTreeState apvts;
    rootnote::ParameterReader parameters { apvts };
    juce::AudioParameterBool* bypassParameter = nullptr;
    // Per preset, the normalised value of every parameter in parameter order; < 0 = the preset keeps it.
    std::vector<std::vector<float>> presetValues;

    // Kept out of apvts.state, which replaceState() swaps on whatever thread
    // the host restores from: the editor reads these every frame.
    std::atomic<int> presetIndex { 0 };
    std::atomic<int> editorWidth { 0 }, editorHeight { 0 }, editorPage { 0 };

    rootnote::Engine engine;
    rootnote::AnalysisWorker analysis;
    rootnote::SpscFifo spectrumFifo { static_cast<size_t>(1) << 15 };

    std::atomic<bool> stateRestored { false }; // set by setStateInformation, consumed by the audio thread
    std::atomic<uint32_t> restores { 0 };
    std::atomic<unsigned> knobTakeovers { 0 };  // NoteControl::boostChanged / cutChanged, consumed by the audio thread
    std::atomic<int> displayUsers { 0 };
    // Triple buffer: the audio thread fills its slot and swaps it in as the
    // newest; the editor swaps the newest out. Each side owns one slot.
    static constexpr int kFresh = 4; // flag in newestSlot: not taken yet
    static constexpr juce::uint32 kDisplayHoldMs = 250;
    DisplayState displaySlots[3];
    int audioSlot = 0;                       // audio thread only
    int editorSlot = 1;                      // message thread only
    std::atomic<int> newestSlot { 2 };
    juce::uint32 lastFrameMs = 0;            // message thread only; 0 = no frame yet
    std::atomic<double> displaySampleRate { 48000.0 };
    std::atomic<float> outputPeak[2] { { 0.0f }, { 0.0f } };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RootnoteProcessor)
};
