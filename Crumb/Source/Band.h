#pragma once
// crumb's band: a low and a high cut (Butterworth, 6 to 96 dB / oct, resonant edges), or the band itself cut out.
// Plain C++, doubles only; the site's port mirrors it.
#include <algorithm>
#include <array>
#include <cmath>

namespace crumb
{
// Zavalishin TPT state-variable filter (topology-preserving transform, trapezoidal integration).
struct Coef { double k = 2, a1 = 1, a2 = 0, a3 = 0; };
inline Coef svf(double g, double k)
{
    Coef c { k, 1 / (1 + g * (g + k)), 0, 0 };
    c.a2 = g * c.a1, c.a3 = g * c.a2;
    return c;
}
struct Svf
{
    double s1 = 0, s2 = 0;
    std::array<double, 3> run(double x, const Coef& c) // lowpass, bandpass, highpass
    {
        const double v3 = x - s2, v1 = c.a1 * s1 + c.a2 * v3, v2 = s2 + c.a2 * s1 + c.a3 * v3;
        s1 = 2 * v1 - s1, s2 = 2 * v2 - s2;
        return { v2, v1, x - c.k * v1 - v2 };
    }
};

inline constexpr int numSlopes = 8, slopeOrder[numSlopes] = { 1, 2, 3, 4, 6, 8, 12, 16 };

struct BandCoefs // one edge at one slope
{
    int pairs = 0;
    bool odd = false;
    double g1 = 0;
    Coef pair[8];
    void set(int order, double g, double reso)
    {
        pairs = order / 2, odd = order % 2 != 0, g1 = g / (1 + g);
        for (int i = 0; i < pairs; ++i)
        {
            const double theta = odd ? 3.14159265358979323846 * (i + 1) / order : 3.14159265358979323846 * (2 * i + 1) / (2 * order);
            pair[i] = svf(g, 2 * std::cos(theta) * (i == pairs - 1 ? std::pow(10.0, -1.3 * reso) : 1.0)); // the sharpest pair resonates
        }
    }
};
struct BandEdge // one channel's state of one edge
{
    Svf pair[8];
    double s = 0;
    double run(double x, const BandCoefs& c, bool high)
    {
        if (c.odd)
        {
            const double v = (x - s) * c.g1, lp = v + s;
            s = lp + v, x = high ? x - lp : lp;
        }
        for (int i = 0; i < c.pairs; ++i)
            x = pair[i].run(x, c.pair[i])[high ? 2 : 0];
        return x;
    }
};

// A configuration is 0 (nothing to do) or 1 | low << 1 | high << 2 | invert << 3 | slope << 4. Changes crossfade.
inline int bandConfig(double lowHz, double highHz, int slope, bool invert)
{
    const int low = lowHz > 20.05 ? 1 : 0, high = highHz < 19990 ? 1 : 0;
    return low + high == 0 ? 0 : 1 | low << 1 | high << 2 | (invert ? 8 : 0) | slope << 4;
}
struct Band
{
    struct Bank
    {
        int config = 0;
        BandCoefs low, high;
        BandEdge edge[2][2]; // channel, low / high
    };
    Bank banks[2];
    int active = 0, fadeAt = -1, fadeLength = 960;
    double fade = 0;
    std::array<double, 3> keys { -1, -1, -1 };

    void reset(double fs, int config)
    {
        *this = Band();
        fadeLength = std::max(1, (int) (0.02 * fs)), banks[0].config = config;
    }
    static void coefs(Bank& b, double gLow, double gHigh, double reso)
    {
        const int order = slopeOrder[b.config >> 4];
        if (b.config & 2) b.low.set(order, gLow, reso);
        if (b.config & 4) b.high.set(order, gHigh, reso);
    }
    // Once per sample before run(): gLow, gHigh = tan(pi f / fs) of the edges.
    void next(int config, double gLow, double gHigh, double reso)
    {
        const std::array<double, 3> now { gLow, gHigh, reso };
        bool fresh = keys != now;
        if (fadeAt < 0 && banks[active].config != config)
        {
            banks[1 - active] = Bank(), banks[1 - active].config = config;
            fadeAt = 0, fresh = true;
        }
        if (fresh)
        {
            keys = now;
            coefs(banks[active], gLow, gHigh, reso);
            if (fadeAt >= 0) coefs(banks[1 - active], gLow, gHigh, reso);
        }
        fade = 0;
        if (fadeAt >= 0)
        {
            fade = std::min(1.0, (double) ++fadeAt / fadeLength);
            if (fadeAt > fadeLength) active = 1 - active, fadeAt = -1, fade = 0;
        }
    }
    static double filter(Bank& b, int channel, double x)
    {
        if (b.config == 0) return x;
        auto& e = b.edge[channel];
        if (b.config & 8) // invert: what is under the low edge and over the high one
            return (b.config & 2 ? e[0].run(x, b.low, false) : 0.0) + (b.config & 4 ? e[1].run(x, b.high, true) : 0.0);
        if (b.config & 2) x = e[0].run(x, b.low, true);
        if (b.config & 4) x = e[1].run(x, b.high, false);
        return x;
    }
    bool idle() const { return fadeAt < 0 && banks[active].config == 0; }
    double run(int channel, double x)
    {
        const double y = filter(banks[active], channel, x);
        return fadeAt < 0 ? y : y + fade * (filter(banks[1 - active], channel, x) - y);
    }
};
} // namespace crumb
