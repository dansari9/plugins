#pragma once
#include "EnvelopeFollower.h"
#include "Constants.h"
#include <juce_dsp/juce_dsp.h>

namespace snap
{
// Trigger source for the sub/hiss generators: 12 dB/oct band-pass on the detection range (gentle slopes keep the
// detection delay short), peak envelope, threshold with hysteresis, rising edge only, minimum retrigger interval.
// The onset is taken as the last sample the envelope sat below the re-arm level. The hit is held until that onset
// reaches the lookahead-delayed audio, measuring its peak meanwhile, then reported there as a velocity in (0, 1].
class HitDetector
{
public:
    void prepare(double fs, int lookaheadSamples)
    {
        using namespace constants;
        for (auto* f : { &lo, &hi })
            f->prepare({ fs, 1, 1 });
        lo.setType(juce::dsp::StateVariableTPTFilterType::highpass), hi.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
        nyquistish = (float) (fs * constants::crossoverMaxFraction);
        env.prepare(fs, hitAttackMs, hitReleaseMs);
        retrigger = (int) std::lround(fs * hitRetriggerMs * 0.001);
        lookahead = std::max(0, lookaheadSamples);
        reset();
    }
    void reset() { lo.reset(), hi.reset(), env.reset(), armed = true, sinceHit = retrigger, sinceQuiet = 0, pending = -1; }
    void setRange(float loHz, float hiHz) // an edge at the end of the Focus range is skipped
    {
        useLo = loHz > constants::focusMinHz, useHi = hiHz < constants::focusMaxHz;
        lo.setCutoffFrequency(juce::jlimit(constants::focusMinHz, nyquistish, std::min(loHz, hiHz)));
        hi.setCutoffFrequency(juce::jlimit(constants::focusMinHz, nyquistish, hiHz));
    }
    void setThreshold(float dbfs) { threshold = juce::Decibels::decibelsToGain(dbfs); }

    // in: the undelayed module input. trig[i]: velocity of a hit landing at sample i, else 0.
    void process(const juce::AudioBuffer<float>& in, int n, float* trig)
    {
        const float rearm = threshold * juce::Decibels::decibelsToGain(-constants::hitHysteresisDb);
        for (int i = 0; i < n; ++i)
        {
            float x = 0;
            for (int c = 0; c < in.getNumChannels(); ++c)
                x += in.getSample(c, i);
            x /= (float) std::max(1, in.getNumChannels());
            x = useLo ? lo.processSample(0, x) : x;
            const float e = env.next(std::abs(useHi ? hi.processSample(0, x) : x));

            trig[i] = 0;
            sinceHit = std::min(sinceHit + 1, retrigger);
            sinceQuiet = e < rearm ? 0 : sinceQuiet + 1;
            armed = armed || e < rearm;
            if (armed && e >= threshold && sinceHit >= retrigger && pending < 0)
                armed = false, sinceHit = 0, pending = std::max(0, lookahead - sinceQuiet), peak = e;
            if (pending >= 0)
            {
                peak = std::max(peak, e);
                if (pending-- == 0)
                    trig[i] = velocity(peak);
            }
        }
        lo.snapToZero(), hi.snapToZero();
    }

private:
    float velocity(float p) const
    {
        using namespace constants;
        const float over = juce::jlimit(0.0f, 1.0f, juce::Decibels::gainToDecibels(p / threshold) / hitVelocityRangeDb);
        return hitMinVelocity + (1.0f - hitMinVelocity) * over;
    }

    juce::dsp::StateVariableTPTFilter<float> lo, hi;
    EnvelopeFollower env;
    float threshold = 0.1f, peak = 0, nyquistish = 20000;
    int retrigger = 1, lookahead = 0, sinceHit = 0, sinceQuiet = 0, pending = -1;
    bool armed = true, useLo = true, useHi = true;
};
} // namespace snap
