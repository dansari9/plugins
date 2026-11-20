#pragma once
#include "Delay.h"
#include "LRCrossover.h"

namespace snap
{
// Focus: Linkwitz-Riley crossovers at two log edges. split() leaves only the band in the buffer for the module to process;
// merge() adds what the module changed in the band to the whole input: out = in + weight x (processed band - band), with
// in and band delayed by the module's latency. An unprocessed band (amount 0) or weight 0 is therefore an exact
// pass-through for any edges, and the audio outside the band keeps its phase. An edge at the end of its range drops out.
class FocusSplitter
{
public:
    void prepare(double fs, int channels, int maxBlock, int maxDelay)
    {
        lo.prepare(fs, channels), hi.prepare(fs, channels);
        input.setSize(channels, maxBlock), dry.setSize(channels, maxBlock);
        inputDelay.prepare(channels, maxDelay), dryDelay.prepare(channels, maxDelay);
    }
    void reset()
    {
        lo.reset(), hi.reset();
        inputDelay.reset(), dryDelay.reset();
    }
    void setEdges(float loHz, float hiHz)
    {
        lo.setCutoff(std::min(loHz, hiHz)), hi.setCutoff(hiHz);
    }
    void setDelay(int samples) { inputDelay.setDelay(samples), dryDelay.setDelay(samples); }

    void split(juce::AudioBuffer<float>& io, int n)
    {
        const int channels = std::min(io.getNumChannels(), input.getNumChannels());
        for (int i = 0; i < n; ++i)
        {
            lo.step(), hi.step();
            for (int c = 0; c < channels; ++c)
            {
                float low, above, band, high;
                lo.split(c, io.getSample(c, i), low, above);
                hi.split(c, above, band, high);
                input.setSample(c, i, io.getSample(c, i));
                dry.setSample(c, i, band);
                io.setSample(c, i, band);
            }
        }
        lo.snapToZero(), hi.snapToZero();
    }

    // io holds the processed band; weight[i] is Mix x Target for sample i (0 = dry, 1 = fully processed).
    void merge(juce::AudioBuffer<float>& io, const float* weight, int n)
    {
        const int channels = std::min(io.getNumChannels(), input.getNumChannels());
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float x = inputDelay.next(c, input.getSample(c, i)), d = dryDelay.next(c, dry.getSample(c, i));
                io.setSample(c, i, x + (io.getSample(c, i) - d) * weight[i]);
            }
    }

private:
    LRCrossover lo, hi;
    juce::AudioBuffer<float> input, dry;
    Delay inputDelay, dryDelay;
};
} // namespace snap
