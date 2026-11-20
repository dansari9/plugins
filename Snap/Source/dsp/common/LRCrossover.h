#pragma once
#include "Smoother.h"
#include <juce_dsp/juce_dsp.h>

namespace snap
{
// One Linkwitz-Riley 4th-order split (two TPT state-variable stages, so the cutoff can move per sample without zipper).
// The cutoff glides in the log domain. At or below focusMinHz the whole signal goes to `high`; at or above focusMaxHz it
// all goes to `low`. Those ends crossfade in over the smoothing time and, once there, pass the input through exactly.
class LRCrossover
{
public:
    void prepare(double fs, int channels)
    {
        sampleRate = fs;
        filter.prepare({ fs, 1, (juce::uint32) channels });
        for (auto* smoother : { &logHz, &toSplit, &toLow, &toHigh })
            smoother->prepare(fs);
        const float hz = std::log2(cutoffHz);
        logHz.reset(hz), filter.setCutoffFrequency(clampHz(hz));
        toSplit.reset(route(1)), toLow.reset(route(2)), toHigh.reset(route(0));
    }
    void reset() { filter.reset(); }

    void setCutoff(float hz)
    {
        cutoffHz = hz;
        logHz.setTarget(std::log2(hz)), toSplit.setTarget(route(1)), toLow.setTarget(route(2)), toHigh.setTarget(route(0));
    }

    // Advance the glide by one sample; call once per sample frame, before split()/allpass() for each channel.
    void step()
    {
        if (logHz.isSmoothing())
            filter.setCutoffFrequency(clampHz(logHz.next()));
        s = toSplit.next(), l = toLow.next(), h = toHigh.next();
    }

    void split(int channel, float x, float& low, float& high)
    {
        float lo, hi;
        filter.processSample(channel, x, lo, hi);
        low = s * lo + l * x, high = s * hi + h * x;
    }
    // The phase shift that low + high carries, for a band that skips this split (keeps 3-way sums flat).
    // Use a separate instance at the same cutoff: it shares no state with split().
    float allpass(int channel, float x)
    {
        float lo, hi;
        filter.processSample(channel, x, lo, hi);
        return s * (lo + hi) + (1.0f - s) * x;
    }

    void snapToZero() { filter.snapToZero(); }
    bool isBypassed() const { return s == 0.0f && !toSplit.isSmoothing(); }

private:
    // 0: everything high, 1: split, 2: everything low. Returns 1 when `which` matches the current cutoff, else 0.
    float route(int which) const
    {
        const int r = cutoffHz <= constants::focusMinHz ? 0 : cutoffHz >= constants::focusMaxHz ? 2 : 1;
        return r == which ? 1.0f : 0.0f;
    }
    float clampHz(float log2Hz) const
    {
        return juce::jlimit(constants::focusMinHz, (float) (sampleRate * constants::crossoverMaxFraction), std::exp2(log2Hz));
    }

    juce::dsp::LinkwitzRileyFilter<float> filter;
    Smoother logHz, toSplit, toLow, toHigh;
    double sampleRate = 48000;
    float cutoffHz = 1000, s = 1, l = 0, h = 0;
};
} // namespace snap
