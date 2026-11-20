#pragma once
#include "Parameters.h"
#include "chain/ReorderCrossfader.h"
#include "dsp/common/Delay.h"
#include "dsp/common/LoudnessMeter.h"
#include "dsp/common/Oversampler.h"
#include "dsp/common/Smoother.h"
#include "dsp/master/Master.h"
#include "dsp/modules/Push.h"
#include "dsp/modules/SubHiss.h"
#include "presets/StateIO.h"
#include <array>
#include <unordered_map>

// snap: drum multi-FX by daveydidit. Parameter table in Parameters.cpp, session state in presets/StateIO.
class SnapProcessor : public juce::AudioProcessor,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::ValueTree::Listener,
                      private juce::AsyncUpdater
{
public:
    static constexpr int historySize = 512; // ~2.5 s of display history

    SnapProcessor();
    ~SnapProcessor() override;
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return snap::spaceConstants::maxTailSeconds(); }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter("bypass"); }
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // Loudness Match: BS.1770 short-term loudness of the plug-in input and of the signal right before Output.
    float inputLufs() const { return inputMeter.lufs(); }
    float preOutputLufs() const { return master.preOutputLufs(); }
    void matchLoudness(); // message thread: sets Output so the output is as loud as the input
    bool clipping() const { return master.clipping(); }

    // Session controls, message thread. Order, locks and snapshots are session state (saved with the host session), not
    // parameters. Order changes, preset loads and snapshot recalls are undo steps.
    snap::ChainOrder order() const;
    void setOrder(const snap::ChainOrder&);
    bool locked(int group) const { return locks[(size_t) group]; } // group: snap::Module index or snap::masterLock
    void setLocked(int group, bool on) { locks[(size_t) group] = on; }
    void storeSnapshot(int slot);
    bool recallSnapshot(int slot); // false if the slot is empty
    bool hasSnapshot(int slot) const { return snapshots[(size_t) slot].has_value(); }
    bool loadPreset(const void* data, int size); // a state blob; keeps locks, locked groups and the session toggles
    void beginUndoStep() { commit(), undoManager.beginNewTransaction(); } // the editor calls this at the start of each gesture
    bool undo();
    bool redo();
    bool reordering() const { return middle.transitioning(); } // audio thread state, for tests and the display
    int reorderFadeSamples() const { return middle.fadeSamples(); }

    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;

    // Display feed: written by the audio thread, read by the editor's timer.
    struct History
    {
        std::array<std::atomic<float>, historySize> in {}, out {}, gainDb {};
        std::atomic<int> write { 0 };
    } history;

private:
    // Every parameter, read once per block; Magnitude pulls the scaled ones toward neutral.
    struct Control
    {
        const snap::ParamSpec* spec;
        std::atomic<float>* raw;
    };
    float value(std::string_view id) const; // host units, Magnitude applied
    bool on(std::string_view id) const { return value(id) > 0.5f; }
    float active(std::string_view onId) const { return on(onId) && *bypass < 0.5f ? 1.0f : 0.0f; } // module amount factor
    snap::SubHiss::Settings subHissSettings() const;
    snap::Space::Settings spaceSettings() const;
    void setModuleParams();
    snap::Master::Params masterParams() const;
    void setLowLatency(bool);
    int factorFor(bool low) const { return *oversampling > 0.5f && !low ? 1 << snap::constants::oversamplingStages : 1; }
    int latencyFor(bool low) const;
    void parameterChanged(const juce::String&, float) override; // low_latency / oversampling: latency or re-prepare
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override; // "order" -> audio thread
    void handleAsyncUpdate() override; // re-prepare for an oversampling change that arrived off the message thread
    void reprepare();
    void commit() { apvts.copyState(); } // flushes parameter changes into the tree, so they join the current undo step

    static constexpr int subBlock = 64; // the chain runs in short slices so the display trace stays fine-grained
    std::atomic<float>*magnitude, *bypass, *lowLatency, *oversampling;
    std::unordered_map<std::string_view, Control> controls;
    snap::Locks locks {};
    snap::Snapshots snapshots;
    std::atomic<int> requestedOrder { snap::pack(snap::defaultChainOrder) };
    double sampleRate = 48000;
    bool prepared = false;
    bool lowLatencyNow = false; // the mode the chain currently runs in (audio thread)
    int factor = 1;             // oversampling factor the chain was prepared for
    snap::Oversampler os;
    snap::Delay chainPad; // whatever the constant latency has left over after the stages
    snap::Smoother inGainDb;
    snap::SubHiss subHiss;
    snap::ReorderCrossfader middle; // shift, space, press, drive in the stored order
    snap::Push push;
    juce::AudioBuffer<float> postBus; // sub/hiss Post layers, delayed to meet the chain just before Clip
    snap::Delay postDelay;
    snap::Master master;
    snap::LoudnessMeter inputMeter;
    float peakIn = 0, peakOut = 0, peakGain = 0;
    int decimation = 1, count = 0;
};
