#pragma once

#include <algorithm>
#include <cmath>

namespace rootnote
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn10 = 2.30258509299404568402;

/**
    Bell (peaking) filter built on the trapezoidal state-variable filter from
    Andrew Simper's "Solving the continuous SVF equations using trapezoidal
    integration and equivalent currents" (Cytomic). It is stable for any
    positive g and k and tolerates fast frequency changes, which a direct-form
    biquad does not. Analog prototype:

        H(s) = (s^2 + s A/Q + 1) / (s^2 + s/(Q A) + 1),   A = 10^(dB/40)

    so the gain at the centre is exactly the set gain, and 0 dB is the identity.
*/
struct BellCoeffs
{
    double a1 = 1.0;
    double a2 = 0.0;
    double a3 = 0.0;
    double m1 = 0.0;
};

struct BellState
{
    double ic1 = 0.0;
    double ic2 = 0.0;
};

inline double bellPrewarp(double hz, double sampleRate) noexcept
{
    return std::tan(kPi * hz / sampleRate);
}

inline BellCoeffs makeBellCoeffs(double g, double q, double gainDb) noexcept
{
    const double A = std::exp(gainDb * (kLn10 / 40.0));
    const double k = 1.0 / (q * A);
    BellCoeffs c;
    c.a1 = 1.0 / (1.0 + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    c.m1 = k * (A * A - 1.0);
    return c;
}

inline double bellTick(const BellCoeffs& c, BellState& s, double v0) noexcept
{
    const double v3 = v0 - s.ic2;
    const double v1 = c.a1 * s.ic1 + c.a2 * v3;
    const double v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0 * v1 - s.ic1;
    s.ic2 = 2.0 * v2 - s.ic2;
    return v0 + c.m1 * v1;
}

/** Magnitude response in dB of the digital bell at frequency hz. */
inline double bellResponseDb(double hz, double centreHz, double q, double gainDb, double sampleRate) noexcept
{
    const double A = std::exp(gainDb * (kLn10 / 40.0));
    const double w = bellPrewarp(hz, sampleRate) / bellPrewarp(centreHz, sampleRate);
    const double reactive = 1.0 - w * w;
    const double numerator = reactive * reactive + (w * A / q) * (w * A / q);
    const double denominator = reactive * reactive + (w / (q * A)) * (w / (q * A));
    return 10.0 * std::log10(numerator / denominator);
}

/** Time for the resonance of the analog bell to fall by 60 dB after the input stops. */
inline double bellRingSeconds(double centreHz, double q, double gainDb) noexcept
{
    // Poles of the prototype have Q_pole = Q * A; the envelope is exp(-w0 t / (2 Q_pole)).
    const double A = std::exp(gainDb * (kLn10 / 40.0));
    return 6.907755 * 2.0 * q * A / (2.0 * kPi * centreHz);
}

/** T60 from the slowest digital pole, including overdamped (very wide) cuts. */
inline double bellDigitalRingSeconds(double centreHz, double q, double gainDb, double sampleRate) noexcept
{
    const double g = bellPrewarp(centreHz, sampleRate);
    const double k = 1.0 / (q * std::exp(gainDb * (kLn10 / 40.0)));
    double logRadius;
    if (k <= 2.0)
    {
        // Complex conjugate poles: log(sqrt((1 + g*g - g*k) / (1 + g*g + g*k))).
        logRadius = -std::atanh(g * k / (1.0 + g * g));
    }
    else
    {
        const double fast = 0.5 * (k + std::sqrt(k * k - 4.0));
        const auto radius = [g](double pole) { return std::abs((1.0 - g * pole) / (1.0 + g * pole)); };
        logRadius = std::log(std::max(radius(fast), radius(1.0 / fast)));
    }
    return -std::log(1000.0) / (sampleRate * logRadius);
}

/** One mono bell with fixed settings. Used by tests and as the reference implementation. */
class SvfBell
{
public:
    void set(double centreHz, double q, double gainDb, double sampleRate) noexcept
    {
        coeffs = makeBellCoeffs(bellPrewarp(centreHz, sampleRate), q, gainDb);
    }

    void reset() noexcept { state = {}; }

    double process(double x) noexcept { return bellTick(coeffs, state, x); }

private:
    BellCoeffs coeffs;
    BellState state;
};
} // namespace rootnote
