#pragma once
#include "Constants.h"
#include <cmath>

namespace snap
{
// One-pole peak follower with separate attack and release; feed it a rectified level.
class EnvelopeFollower
{
public:
    void prepare(double fs, double attackMs, double releaseMs) { a = coef(fs, attackMs), r = coef(fs, releaseMs), env = 0; }
    void reset() { env = 0; }
    float next(float level)
    {
        env = level + (level > env ? a : r) * (env - level);
        return env = env < constants::denormalFloor ? 0.0f : env;
    }
    float value() const { return env; }

    static float coef(double fs, double ms) { return ms > 0 ? (float) std::exp(-1000.0 / (ms * fs)) : 0.0f; }

private:
    float env = 0, a = 0, r = 0;
};
} // namespace snap
