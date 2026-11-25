#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vibrate
{
enum Lane { pitch, pan, delay, phase, side, mix, numControls };
using Controls = std::array<double, numControls>;
inline constexpr Controls defaults { 0.30, 0.40, 0.30, 0.15, 0.25, 1.0 };
inline constexpr std::array<const char*, numControls + 1> ids { "pitch", "pan", "delay", "phase", "side", "mix", "bypass" };
inline constexpr double pi = 3.14159265358979323846;
inline constexpr std::array<double, 11> rates { 1.30, 1.78, 14.0, 19.2, 3.1, 11.0, 0.19, 0.27, 0.41, 0.53, 0.17 };

inline double snapZero(double value)
{
    return std::abs(value) < 1e-20 ? 0.0 : value;
}

inline double tanSmall(double x)
{
    // Taylor through x^9. All arguments are below 0.28 radians at the lowest rate.
    const double xx = x * x;
    return x + x * xx * (1.0 / 3 + xx * (2.0 / 15 + xx * (17.0 / 315 + xx * (62.0 / 2835))));
}

class Spline
{
public:
    void reset(int index, double sampleRate)
    {
        seed = 0xA511E9B3u + static_cast<uint32_t>(index) * 0x85EBCA77u;
        increment = rates[static_cast<std::size_t>(index)] / sampleRate;
        fraction = 0;
        knot = 0;
        for (std::size_t i = 0; i < points.size(); ++i)
            points[i] = point(static_cast<uint32_t>(i));
        coefficients();
    }

    double next()
    {
        const double value = ((a * fraction + b) * fraction + c) * fraction + d;
        fraction += increment;
        if (fraction >= 1.0)
        {
            fraction -= 1.0;
            ++knot;
            points = { points[1], points[2], points[3], point(knot + 3u) };
            coefficients();
        }
        return value;
    }

private:
    double uniform(uint32_t at) const
    {
        uint32_t x = seed + at * 0x9E3779B1u;
        x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
        x = (x ^ (x >> 13)) * 0xC2B2AE35u;
        return static_cast<double>(x ^ (x >> 16)) * (2.0 / 4294967296.0) - 1.0;
    }

    double point(uint32_t at) const { return (uniform(at) - 0.6 * uniform(at - 1u)) / 1.6; }

    void coefficients()
    {
        const auto [p0, p1, p2, p3] = points;
        a = ((p3 - p0) + 3.0 * (p1 - p2)) / 6.0;
        b = (p0 - 2.0 * p1 + p2) * 0.5;
        c = (p2 - p0) * 0.5;
        d = (p0 + 4.0 * p1 + p2) / 6.0;
    }

    std::array<double, 4> points {};
    uint32_t seed = 0, knot = 0;
    double fraction = 0, increment = 0, a = 0, b = 0, c = 0, d = 0;
};

template <std::size_t Size>
struct Ramp
{
    using Values = std::array<double, Size>;
    Values current {}, target {}, step {};
    int remaining = 0;

    void reset(const Values& values)
    {
        current = target = values;
        remaining = 0;
    }

    void set(const Values& values, int length)
    {
        if (target == values)
            return;
        target = values;
        remaining = length;
        for (std::size_t i = 0; i < Size; ++i)
            step[i] = (target[i] - current[i]) / static_cast<double>(length);
    }

    void advance()
    {
        if (remaining == 0)
            return;
        if (--remaining == 0)
            current = target;
        else
            for (std::size_t i = 0; i < Size; ++i)
                current[i] += step[i];
    }
};

struct Stereo { float left, right; };

class Engine
{
public:
    static constexpr unsigned allLanes = (1u << mix) - 1u;

    void prepare(double sampleRate, const Controls& controls = defaults, bool bypass = false)
    {
        fs = sampleRate;
        gainLength = std::max(1, static_cast<int>(std::round(0.030 * fs)));
        depthLength = std::max(1, static_cast<int>(std::round(0.250 * fs)));
        int size = 4;
        while (size < 0.016 * fs)
            size *= 2;
        line.resize(static_cast<std::size_t>(size));
        lineMask = static_cast<unsigned>(size - 1);
        const double g = tanSmall(pi * 160.0 / fs);
        hp1 = 1.0 / (1.0 + g * (g + damping));
        hp2 = g * hp1;
        hp3 = g * hp2;
        reset(controls, bypass);
    }

    void reset(const Controls& controls = defaults, bool bypass = false)
    {
        std::fill(line.begin(), line.end(), 0.0);
        cursor = 0;
        hpIc1 = hpIc2 = 0;
        for (std::size_t i = 0; i < modulators.size(); ++i)
            modulators[i].reset(static_cast<int>(i), fs);
        gain.reset(gains(controls, bypass));
        depth.reset(depths(controls));
    }

    void setControls(const Controls& controls, bool bypass = false)
    {
        gain.set(gains(controls, bypass), gainLength);
        depth.set(depths(controls), depthLength);
    }

    template <unsigned lanes = allLanes>
    Stereo process(float left, float right)
    {
        gain.advance();
        depth.advance();
        const auto& g = gain.current;
        const auto& d = depth.current;

        const double middle = (static_cast<double>(left) + right) * 0.5;
        const double input = std::isfinite(middle) ? middle : 0.0;
        const double v3 = input - hpIc2;
        const double v1 = hp1 * hpIc1 + hp2 * v3;
        const double v2 = hpIc2 + hp2 * hpIc1 + hp3 * v3;
        hpIc1 = snapZero(2.0 * v1 - hpIc1);
        hpIc2 = snapZero(2.0 * v2 - hpIc2);
        const double x = input - damping * v1 - v2;
        line[cursor] = x;

        const double n0 = modulators[0].next(), n1 = modulators[1].next();
        const double n2 = modulators[2].next(), n3 = modulators[3].next();
        const double n4 = modulators[4].next(), n5 = modulators[5].next();
        const double n6 = modulators[6].next(), n7 = modulators[7].next();
        modulators[8].next();
        modulators[9].next();
        modulators[10].next();

        double p = 0, a = 0, dl = 0;
        if constexpr ((lanes & (1u << pitch)) != 0)
            if (g[pitch] > 0)
            {
                const double tap1 = 1.0 + fs * (0.0023 + d[pitchDepth] * n0 * (9.0 / (3462.0 * rates[0]))
                                                            + d[pitchDepth] * n2 * (1.0 / (3462.0 * rates[2])));
                const double tap2 = 1.0 + fs * (0.0023 + d[pitchDepth] * n1 * (9.0 / (3462.0 * rates[1]))
                                                            + d[pitchDepth] * n3 * (1.0 / (3462.0 * rates[3])));
                p = g[pitch] * (tap(tap1) - tap(tap2));
            }
        if constexpr ((lanes & (1u << pan)) != 0)
            if (g[pan] > 0)
                a = g[pan] * ((n4 + 0.35 * n5) / 1.35) * x;
        if constexpr ((lanes & (1u << delay)) != 0)
            if (g[delay] > 0)
            {
                const double tap1 = 1.0 + fs * d[delayDepth] * (1.0 + n6);
                const double tap2 = 1.0 + fs * d[delayDepth] * (1.0 + n7);
                dl = g[delay] * (tap(tap1) - tap(tap2));
            }

        cursor = (cursor + 1u) & lineMask;
        const double q = g[mix] * (p + a + dl);
        if (std::abs(q) > 0.0)
            return { static_cast<float>(left + q), static_cast<float>(right - q) };
        return { left, right };
    }

private:
    static Controls gains(const Controls& c, bool bypass)
    {
        return { 0.20 * std::sqrt(c[pitch]), 0.171 * c[pan], 0.22 * std::sqrt(c[delay]),
                 0.30 * c[phase], 0.28 * c[side], bypass ? 0.0 : c[mix] };
    }

    static std::array<double, 3> depths(const Controls& c)
    {
        // Stored in pitch, delay, phase order. The pitch voice constants are part of the taps.
        return { c[pitch], 0.0015 * c[delay] * std::sqrt(c[delay]), 0.6 + 0.6 * c[phase] };
    }

    double tap(double distance) const
    {
        // Third-order Lagrange with the two central samples bracketing the read pointer.
        const auto whole = static_cast<unsigned>(distance);
        const double t = distance - static_cast<double>(whole);
        const double u = 1.0 - t;
        const unsigned at = cursor - whole;
        const double inner = (t + 1.0) * (2.0 - t) * 0.5;
        const double outer = t * u / 6.0;
        return inner * (u * line[at & lineMask] + t * line[(at - 1u) & lineMask])
             - outer * ((2.0 - t) * line[(at + 1u) & lineMask] + (t + 1.0) * line[(at - 2u) & lineMask]);
    }

    static constexpr double damping = 1.41421356237309504880;
    double fs = 48000;
    double hp1 = 0, hp2 = 0, hp3 = 0, hpIc1 = 0, hpIc2 = 0;
    unsigned cursor = 0, lineMask = 0;
    int gainLength = 1440, depthLength = 12000;
    std::vector<double> line;
    std::array<Spline, rates.size()> modulators;
    Ramp<numControls> gain;
    enum Depth { pitchDepth, delayDepth, phaseDepth, numDepths };
    Ramp<numDepths> depth;
};
} // namespace vibrate
