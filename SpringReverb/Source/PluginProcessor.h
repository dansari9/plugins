#pragma once

#include <JuceHeader.h>
#include "DSP/SpringReverb.h"

class SpringTankProcessor : public juce::AudioProcessor
{
public:
    SpringTankProcessor();
    ~SpringTankProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }
    juce::ChangeBroadcaster& getProgramBroadcaster() noexcept { return programBroadcaster; }
    float getInputMeter(int channel) const noexcept;
    float getOutputMeter(int channel) const noexcept;

private:
    void applyProgram(int index);
    void storePeaks(const juce::AudioBuffer<float>& buffer, std::atomic<float>* dest) noexcept;
    SpringReverb::Params readParams() const;

    SpringReverb reverb;
    juce::AudioProcessorValueTreeState apvts;
    juce::ChangeBroadcaster programBroadcaster;
    std::atomic<int> currentProgram { 0 };
    std::atomic<float> inputMeter[2] { 0.0f, 0.0f };
    std::atomic<float> outputMeter[2] { 0.0f, 0.0f };
    juce::SmoothedValue<float> bypassMix { 1.0f };
    juce::AudioBuffer<float> dryBuffer;

    std::atomic<float>* bypassRaw = nullptr;
    std::atomic<float>* decayRaw = nullptr;
    std::atomic<float>* tensionRaw = nullptr;
    std::atomic<float>* toneRaw = nullptr;
    std::atomic<float>* dripRaw = nullptr;
    std::atomic<float>* predelayRaw = nullptr;
    std::atomic<float>* mixRaw = nullptr;
    std::atomic<float>* outputRaw = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpringTankProcessor)
};
