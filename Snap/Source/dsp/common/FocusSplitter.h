#pragma once
#include "Delay.h"
#include "LRCrossover.h"

namespace snap
{
// Focus: Linkwitz-Riley crossovers at two log edges. split() leaves only the band in the buffer for the module to process;
// merge() puts back low + high and crossfades the processed band against the band's own dry (never the full dry), both
// delayed by the module's latency. An edge at the end of its range drops out, so full range is an exact pass-through.
class FocusSplitter
{
public:
    void prepare(double fs, int channels, int maxBlock, int maxDelay)
    {
        for (auto* x : { &lo, &hi, &lowPhase })
            x->prepare(fs, channels);
        rest.setSize(channels, maxBlock), dry.setSize(channels, maxBlock);
        restDelay.prepare(channels, maxDelay), dryDelay.prepare(channels, maxDelay);
    }
    void reset()
    {
        for (auto* x : { &lo, &hi, &lowPhase })
            x->reset();
        restDelay.reset(), dryDelay.reset();
    }
    void setEdges(float loHz, float hiHz)
    {
        lo.setCutoff(std::min(loHz, hiHz)), hi.setCutoff(hiHz), lowPhase.setCutoff(hiHz);
    }
    void setDelay(int samples) { restDelay.setDelay(samples), dryDelay.setDelay(samples); }

    void split(juce::AudioBuffer<float>& io, int n)
    {
        const int channels = std::min(io.getNumChannels(), rest.getNumChannels());
        for (int i = 0; i < n; ++i)
        {
            lo.step(), hi.step(), lowPhase.step();
            for (int c = 0; c < channels; ++c)
            {
                float low, above, band, high;
                lo.split(c, io.getSample(c, i), low, above);
                hi.split(c, above, band, high);
                rest.setSample(c, i, lowPhase.allpass(c, low) + high);
                dry.setSample(c, i, band);
                io.setSample(c, i, band);
            }
        }
        for (auto* x : { &lo, &hi, &lowPhase })
            x->snapToZero();
    }

    // io holds the processed band; weight[i] is Mix x Target for sample i (0 = band dry, 1 = fully processed).
    void merge(juce::AudioBuffer<float>& io, const float* weight, int n)
    {
        const int channels = std::min(io.getNumChannels(), rest.getNumChannels());
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float d = dryDelay.next(c, dry.getSample(c, i));
                io.setSample(c, i, restDelay.next(c, rest.getSample(c, i)) + d + (io.getSample(c, i) - d) * weight[i]);
            }
    }

private:
    LRCrossover lo, hi, lowPhase; // lowPhase gives the low band the high edge's phase shift
    juce::AudioBuffer<float> rest, dry;
    Delay restDelay, dryDelay;
};
} // namespace snap
