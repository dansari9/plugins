#pragma once
#include "Delay.h"
#include "EnvelopeFollower.h"

namespace snap
{
// Target: how transient each sample is. Fast and slow followers on the linked level; T = (fast dB - slow dB - dead zone) / range,
// clamped to [0, 1] and lightly smoothed. process() delays the audio by the lookahead so the mask opens before the hit
// reaches the processing, and the delayed audio is the latency-aligned dry.
class TransientMask
{
public:
    void prepare(double fs, int channels, int maxLookahead)
    {
        using namespace constants;
        fast.prepare(fs, maskFastAttackMs, maskFastReleaseMs), slow.prepare(fs, maskSlowAttackMs, maskSlowReleaseMs);
        smooth = EnvelopeFollower::coef(fs, maskSmoothingMs), t = 0;
        audio.prepare(channels, maxLookahead);
    }
    void reset() { fast.reset(), slow.reset(), audio.reset(), t = 0; }
    void setLookahead(int samples) { audio.setDelay(samples); } // shrinks in Low Latency Mode
    int getLookahead() const { return audio.getDelay(); }

    // Writes T for each sample into mask, then delays io in place by the lookahead.
    void process(juce::AudioBuffer<float>& io, int n, float* mask)
    {
        for (int i = 0; i < n; ++i)
        {
            float level = 0;
            for (int c = 0; c < std::min(io.getNumChannels(), audio.getNumChannels()); ++c)
            {
                const float x = io.getSample(c, i);
                level = std::max(level, std::abs(x));
                io.setSample(c, i, audio.next(c, x));
            }
            const float target = juce::jlimit(0.0f, 1.0f, (db(fast.next(level)) - db(slow.next(level)) - constants::maskDeadZoneDb) / constants::maskRangeDb);
            mask[i] = t = target + smooth * (t - target);
        }
    }

    // Target slider tau in [-1, +1]: 0 processes everything, -1 only transients, +1 only sustain.
    static float weight(float transient, float tau)
    {
        return tau < 0 ? 1.0f + (transient - 1.0f) * -tau : 1.0f - transient * tau;
    }

private:
    static float db(float g) { return g > 1.0e-6f ? 20.0f * std::log10(g) : -120.0f; }
    EnvelopeFollower fast, slow;
    Delay audio;
    float smooth = 0, t = 0;
};
} // namespace snap
