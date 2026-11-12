#pragma once

#include "Series.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
constexpr double kMaxBandGainDb = 24.0;
constexpr double kMaxCutDb = 30.0;
constexpr double kLowSkipHz = 18.0;       // bands below this are off
constexpr double kLowFullHz = 20.0;       // bands from here up have full weight
constexpr double kNyquistGuard = 0.45;    // bands above this fraction of fs are off
constexpr double kNyquistFadeOctaves = 1.0 / 6.0;

inline double smoothstep01(double x) noexcept
{
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/**
    Fade at both ends of the spectrum. 1 inside the usable band, 0 below 18 Hz
    and above 0.45 fs. The upper fade covers the 1/6 octave below 0.45 fs, so
    no band ever sits above 0.45 fs.
*/
inline double edgeWeight(double hz, double sampleRate) noexcept
{
    const double top = kNyquistGuard * sampleRate;
    if (hz <= kLowSkipHz || hz >= top)
        return 0.0;

    double weight = 1.0;
    if (hz < kLowFullHz)
        weight *= smoothstep01(std::log(hz / kLowSkipHz) / std::log(kLowFullHz / kLowSkipHz));

    const double octavesBelowTop = std::log2(top / hz);
    if (octavesBelowTop < kNyquistFadeOctaves)
        weight *= smoothstep01(octavesBelowTop / kNyquistFadeOctaves);

    return weight;
}

/** Gaussian over log-frequency around the focus. rangeOct is the standard deviation in octaves. */
inline double rangeWeight(double hz, double focusHz, double rangeOct) noexcept
{
    const double x = std::log2(hz / focusHz) / rangeOct;
    return std::exp(-0.5 * x * x);
}

/**
    Odd/even balance for the harmonic series. Centre (0.5) keeps both at full
    weight, 0 is odd only, 1 is even only. The fundamental is never reduced.
*/
inline double blendWeight(int harmonicNumber, double blend) noexcept
{
    if (harmonicNumber <= 1)
        return 1.0;
    const double b = std::clamp(blend, 0.0, 1.0);
    return (harmonicNumber % 2) == 1 ? std::min(1.0, 2.0 * (1.0 - b))
                                     : std::min(1.0, 2.0 * b);
}

/** Weight slope in dB per octave relative to the root. */
inline double tiltWeight(double hz, double rootHz, double tiltDbPerOct) noexcept
{
    return std::pow(10.0, tiltDbPerOct * std::log2(hz / rootHz) / 20.0);
}

struct BoostShape
{
    SeriesType series = SeriesType::harmonic;
    int bands = 8;
    double blend = 0.5;
    bool focusLink = true;
    double focusHz = 65.0;
    double rangeOct = 1.5;
    double tiltDbPerOct = -3.0;
};

/**
    Dimensionless weight of a boost slot: range * blend * tilt, without the
    spectrum-edge fade. 0 for a slot the Bands count switches off.
*/
inline double boostShapeWeight(const BoostShape& shape, int slot, double rootHz) noexcept
{
    if (slot >= seriesSlotCount(shape.series) || !seriesSlotEnabled(shape.series, slot, shape.bands))
        return 0.0;

    const double hz = rootHz * seriesRatio(shape.series, slot);
    const double focus = shape.focusLink ? rootHz : shape.focusHz;
    double weight = rangeWeight(hz, focus, shape.rangeOct) * tiltWeight(hz, rootHz, shape.tiltDbPerOct);
    if (shape.series == SeriesType::harmonic)
        weight *= blendWeight(slot + 1, shape.blend);
    return weight;
}

/** Gain of a boost slot in dB, clamped to +-24 dB, including the spectrum-edge fade. */
inline double boostSlotGainDb(const BoostShape& shape, double gainDb, int slot, double rootHz, double sampleRate) noexcept
{
    const double weight = boostShapeWeight(shape, slot, rootHz);
    if (weight <= 0.0)
        return 0.0;
    const double hz = rootHz * seriesRatio(shape.series, slot);
    const double gain = std::clamp(gainDb * weight, -kMaxBandGainDb, kMaxBandGainDb);
    return gain * edgeWeight(hz, sampleRate);
}

/**
    Gain of a cut slot in dB (0 or negative). The root takes the full depth;
    every other band takes depth * overtones.
*/
inline double cutSlotGainDb(SeriesType series, int bands, double depthDb, double overtones,
                            int slot, double rootHz, double sampleRate) noexcept
{
    if (slot >= seriesSlotCount(series) || !seriesSlotEnabled(series, slot, bands))
        return 0.0;
    const double hz = rootHz * seriesRatio(series, slot);
    const double amount = slot == seriesRootSlot(series) ? 1.0 : std::clamp(overtones, 0.0, 1.0);
    const double gain = std::clamp(depthDb, -kMaxCutDb, 0.0) * amount;
    return gain * edgeWeight(hz, sampleRate);
}

/** Q of a slot for the chosen Q mode. */
inline double slotQ(QMode mode, double q, double ratio) noexcept
{
    if (mode == QMode::constantQ)
        return q;
    return std::clamp(q * ratio, 0.25, 20000.0);
}
} // namespace rootnote
