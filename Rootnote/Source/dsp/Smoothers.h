#pragma once

namespace rootnote
{
/**
    Linear ramp to a target over a set number of samples. Feed it a value in
    dB or in log2(Hz) and the ramp becomes multiplicative in gain or frequency.
*/
struct LinearRamp
{
    double value = 0.0;
    double target = 0.0;
    double step = 0.0;
    int remaining = 0;

    void snap(double v) noexcept
    {
        value = target = v;
        step = 0.0;
        remaining = 0;
    }

    void setTarget(double newTarget, int samples) noexcept
    {
        if (samples <= 0)
        {
            snap(newTarget);
            return;
        }
        target = newTarget;
        remaining = samples;
        step = (target - value) / static_cast<double>(samples);
    }

    bool isActive() const noexcept { return remaining > 0; }

    /** Moves the ramp forward by n samples and returns the new value. */
    double advance(int n) noexcept
    {
        if (remaining <= 0)
            return value;
        if (n >= remaining)
        {
            value = target;
            remaining = 0;
            step = 0.0;
        }
        else
        {
            value += step * static_cast<double>(n);
            remaining -= n;
        }
        return value;
    }

    double next() noexcept
    {
        if (remaining <= 0)
            return value;
        if (--remaining == 0)
        {
            value = target;
            step = 0.0;
        }
        else
        {
            value += step;
        }
        return value;
    }
};
} // namespace rootnote
