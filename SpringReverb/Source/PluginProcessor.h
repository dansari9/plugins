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
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Surf Tank"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }

private:
    SpringReverb::Params readParams() const;

    SpringReverb reverb;
    juce::AudioProcessorValueTreeState apvts;

    std::atomic<float>* decayRaw = nullptr;
    std::atomic<float>* tensionRaw = nullptr;
    std::atomic<float>* toneRaw = nullptr;
    std::atomic<float>* dripRaw = nullptr;
    std::atomic<float>* predelayRaw = nullptr;
    std::atomic<float>* mixRaw = nullptr;
    std::atomic<float>* outputRaw = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpringTankProcessor)
};
