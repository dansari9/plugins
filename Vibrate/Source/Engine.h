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

// Two doubles worked on together. Each lane has the scalar's IEEE result, so the same bits.
#if defined(__GNUC__) || defined(__clang__)
using Pair = double __attribute__((vector_size(16)));

inline Pair snapZero(Pair value)
{
    using Mask = long long __attribute__((vector_size(16)));
    const Mask small = (value < 1e-20) & (value > -1e-20);
    return reinterpret_cast<Pair>(reinterpret_cast<Mask>(value) & ~small);
}
#else
struct Pair
{
    double a, b;
    double operator[](int i) const { return i == 0 ? a : b; }
    friend Pair operator+(Pair x, Pair y) { return { x.a + y.a, x.b + y.b }; }
    friend Pair operator-(Pair x, Pair y) { return { x.a - y.a, x.b - y.b }; }
    friend Pair operator*(Pair x, Pair y) { return { x.a * y.a, x.b * y.b }; }
    friend Pair operator/(Pair x, Pair y) { return { x.a / y.a, x.b / y.b }; }
    friend Pair operator+(double x, Pair y) { return { x + y.a, x + y.b }; }
    friend Pair operator*(double x, Pair y) { return { x * y.a, x * y.b }; }
    friend Pair operator*(Pair x, double y) { return { x.a * y, x.b * y }; }
    friend Pair operator/(Pair x, double y) { return { x.a / y, x.b / y }; }
};

inline Pair snapZero(Pair value)
{
    return { snapZero(value.a), snapZero(value.b) };
}
#endif

template <typename T>
T tanSmall(T x)
{
    // Taylor through x^9. All arguments are below 0.28 radians at the lowest rate.
    const T xx = x * x;
    return x + x * xx * (1.0 / 3 + xx * (2.0 / 15 + xx * (17.0 / 315 + xx * (62.0 / 2835))));
}

template <typename T>
T pow2Small(T x)
{
    // exp(x * log(2)), through x^9 for a maximum exponent of ±1.2.
    const T t = x * 0.69314718055994530942;
    return 1.0 + t * (1.0 + t * (1.0 / 2 + t * (1.0 / 6 + t * (1.0 / 24
        + t * (1.0 / 120 + t * (1.0 / 720 + t * (1.0 / 5040
        + t * (1.0 / 40320 + t / 362880.0))))))));
}

// Fixed independent streams, selected to avoid chance correlation over the first 120 seconds.
inline constexpr std::array<uint32_t, rates.size()> seeds {
    0xa511e9b3u, 0x2afdb42au, 0xb0e97ea1u, 0x36d54918u, 0xbcc1138fu, 0x42acde06u,
    0xa34e3f80u, 0x41734c5eu, 0x7b4d81ddu, 0xe241b397u, 0xae8afce8u
};

class Spline
{
public:
    void reset(int index, double sampleRate)
    {
        seed = seeds[static_cast<std::size_t>(index)];
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
            nextKnot();
        }
        return value;
    }

    void nextKnot()
    {
        ++knot;
        points = { points[1], points[2], points[3], point(knot + 3u) };
        coefficients();
    }

    double rate() const { return increment; }
    std::array<double, 4> cubic() const { return { a, b, c, d }; }

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

// Every modulator in step, laid out so the compiler evaluates them in pairs. The same arithmetic as Spline::next,
// in the same order, so the same bits.
class SplineBank
{
public:
    static constexpr std::size_t count = rates.size(), padded = (count + 1) / 2 * 2;
    using Values = std::array<double, padded>;

    void reset(double sampleRate)
    {
        fraction = {};
        for (std::size_t i = 0; i < count; ++i)
        {
            splines[i].reset(static_cast<int>(i), sampleRate);
            increment[i] = splines[i].rate();
            load(i);
        }
        until = 0;
    }

    void next(Values& values)
    {
        for (std::size_t i = 0; i < padded; ++i)
        {
            values[i] = ((a[i] * fraction[i] + b[i]) * fraction[i] + c[i]) * fraction[i] + d[i];
            fraction[i] += increment[i];
        }
        if (--until < 0)
            wrap();
    }

private:
    void load(std::size_t i)
    {
        const auto cubic = splines[i].cubic();
        a[i] = cubic[0];
        b[i] = cubic[1];
        c[i] = cubic[2];
        d[i] = cubic[3];
    }

    // Looks for a passed knot, then counts the samples no modulator can reach one in (two short, for rounding).
    void wrap()
    {
        double least = 1e6;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (fraction[i] >= 1.0)
            {
                fraction[i] -= 1.0;
                splines[i].nextKnot();
                load(i);
            }
            least = std::min(least, (1.0 - fraction[i]) / increment[i]);
        }
        until = std::max(0, static_cast<int>(least) - 2);
    }

    std::array<Spline, count> splines;
    Values a {}, b {}, c {}, d {}, fraction {}, increment {};
    int until = 0;
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

struct Sample
{
    double mid, input;
    Stereo out;
    Controls terms;
    double pitch1, pitch2, panGain, delay1, delay2, phase1, phase2;
    unsigned enabled;
};

