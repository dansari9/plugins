#pragma once
#include "Parameters.h"
#include "chain/ChainOrder.h"
#include "dsp/common/Delay.h"
#include "dsp/common/LoudnessMeter.h"
#include "dsp/common/Smoother.h"
#include "dsp/master/Master.h"
#include "dsp/modules/Drive.h"
#include "dsp/modules/Press.h"
#include "dsp/modules/Push.h"
#include "dsp/modules/Shift.h"
#include "dsp/modules/Space.h"
#include "dsp/modules/SubHiss.h"
#include <array>
#include <unordered_map>

// snap: drum multi-FX by daveydidit. Parameter table in Parameters.cpp, session state in presets/StateIO.
class SnapProcessor : public juce::AudioProcessor, private juce::AudioProcessorValueTreeState::Listener
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

    juce::AudioProcessorValueTreeState apvts;
    snap::ChainOrder chainOrder = snap::defaultChainOrder; // message thread; the chain runs the default order until M7

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
    void parameterChanged(const juce::String&, float) override; // low_latency: report the new latency

    static constexpr int subBlock = 64; // the chain runs in short slices so the display trace stays fine-grained
    std::atomic<float>*magnitude, *bypass, *lowLatency;
    std::unordered_map<std::string_view, Control> controls;
    double sampleRate = 48000;
    bool lowLatencyNow = false; // the mode the chain currently runs in (audio thread)
    snap::Delay chainPad;       // whatever the constant latency has left over after the stages (Low Latency Mode)
    snap::Smoother inGainDb;
    snap::SubHiss subHiss;
    snap::Shift shift;
    snap::Space space;
    snap::Press press;
    snap::Drive drive;
    snap::Push push;
    juce::AudioBuffer<float> postBus; // sub/hiss Post layers, delayed to meet the chain just before Clip
    snap::Delay postDelay;
    snap::Master master;
    snap::LoudnessMeter inputMeter;
    float peakIn = 0, peakOut = 0, peakGain = 0;
    int decimation = 1, count = 0;
};
