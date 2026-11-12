#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <complex>
#include <vector>

// crumb: vintage-sampler bit crusher by daveydidit (clean-room: textbook DSP only).
// FROZEN: the parameter IDs listed in Crumb.cpp (ParameterID version 1) and the state format
// (APVTS XML <CRUMB stateVersion="1">). Never rename, remove or reorder; only append.
namespace crumb
{
void fft(std::vector<std::complex<double>>& x, bool inverse); // radix-2 in place, size a power of two
}

class CrumbProcessor : public juce::AudioProcessor
{
public:
    static constexpr int stateVersion = 1;

    CrumbProcessor();
    ~CrumbProcessor() override;
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Crumb"; }
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

private:
    struct Dsp;
    std::unique_ptr<Dsp> dsp; // allocated once in the constructor, never on the audio thread
};
