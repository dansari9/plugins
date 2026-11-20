#pragma once
#include "Parameters.h"
#include "chain/ChainOrder.h"
#include "dsp/common/Delay.h"
#include "dsp/master/TransientShaper.h"
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

    juce::AudioProcessorValueTreeState apvts;
    snap::ChainOrder chainOrder = snap::defaultChainOrder; // message thread; the chain reads it once modules exist

    // Display feed: written by the audio thread, read by the editor's timer.
    struct History
    {
        std::array<std::atomic<float>, historySize> in {}, out {}, gainDb {};
        std::atomic<int> write { 0 };
    } history;

private:
    // A Magnitude-scaled parameter: its raw value, mapped toward neutral by Magnitude, then smoothed.
    struct Scaled
    {
        const snap::ParamSpec& spec;
        std::atomic<float>* raw;
        juce::SmoothedValue<float> smooth {};
        float target(float magnitude) const { return snap::applyMagnitude(spec, raw->load(), magnitude); }
    };
    enum { inGain, outGain, attack, sustain };
    void parameterChanged(const juce::String&, float) override; // low_latency: report the new latency

    std::atomic<float>*magnitude, *bypass, *lowLatency;
    double sampleRate = 48000;
    snap::Delay chainPad; // the chain's constant latency; modules join it as they are wired (M7)
    std::array<Scaled, 4> master;
    snap::TransientShaper shaper;
    float peakIn = 0, peakOut = 0, peakGain = 0;
    int decimation = 1, count = 0;
};
