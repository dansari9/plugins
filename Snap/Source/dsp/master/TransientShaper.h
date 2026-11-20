#pragma once
#include "../common/Constants.h"
#include <cmath>

namespace snap
{
// Envelope-difference transient shaper, linked across channels: transient = fast attack over slow attack,
// tail = slow release over fast release. Both are >= 0 dB amounts that Attack and Sustain (-100..+100 %) scale.
class TransientShaper
{
public:
    void prepare(double fs)
    {
        using namespace constants;
        fastA = coef(fs, shaperFastAttackMs), fastR = coef(fs, shaperFastReleaseMs), slowA = coef(fs, shaperSlowAttackMs);
        slowR = coef(fs, shaperSlowReleaseMs), smooth = coef(fs, shaperGainSmoothingMs);
        fast = slowAttack = slowRelease = gainDb = 0;
    }

    // Takes the linked input level and returns the gain in dB to apply to this sample.
    float next(float level, float attack, float sustain)
    {
        using namespace constants;
        fast = follow(fast, level, fastA, fastR);
        slowAttack = follow(slowAttack, level, slowA, fastR);
        slowRelease = follow(slowRelease, level, fastA, slowR);
        const float transient = std::fmax(0.0f, db(fast) - db(slowAttack)), tail = std::fmax(0.0f, db(slowRelease) - db(fast));
        const float target = std::fmin(shaperMaxGainDb, std::fmax(-shaperMaxGainDb, attack * shaperAttackDbPerPercentDb * transient
                                                                                        + sustain * shaperSustainDbPerPercentDb * tail));
        return gainDb = target + smooth * (gainDb - target);
    }

private:
    static float coef(double fs, double ms) { return (float) std::exp(-1000.0 / (ms * fs)); }
    static float follow(float e, float x, float a, float r) { return x + (x > e ? a : r) * (e - x); }
    static float db(float g) { return g > 1.0e-6f ? 20.0f * std::log10(g) : -120.0f; }

    float fast = 0, slowAttack = 0, slowRelease = 0, gainDb = 0;
    float fastA = 0, fastR = 0, slowA = 0, slowR = 0, smooth = 0;
};
} // namespace snap
