#pragma once

#include <algorithm>
#include <cmath>

namespace rootnote
{
/**
    Zero-latency safety limiter, stereo linked.

    A peak follower (0.1 ms attack, 80 ms release) pulls the level down to the
    threshold, 1 dB under the ceiling. The follower needs a few samples to
    react, so whatever still overshoots goes through a tanh curve that starts
    at the threshold and saturates at the ceiling. The output can therefore
    never exceed the ceiling (-0.3 dBFS), and anything under the threshold
    passes unchanged.

    The follower stops at +40 dBFS. Beyond that the tanh curve does the work,
    and one absurd spike cannot hold the gain down for seconds afterwards.
*/
class Limiter
{
public:
    static constexpr double kCeiling = 0.96605087898981337;   // -0.3 dBFS
    static constexpr double kThreshold = 0.86099375218460053; // -1.3 dBFS
    static constexpr double kMaxEnvelope = 100.0;             // +40 dBFS

    void prepare(double sampleRate) noexcept
    {
        attack = 1.0 - std::exp(-1.0 / (0.0001 * sampleRate));
        release = 1.0 - std::exp(-1.0 / (0.080 * sampleRate));
        reset();
    }

    void reset() noexcept { envelope = 0.0; }

    /** Current gain reduction as a linear factor (1 = none). */
    double gainReduction() const noexcept { return envelope > kThreshold ? kThreshold / envelope : 1.0; }

    static double softClip(double x) noexcept
    {
        const double magnitude = std::abs(x);
        if (magnitude <= kThreshold)
            return x;
        const double knee = kCeiling - kThreshold;
        const double shaped = kThreshold + knee * std::tanh((magnitude - kThreshold) / knee);
        return x < 0.0 ? -shaped : shaped;
    }

    void tick(double& left, double& right) noexcept
    {
        const double peak = std::min(std::max(std::abs(left), std::abs(right)), kMaxEnvelope);
        envelope += (peak - envelope) * (peak > envelope ? attack : release);
        if (envelope > kThreshold)
        {
            const double gain = kThreshold / envelope;
            left *= gain;
            right *= gain;
        }
        left = softClip(left);
        right = softClip(right);
    }

private:
    double attack = 0.2;
    double release = 0.0003;
    double envelope = 0.0;
};
} // namespace rootnote
