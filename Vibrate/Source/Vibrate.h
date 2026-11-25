#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Presets.h"

class VibrateProcessor final : public juce::AudioProcessor
{
public:
    explicit VibrateProcessor(juce::File presetDirectory = {});

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
    int getNumPrograms() override { return vibrate::numPresets; }
    int getCurrentProgram() override { return juce::jmax(0, preset.load()); }
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    static constexpr int stateVersion = 1;
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<int> editorWidth { 0 };
    std::atomic<int> preset { 0 };

    juce::String presetName() const;
    void stepPreset(int delta);
    juce::StringArray userPresets() const;
    void loadUserPreset(const juce::String& name);
    juce::File saveUserPreset();
    int abSlot() const { return ab.load(); }
    void selectAb(int slot);

private:
    vibrate::Controls controls() const;
    bool bypassed() const;
    void process(juce::AudioBuffer<float>&, bool hostBypass);
    std::array<std::atomic<float>*, vibrate::ids.size()> parameters {};
    std::atomic<bool> resetPending { false };
    vibrate::Engine engine;

    using Slot = std::array<float, vibrate::numControls>;
    void setAll(const Slot& values);
    void applyParams(const juce::XmlElement& state, bool includeBypass);
    void setIdentity(int index, const juce::String& name);
    void serialize(juce::MemoryBlock& dest);
    juce::File presetFolder() const;
    struct Transaction;

    const juce::File directory;
    juce::CriticalSection txLock; // writers only, may call the host
    mutable juce::CriticalSection stateLock; // short reads, never across a host callback
    bool busy = false;
    juce::MemoryBlock committed;
    std::array<Slot, 2> slots {};
    std::array<int, 2> slotPreset {};
    std::array<bool, 2> slotStored {};
    std::atomic<int> ab { 0 };
    juce::String userName, slotName[2];
};
