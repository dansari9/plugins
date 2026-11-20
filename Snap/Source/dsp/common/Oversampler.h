#pragma once
#include <juce_dsp/juce_dsp.h>

namespace snap
{
// Wraps juce::dsp::Oversampling: polyphase IIR half-bands in Low Latency Mode, linear-phase FIR otherwise. Latency is
// rounded to whole samples so the plug-in can report it. 0 stages is an exact pass-through with no latency.
class Oversampler
{
public:
    void prepare(int channels, int maxBlock, int stages, bool lowLatency)
    {
        os.reset();
        if (stages <= 0)
            return;
        using Os = juce::dsp::Oversampling<float>;
        os = std::make_unique<Os>((size_t) channels, (size_t) stages,
                                  lowLatency ? Os::filterHalfBandPolyphaseIIR : Os::filterHalfBandFIREquiripple, true, true);
        os->initProcessing((size_t) maxBlock);
    }
    void reset()
    {
        if (os)
            os->reset();
    }

    juce::dsp::AudioBlock<float> up(juce::dsp::AudioBlock<float> block) { return os ? os->processSamplesUp(block) : block; }
    void down(juce::dsp::AudioBlock<float> block)
    {
        if (os)
            os->processSamplesDown(block);
    }
    int factor() const { return os ? (int) os->getOversamplingFactor() : 1; }
    int latency() const { return os ? (int) os->getLatencyInSamples() : 0; }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
};
} // namespace snap
