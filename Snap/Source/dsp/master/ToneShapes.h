#pragma once
#include "MasterConstants.h"
#include "../common/Smoother.h"
#include <array>

namespace snap
{
class ToneShapes
{
public:
    void prepare(double fs)
    {
        using namespace masterConstants;
        tilt = frequency(fs, tiltHz);
        bumpLow = frequency(fs, bumpLowHz), bumpMid = frequency(fs, bumpMidHz);
        smileLow = frequency(fs, smileLowHz), smileMid = frequency(fs, smileMidHz), smileHigh = frequency(fs, smileHighHz);
        focusPresence = frequency(fs, focusPresenceHz);
        focusSide = frequency(fs, focusSideShelfHz), focusMid = frequency(fs, focusMidShelfHz);
        tone.prepare(fs), shapeMix.prepare(fs, toneShapeFadeSeconds);
        reset();
    }
    void reset()
    {
        tone.reset(tone.targetValue()), shapeMix.reset(1);
        shape = requestedShape;
        clearFilters();
    }
    void setParams(float t, int choice)
    {
        tone.setTarget(t);
        requestedShape = std::clamp(choice, 0, 3);
        if (requestedShape != shape)
            shapeMix.setTarget(0);
    }

    void process(float& left, float& right, bool stereo)
    {
        using namespace masterConstants;
        const float t = tone.next();
        const float mix = shapeMix.next();
        if (mix == 0)
        {
            shape = requestedShape;
            clearFilters();
            shapeMix.setTarget(1);
            return;
        }
        if (t == 0)
        {
            if (lastTone != 0)
                clearFilters();
            return;
        }

        if (untilUpdate == 0)
        {
            if (lastTone != t)
                tune(t);
            untilUpdate = toneCoefficientInterval;
        }
        --untilUpdate;

        // Fade out residual filter state as the gain approaches unity.
        const float wet = mix * std::min(1.0f, std::abs(t) / toneNeutralFade);
        if (shape == 3)
        {
            const double mid = stereo ? (static_cast<double>(left) + right) * 0.5 : left;
            const double side = stereo ? (static_cast<double>(left) - right) * 0.5 : 0;
            const double midChange = filters[1].process(0, filters[0].process(0, mid)) - mid;
            const double sideChange = stereo ? filters[2].process(0, side) * sideGain - side : 0;
            left = static_cast<float>(left + wet * (midChange + sideChange));
            if (stereo)
                right = static_cast<float>(right + wet * (midChange - sideChange));
            return;
        }

        left = apply(0, left, wet);
        if (stereo)
            right = apply(1, right, wet);
    }

private:
    struct Frequency { double cosW = 0, sinW = 0; };
    struct Coefficients { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

    struct Biquad
    {
        double process(size_t ch, double x)
        {
            auto& [z1, z2] = state[ch];
            const double y = f.b0 * x + z1;
            z1 = f.b1 * x - f.a1 * y + z2, z2 = f.b2 * x - f.a2 * y;
            return y;
        }

        Coefficients f;
        std::array<std::array<double, 2>, 2> state {};
    };

    static Frequency frequency(double fs, double hz)
    {
        const double w = 2 * std::numbers::pi * std::min(hz / fs, masterConstants::maxCutoffFraction);
        return { std::cos(w), std::sin(w) };
    }

    static Coefficients bell(Frequency f, double db, double q)
    {
        if (db == 0)
            return {};
        const double a = std::pow(10.0, db / 40), alpha = f.sinW / (2 * q);
        const double a0 = 1 + alpha / a, b1 = -2 * f.cosW / a0;
        return { (1 + alpha * a) / a0, b1, (1 - alpha * a) / a0, b1, (1 - alpha / a) / a0 };
    }

    static Coefficients shelf(Frequency f, double db, bool high)
    {
        if (db == 0)
            return {};
        const double a = std::pow(10.0, db / 40);
        const double plus = a + 1, minus = a - 1, cosW = high ? -f.cosW : f.cosW;
        const double beta = f.sinW * std::sqrt(a * ((a + 1 / a) * (1 / masterConstants::toneShelfSlope - 1) + 2));
        const double a0 = plus + minus * cosW + beta, sign = high ? -1 : 1;
        return { a * (plus - minus * cosW + beta) / a0, sign * 2 * a * (minus - plus * cosW) / a0,
                 a * (plus - minus * cosW - beta) / a0, sign * -2 * (minus + plus * cosW) / a0,
                 (plus + minus * cosW - beta) / a0 };
    }

    void tune(float t)
    {
        using namespace masterConstants;
        switch (shape)
        {
            case 0:
                filters[0].f = shelf(tilt, -tiltDb * t, false);
                filters[1].f = shelf(tilt, tiltDb * t, true);
                break;
            case 1:
                filters[0].f = shelf(bumpLow, bumpLowDb * t, false);
                filters[1].f = bell(bumpMid, bumpMidDb * t, bumpMidQ);
                break;
            case 2:
                filters[0].f = shelf(smileLow, smileShelfDb * t, false);
                filters[1].f = bell(smileMid, smileMidDb * t, smileMidQ);
                filters[2].f = shelf(smileHigh, smileShelfDb * t, true);
                break;
            case 3:
            {
                const double tighten = std::max(0.0f, -t), widen = std::max(0.0f, t);
                filters[0].f = bell(focusPresence, focusPresenceDb * tighten, focusPresenceQ);
                filters[1].f = shelf(focusMid, focusMidShelfDb * widen, true);
                filters[2].f = shelf(focusSide, focusSideShelfDb * widen, true);
                sideGain = std::pow(10.0, focusSideCutDb * tighten / 20);
                break;
            }
        }
        lastTone = t;
    }

    float apply(size_t ch, float x, float wet)
    {
        double filtered = filters[1].process(ch, filters[0].process(ch, x));
        if (shape == 2)
            filtered = filters[2].process(ch, filtered);
        return static_cast<float>(x + wet * (filtered - x));
    }

    void clearFilters()
    {
        filters = {};
        lastTone = 0;
        untilUpdate = 0;
        sideGain = 1;
    }

    Frequency tilt, bumpLow, bumpMid, smileLow, smileMid, smileHigh, focusPresence, focusSide, focusMid;
    std::array<Biquad, 3> filters;
    Smoother tone, shapeMix;
    double sideGain = 1;
    float lastTone = 0;
    int shape = 0, requestedShape = 0, untilUpdate = 0;
};
} // namespace snap
