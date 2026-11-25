#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"

class VibrateProcessor final : public juce::AudioProcessor
{
public:
    VibrateProcessor();

    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    void reset() override { resetPending.store(true, std::memory_order_release); }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "init"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    vibrate::Controls controls() const;
    bool bypassed() const;
    void process(juce::AudioBuffer<float>&, bool hostBypass);
    std::array<std::atomic<float>*, vibrate::ids.size()> parameters {};
    std::atomic<bool> resetPending { false };
    vibrate::Engine engine;
};