struct IgnoreSample { void operator()(const Sample&) const {} };

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
        phaseScale = pi * 800.0 / fs;
        for (std::size_t i = 0; i < sideTaps.size(); ++i)
            sideTaps[i] = static_cast<unsigned>(std::round(fs * sideSeconds[i]));
        reset(controls, bypass);
    }

    void reset(const Controls& controls = defaults, bool bypass = false)
    {
        std::fill(line.begin(), line.end(), 0.0);
        cursor = 0;
        hpIc1 = hpIc2 = 0;
        phaseState = {};
        phaseActive = false;
        modulators.reset(fs);
        gain.reset(gains(controls, bypass));
        depth.reset(depths(controls));
    }

    void setControls(const Controls& controls, bool bypass = false)
    {
        gain.set(gains(controls, bypass), gainLength);
        depth.set(depths(controls), depthLength);
    }

    template <unsigned lanes = allLanes, typename Observer = IgnoreSample>
    Stereo process(float left, float right, const Observer& observe = {})
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

        SplineBank::Values n;
        modulators.next(n);

        double p = 0, a = 0, dl = 0, h = 0, s = 0;
        double pitch1 = 0, pitch2 = 0, panGain = 0, delay1 = 0, delay2 = 0, phase1 = 0, phase2 = 0;
        if constexpr ((lanes & (1u << pitch)) != 0)
            if (g[pitch] > 0)
            {
                pitch1 = 1.0 + fs * (0.0023 + d[pitchDepth] * n[0] * (9.0 / (3462.0 * rates[0]))
                                                            + d[pitchDepth] * n[2] * (1.0 / (3462.0 * rates[2])));
                pitch2 = 1.0 + fs * (0.0023 + d[pitchDepth] * n[1] * (9.0 / (3462.0 * rates[1]))
                                                            + d[pitchDepth] * n[3] * (1.0 / (3462.0 * rates[3])));
                p = g[pitch] * (tap(pitch1) - tap(pitch2));
            }
        if constexpr ((lanes & (1u << pan)) != 0)
            if (g[pan] > 0)
            {
                panGain = g[pan] * ((n[4] + 0.35 * n[5]) / 1.35);
                a = panGain * x;
            }
        if constexpr ((lanes & (1u << delay)) != 0)
            if (g[delay] > 0)
            {
                delay1 = 1.0 + fs * d[delayDepth] * (1.0 + n[6]);
                delay2 = 1.0 + fs * d[delayDepth] * (1.0 + n[7]);
                dl = g[delay] * (tap(delay1) - tap(delay2));
            }

        if constexpr ((lanes & (1u << phase)) != 0)
        {
            if (g[phase] > 0)
            {
                phaseActive = true;
                const Pair factors = pow2Small(Pair { d[phaseDepth] * n[8], d[phaseDepth] * n[9] });
                phase1 = 800.0 * factors[0];
                phase2 = 800.0 * factors[1];
                h = 0.5 * g[phase] * chains(x, factors * phaseScale);
            }
            else if (phaseActive)
            {
                phaseState = {};
                phaseActive = false;
            }
        }
        if constexpr ((lanes & (1u << side)) != 0)
            if (g[side] > 0)
            {
                double diffuse = 0;
                for (std::size_t i = 0; i < sideTaps.size(); ++i)
                    diffuse += sideWeights[i] * line[(cursor - sideTaps[i]) & lineMask];
                s = g[side] * (0.8 + 0.2 * n[10]) * diffuse;
            }

        cursor = (cursor + 1u) & lineMask;
        const double q = g[mix] * (p + a + dl + h + s);
        Stereo out { left, right };
        if (std::abs(q) > 0.0)
        {
            out.left = static_cast<float>(left + q);
            out.right = static_cast<float>(right - q);
        }
        unsigned enabled = 0;
        for (unsigned lane = 0; lane < numControls; ++lane)
            if (g[lane] > 0)
                enabled |= (1u << lane);
        observe({ input, x, out, { p, a, dl, h, s, q }, pitch1, pitch2, panGain, delay1, delay2, phase1, phase2,
                  enabled & (lanes | (1u << mix)) });
        return out;
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

    // Two four-stage allpass chains side by side; the first minus the second.
    double chains(double x, Pair frequency)
    {
        constexpr std::array<double, 4> ratios { 0.5, 0.84, 1.3, 2.1 };
        Pair y { x, x };
        for (std::size_t i = 0; i < ratios.size(); ++i)
        {
            const Pair g = tanSmall(frequency * ratios[i]);
            const Pair v = (y - phaseState[i]) * (g / (1.0 + g));
            const Pair low = phaseState[i] + v;
            phaseState[i] = snapZero(low + v);
            y = y - 2.0 * low;
        }
        return y[0] - y[1];
    }

    static constexpr std::array<double, 8> sideSeconds { .0009, .0017, .0029, .0043, .0059, .0077, .0098, .0119 };
    // Signed exp(-t / 5 ms), last tap balances DC, then all taps normalized to unit squared sum.
    static constexpr std::array<double, 8> sideWeights {
        0.6123163047452082, -0.521781535971371, -0.41044789358518047, 0.31020984468642915,
        0.2252585800098297, -0.1571575785172354, -0.1032598871745846, 0.04486216580690451
    };
    std::array<unsigned, 8> sideTaps {};
    double phaseScale = 0;
    std::array<Pair, 4> phaseState {};
    bool phaseActive = false;

    static constexpr double damping = 1.41421356237309504880;
    double fs = 48000;
    double hp1 = 0, hp2 = 0, hp3 = 0, hpIc1 = 0, hpIc2 = 0;
    unsigned cursor = 0, lineMask = 0;
    int gainLength = 1440, depthLength = 12000;
    std::vector<double> line;
    SplineBank modulators;
    Ramp<numControls> gain;
    enum Depth { pitchDepth, delayDepth, phaseDepth, numDepths };
    Ramp<numDepths> depth;
};
} // namespace vibrate
