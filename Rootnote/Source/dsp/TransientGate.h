#pragma once

#include <algorithm>
#include <cmath>

namespace rootnote
{
/**
    Opens for the start of a drum hit and nothing else.

    Two envelope followers watch the input. When the fast one jumps clear of
    the slow one, a hit has started: the gate rises from 0 to 1 in 1 ms, holds
    for 30 ms and fades out over 20 ms. A steady tone or a decaying tail never
    opens it. Used as the excitation window of the ring engine.

    The followers start from silence, so a hit on the very first sample
    counts. Keep the gate running while its output is not needed: a gate that
    is reset in the middle of a sound takes that sound for a hit.
*/
class TransientGate
{
public:
    static constexpr double kAttackSeconds = 0.001;
    static constexpr double kHoldSeconds = 0.030;
    static constexpr double kReleaseSeconds = 0.020;
    /** Area under one gate window, in seconds. */
    static constexpr double kEffectiveSeconds = 0.5 * kAttackSeconds + kHoldSeconds + 0.5 * kReleaseSeconds;

    void prepare(double sampleRate) noexcept
    {
        // The fast follower releases slowly enough to bridge the half-waves of
        // a 30 Hz tone; otherwise a sustained sub would look like a row of hits.
        fastAttack = coefficient(0.0002, sampleRate);
        fastRelease = coefficient(0.025, sampleRate);
        slowAttack = coefficient(0.020, sampleRate);
        slowRelease = coefficient(0.100, sampleRate);
        attackSamples = std::max(1, static_cast<int>(std::lround(kAttackSeconds * sampleRate)));
        holdSamples = std::max(1, static_cast<int>(std::lround(kHoldSeconds * sampleRate)));
        releaseSamples = std::max(1, static_cast<int>(std::lround(kReleaseSeconds * sampleRate)));
        lockoutSamples = static_cast<int>(std::lround(0.045 * sampleRate));
        reset();
    }

    void reset() noexcept
    {
        fast = slow = 0.0;
        phase = Phase::idle;
        counter = 0;
        level = 0.0;
        sinceTrigger = lockoutSamples;
        triggered = false;
    }

    /** Feeds one detector sample (any sign) and returns the gate value 0..1. */
    double tick(double detector) noexcept
    {
        const double magnitude = std::abs(detector);
        fast += (magnitude - fast) * (magnitude > fast ? fastAttack : fastRelease);
        slow += (fast - slow) * (fast > slow ? slowAttack : slowRelease);

        if (sinceTrigger < lockoutSamples)
            ++sinceTrigger;

        if (fast > kFloor && fast > kRatio * slow && sinceTrigger >= lockoutSamples)
        {
            phase = Phase::attack;
            counter = 0;
            attackFrom = level;
            sinceTrigger = 0;
            triggered = true;
        }

        switch (phase)
        {
            case Phase::idle:
                level = 0.0;
                break;
            case Phase::attack:
                ++counter;
                level = attackFrom + (1.0 - attackFrom) * static_cast<double>(counter) / static_cast<double>(attackSamples);
                if (counter >= attackSamples)
                {
                    level = 1.0;
                    phase = Phase::hold;
                    counter = 0;
                }
                break;
            case Phase::hold:
                level = 1.0;
                if (++counter >= holdSamples)
                {
                    phase = Phase::release;
                    counter = 0;
                }
                break;
            case Phase::release:
            {
                ++counter;
                const double t = static_cast<double>(counter) / static_cast<double>(releaseSamples);
                level = 0.5 * (1.0 + std::cos(3.14159265358979323846 * std::min(1.0, t)));
                if (counter >= releaseSamples)
                {
                    level = 0.0;
                    phase = Phase::idle;
                }
                break;
            }
        }
        return level;
    }

    /** True once after each new hit; reading clears it. */
    bool takeTrigger() noexcept
    {
        const bool was = triggered;
        triggered = false;
        return was;
    }

    bool isOpen() const noexcept { return phase != Phase::idle; }

private:
    enum class Phase { idle, attack, hold, release };

    static double coefficient(double seconds, double sampleRate) noexcept
    {
        return 1.0 - std::exp(-1.0 / (seconds * sampleRate));
    }

    static constexpr double kRatio = 1.8;    // about 5 dB
    static constexpr double kFloor = 0.003;  // about -50 dBFS

    double fastAttack = 0.1, fastRelease = 0.003, slowAttack = 0.001, slowRelease = 0.0001;
    double fast = 0.0, slow = 0.0;
    Phase phase = Phase::idle;
    int counter = 0;
    int attackSamples = 48, holdSamples = 1440, releaseSamples = 960, lockoutSamples = 2160;
    int sinceTrigger = 0;
    double level = 0.0;
    double attackFrom = 0.0;
    bool triggered = false;
};
} // namespace rootnote
