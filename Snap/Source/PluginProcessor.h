#pragma once
#include "Parameters.h"
#include "chain/ChainOrder.h"
#include "dsp/common/Delay.h"
#include "dsp/common/LoudnessMeter.h"
#include "dsp/common/Smoother.h"
#include "dsp/master/Master.h"
#include <array>

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
    double getTailLengthSeconds() const override { return 0.0; }
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
    snap::ChainOrder chainOrder = snap::defaultChainOrder; // message thread; the chain reads it once modules exist

    // Display feed: written by the audio thread, read by the editor's timer.
    struct History
    {
        std::array<std::atomic<float>, historySize> in {}, out {}, gainDb {};
        std::atomic<int> write { 0 };
    } history;

private:
    // The master controls, read once per block and pulled toward neutral by Magnitude.
    enum { inGain, outGain, eqOn, cutLo, cutHi, lcShape, hcShape, tone, toneShape, air, attack, sustain, clipOn, numMaster };
    struct Control
    {
        const snap::ParamSpec* spec;
        std::atomic<float>* raw;
    };
    float scaled(int control) const;
    snap::Master::Params masterParams() const;
    void parameterChanged(const juce::String&, float) override; // low_latency: report the new latency

    static constexpr int subBlock = 64; // the master runs in short slices so the display trace stays fine-grained
    std::atomic<float>*magnitude, *bypass, *lowLatency;
    std::array<Control, numMaster> controls {};
    double sampleRate = 48000;
    snap::Delay chainPad; // the modules' share of the constant latency; they join it as they are wired (M7)
    snap::Smoother inGainDb;
    snap::Master master;
    snap::LoudnessMeter inputMeter;
    float peakIn = 0, peakOut = 0, peakGain = 0;
    int decimation = 1, count = 0;
};
