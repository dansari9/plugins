#pragma once
#include "Constants.h"
#include <algorithm>
#include <cmath>

namespace snap
{
// Linear parameter ramp that lands exactly on its target; tiny targets are flushed to zero.
class Smoother
{
public:
    void prepare(double fs, double seconds = constants::smoothingSeconds) { steps = std::max(1, (int) std::lround(fs * seconds)); }
    void reset(float v) { current = target = flush(v), left = 0; }
    void setTarget(float v)
    {
        if ((v = flush(v)) == target)
            return;
        target = v, left = steps, step = (target - current) / (float) steps;
    }
    float next()
    {
        if (left > 0)
            current = --left == 0 ? target : current + step;
        return current;
    }
    float value() const { return current; }
    float targetValue() const { return target; }
    bool isSmoothing() const { return left > 0; }

private:
    static float flush(float v) { return std::abs(v) < constants::denormalFloor ? 0.0f : v; }
    float current = 0, target = 0, step = 0;
    int steps = 1, left = 0;
};
} // namespace snap
