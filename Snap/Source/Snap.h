#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <array>

// Snap: drum transient shaper by daveydidit.
// FROZEN: parameter IDs "attack", "sustain", "mix", "output" (ParameterID version 1) and the
// state format (APVTS XML with stateVersion = 1). Never rename or remove; only add.
class SnapProcessor : public juce::AudioProcessor
{
public:
    static constexpr int stateVersion = 1;
    static constexpr int historySize = 512; // ~2.5 s of display history

    SnapProcessor();
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Snap"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    // Display feed: written by the audio thread, read by the editor's timer.
    struct History
    {
        std::array<std::atomic<float>, historySize> in {}, out {}, gainDb {};
        std::atomic<int> write { 0 };
    } history;

private:
    std::atomic<float>*attack, *sustain, *mix, *output;
    juce::SmoothedValue<float> sAttack, sSustain, sMix, sOut;
    float fast = 0, slowAttack = 0, slowRelease = 0, gainDb = 0;
    float cFastA = 0, cFastR = 0, cSlowA = 0, cSlowR = 0, cGain = 0;
    float peakIn = 0, peakOut = 0, peakGain = 0;
    int decimation = 1, count = 0;
};
