#pragma once
#include "EnvelopeFollower.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace snap
{
// Duck: an envelope of the module input pushes the added (generated or wet) signal down on each hit; on release the
// added signal swells back. Returns the gain for the added signal; amount 0 is exactly unity.
class Ducker
{
public:
    void prepare(double fs) { env.prepare(fs, constants::duckAttackMs, constants::duckReleaseMs); }
    void reset() { env.reset(); }

    float next(float keyLevel, float amount) // amount 0..1
    {
        using namespace constants;
        const float e = env.next(keyLevel);
        if (amount <= 0.0f || e <= 0.0f)
            return 1.0f;
        const float depth = juce::jlimit(0.0f, 1.0f, 1.0f - juce::Decibels::gainToDecibels(e, duckFloorDb) / duckFloorDb);
        return juce::Decibels::decibelsToGain(-amount * duckDepthDb * depth);
    }

private:
    EnvelopeFollower env;
};
} // namespace snap
