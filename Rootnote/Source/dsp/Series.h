#pragma once

#include <cmath>

namespace rootnote
{
enum class SeriesType
{
    harmonic = 0,
    octaves,
    drum
};

enum class QMode
{
    constantQ = 0, // every band has the same Q, so higher bands are wider in Hz
    constantWidth  // every band has the root band's width in Hz, so all bands ring equally long
};

constexpr int kMaxSlots = 32;

// The octave series uses a fixed slot-to-octave mapping so a band keeps its
// filter state while the root note moves. Slot kOctaveRootSlot is the root.
constexpr int kOctaveRootSlot = 6;
constexpr int kOctaveSlots = 19; // 2^-6 .. 2^12 covers 18 Hz .. 0.45 * 192 kHz for every root

// Mode frequencies of an ideal circular membrane: zeros j(m,n) of the Bessel
// functions J_m, divided by j(0,1) = 2.404826. Listed in rising order.
constexpr double kDrumRatios[kMaxSlots] = {
    1.000000, 1.593341, 2.135549, 2.295417, 2.653066, 2.917295, 3.155465, 3.500147,
    3.598485, 3.647451, 4.058932, 4.131738, 4.230439, 4.601045, 4.610052, 4.831885,
    4.903281, 5.083567, 5.130769, 5.412118, 5.540399, 5.553126, 5.650842, 5.976540,
    6.019356, 6.152609, 6.163137, 6.208732, 6.482735, 6.528612, 6.668997, 6.746213
};

/** Number of slots the series can address, before the Bands count is applied. */
constexpr int seriesSlotCount(SeriesType series) noexcept
{
    return series == SeriesType::octaves ? kOctaveSlots : kMaxSlots;
}

/** Frequency ratio of a slot to the root. slot must be below seriesSlotCount(). */
inline double seriesRatio(SeriesType series, int slot) noexcept
{
    switch (series)
    {
        case SeriesType::harmonic: return static_cast<double>(slot + 1);
        case SeriesType::octaves:  return std::exp2(static_cast<double>(slot - kOctaveRootSlot));
        case SeriesType::drum:     return kDrumRatios[slot];
    }
    return 1.0;
}

/** Slot that holds the root itself. */
constexpr int seriesRootSlot(SeriesType series) noexcept
{
    return series == SeriesType::octaves ? kOctaveRootSlot : 0;
}

/**
    Whether a slot is switched on for the given Bands count. The octave series
    ignores Bands: every octave inside the audible guard band is used.
*/
constexpr bool seriesSlotEnabled(SeriesType series, int slot, int bands) noexcept
{
    return series == SeriesType::octaves ? slot < kOctaveSlots : slot < bands;
}
} // namespace rootnote
