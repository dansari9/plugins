#pragma once

#include "Feed.h"

namespace vibrate
{
inline constexpr std::array<uint32_t, numControls> inks { 0xff4f9a, 0xff6a2f, 0xffc83d, 0x4fd6c8, 0x8f86ff, 0xe9cf8f };
inline constexpr std::array<int, numControls> dialRows { 5, 12, 19, 26, 33, 40 };
inline constexpr int plotX = 3, plotWidth = 94, plotRows = 6, readoutX = 106;
inline constexpr uint32_t baselineInk = 0x282828, signalInk = 0x454545, paperInk = 0x0d0d0d, textInk = 0xefe6d2;

inline constexpr uint8_t brailleBit(int x, int y)
{
    return static_cast<uint8_t>(y == 3 ? 0x40 << x : 1 << (y + 3 * x));
}

struct Reading
{
    unsigned active = 0;
    std::array<double, 2> cents {}, hz {};
    double panDb = 0, gapMs = 0, sideFraction = 0, mixFraction = 0, correlation = 0;
};

class Lanes
{
public:
    static constexpr int columns = plotWidth * 2, dotsHigh = plotRows * 4, centre = 12; // centre: row(0)
    static constexpr int framesPerColumn = 5, averagingFrames = 10;
    static constexpr double energyFloor = 6.30957344480193e-8; // -72 dBFS squared
    // Full scale of each lane's axis: cents, dB, ms, octaves either side of 800 Hz, side fraction.
    static constexpr double pitchCents = 15, panDb = 3.75, gapMs = 3.75, phaseOctaves = 1.75, sideFraction = 0.625;

    // A cell's dots in two inks: `dots` print over `back` (the centre line, the mix lane's grey).
    struct Cell
    {
        uint8_t dots = 0, back = 0;
        uint32_t ink = 0, backInk = 0;
    };
    using Plot = std::array<Cell, plotWidth * plotRows>;

    void clear()
    {
        history = {};
        frames = {};
        latest = {};
        frameAt = frameCount = hop = columnAt = 0;
        haveSequence = hasTrace = false;
        enabled = 0;
        mixGain = 0;
    }

    bool add(const Frame& frame)
    {
        bool changed = false;
        if (haveSequence && frame.sequence != sequence + 1u)
        {
            clear();
            changed = true;
        }
        haveSequence = true;
        sequence = frame.sequence;
        // A lane turned off goes blank at once. The mix lane shows the signal itself, whatever mix is set to.
        const unsigned disabled = enabled & ~frame.enabled & ~(1u << mix);
        enabled = frame.enabled;
        if (disabled != 0)
        {
            latest.active &= ~disabled;
            for (auto& column : history)
                column.active &= ~disabled;
            changed = true;
        }
        if (std::abs(frame.mixGain - mixGain) > 0.0)
        {
            mixGain = frame.mixGain;
            changed |= hasTrace;
        }

        frames[static_cast<std::size_t>(frameAt)] = frame;
        frameAt = (frameAt + 1) % averagingFrames;
        frameCount = std::min(averagingFrames, frameCount + 1);
        if (++hop < framesPerColumn)
            return changed;
        hop = 0;

        const auto value = read();
        if (value.active == 0 && std::all_of(frames.begin(), frames.end(), [](const Frame& f) { return f.midEnergy < energyFloor; }))
        {
            if (hasTrace)
            {
                history = {};
                latest = {};
                hasTrace = false;
                return true;
            }
            return changed;
        }

        latest = value;
        if (!hasTrace && value.active == 0)
            return changed;

        Column column;
        column.active = value.active;
        column.row[pitch] = { bipolar(value.cents[0] / pitchCents), bipolar(value.cents[1] / pitchCents) };
        column.row[pan][0] = bipolar(value.panDb / panDb);
        column.row[delay][0] = bipolar(value.gapMs / gapMs);
        if (value.hz[0] > 0 && value.hz[1] > 0)
            column.row[phase] = { frequency(value.hz[0]), frequency(value.hz[1]) };
        column.row[side][0] = level(value.sideFraction / sideFraction);
        // The centre's lowest and highest sample over this column's frames: the signal's extent, top row first.
        double low = 0, high = 0;
        for (int i = 1; i <= std::min(framesPerColumn, frameCount); ++i)
        {
            const auto& f = frames[static_cast<std::size_t>((frameAt - i + averagingFrames) % averagingFrames)];
            low = std::min(low, f.midLow);
            high = std::max(high, f.midHigh);
        }
        column.row[mix] = { bipolar(high), bipolar(low) };
        history[static_cast<std::size_t>(columnAt)] = column;
        columnAt = (columnAt + 1) % columns;
        hasTrace = true;
        return true;
    }

    const Reading& readout() const { return latest; }
    double mixShown() const { return mixGain; }

    // How many of a mix-lane column's lit dots, counted up from its lowest, print in the rainbow.
    static int rainbowDots(int lit, double mixGain)
    {
        return std::clamp(static_cast<int>(std::floor(mixGain * lit + 0.5)), 0, lit);
    }

    // The newest `width` cells of a lane (all of them by default).
    Plot plot(std::size_t lane, int width = plotWidth) const
    {
        Plot result {};
        const int count = 2 * width, offset = columns - count;
        const auto cell = [&](int x, int y) -> Cell& { return result[static_cast<std::size_t>(y / 4 * width + x / 2)]; };
        const auto dot = [&](int x, int y, uint32_t ink) {
            auto& c = cell(x, y);
            const auto bit = brailleBit(x % 2, y % 4);
            c.dots |= bit;
            c.back &= static_cast<uint8_t>(~bit);
            c.ink = ink;
        };
        const auto under = [&](int x, int y, uint32_t ink) {
            auto& c = cell(x, y);
            const auto bit = brailleBit(x % 2, y % 4);
            if ((c.dots & bit) != 0) return;
            c.back |= bit;
            c.backInk = ink;
        };
        const auto column = [&](int x) -> const Column& {
            return history[static_cast<std::size_t>((columnAt + offset + x) % columns)];
        };
        const unsigned mask = 1u << lane;
        const auto active = [&](int x) { return (column(x).active & mask) != 0; };
        const auto ink = [&](int x, Shade shade) { return colours(lane, x / 2, width, shade, x == count - 1); };

        for (int x = 0; x < count; x += 2)
            under(x, centre, baselineInk);

        if (lane == mix)
        {
            for (int x = 0; x < count; ++x)
                if (active(x))
                {
                    const auto [top, bottom] = column(x).row[mix];
                    const int rainbow = rainbowDots(bottom - top + 1, mixGain);
                    for (int y = top; y <= bottom; ++y)
                    {
                        if (y > bottom - rainbow) dot(x, y, ink(x, Shade::rainbow));
                        else under(x, y, ink(x, Shade::grey));
                    }
                }
            return result;
        }

        // One dot a column; a jump of more than a row is joined, the half nearer each end in that end's column.
        const auto trace = [&](int voice, Shade shade) {
            int prev = -1;
            for (int x = 0; x < count; ++x)
            {
                if (!active(x))
                {
                    prev = -1;
                    continue;
                }
                const int y = column(x).row[lane][static_cast<std::size_t>(voice)];
                if (prev >= 0)
                {
                    const int step = y > prev ? 1 : -1, between = std::abs(y - prev) - 1;
                    for (int k = 1; k <= between; ++k)
                        dot(k <= between / 2 ? x - 1 : x, prev + step * k, ink(k <= between / 2 ? x - 1 : x, shade));
                }
                dot(x, y, ink(x, shade));
                prev = y;
            }
        };
        if (lane == pitch || lane == phase)
            trace(1, Shade::second);
        trace(0, Shade::first);
        return result;
    }

    static uint32_t tint(uint32_t ink, uint32_t paper, double alpha)
    {
        uint32_t result = 0;
        for (unsigned shift : { 16u, 8u, 0u })
        {
            const auto top = static_cast<int>((ink >> shift) & 255u);
            const auto base = static_cast<int>((paper >> shift) & 255u);
            result |= static_cast<uint32_t>(std::round(base + (top - base) * alpha)) << shift;
        }
        return result;
    }

    // The five lane inks in order, pitch to side, blended across the lane: the family's rainbow.
    static uint32_t rainbow(double at)
    {
        const double place = std::clamp(at, 0.0, 1.0) * 4;
        const int from = std::min(3, static_cast<int>(place));
        return tint(inks[static_cast<std::size_t>(from + 1)], inks[static_cast<std::size_t>(from)], place - from);
    }

private:
    enum class Shade { first, second, grey, rainbow };

    struct Column
    {
        unsigned active = 0;
        std::array<std::array<int, 2>, numControls> row {};
    };

    static int row(double n)
    {
        return static_cast<int>(std::round((1.0 - std::clamp(n, -1.0, 1.0)) * 0.5 * (dotsHigh - 1)));
    }
    static int bipolar(double n) { return row(std::copysign(std::sqrt(std::abs(n)), n)); }
    static int frequency(double hz) { return row(std::log(hz / 800.0) / (phaseOctaves * 0.69314718055994530942)); }
    static int level(double n) { return row(2 * std::sqrt(std::max(0.0, n)) - 1); }

    // Older cells fade toward the page, 25 % at the left edge to the full ink at the newest; the newest dot is brighter.
    static uint32_t colours(std::size_t lane, int x, int width, Shade shade, bool newest)
    {
        uint32_t base = shade == Shade::grey ? signalInk
                      : shade == Shade::rainbow ? rainbow(width > 1 ? static_cast<double>(x) / (width - 1) : 1.0)
                      : shade == Shade::second ? tint(textInk, inks[lane], 0.35) : inks[lane];
        if (newest)
            return tint(0xffffff, base, 0.3);
        const double fade = 0.25 + 0.75 * (2.0 * x + 1) / (2.0 * width - 1);
        return tint(base, paperInk, fade);
    }

    Reading read() const
    {
        Frame average;
        for (int i = 0; i < frameCount; ++i)
        {
            const auto& f = frames[static_cast<std::size_t>(i)];
            for (std::size_t j = 0; j < average.energy.size(); ++j)
                average.energy[j] += f.energy[j];
            for (std::size_t j = 0; j < f.pitchStep.size(); ++j)
            {
                average.pitchStep[j] += f.pitchStep[j];
                average.phaseHz[j] += f.phaseHz[j];
            }
            average.panGain += f.panGain;
            average.gapSeconds += f.gapSeconds;
            average.midEnergy += f.midEnergy;
            average.feedEnergy += f.feedEnergy;
            average.leftEnergy += f.leftEnergy;
            average.rightEnergy += f.rightEnergy;
            average.cross += f.cross;
        }
        Reading value;
        // A lane draws while it is on and the centre it moves is there (its own term can pass through zero);
        // the mix lane draws the centre whenever there is one.
        const bool fed = average.feedEnergy >= energyFloor * frameCount;
        for (unsigned j = 0; j < mix; ++j)
            if (fed && (enabled & (1u << j)) != 0)
                value.active |= 1u << j;
        if (average.midEnergy >= energyFloor * frameCount)
            value.active |= 1u << mix;
        for (std::size_t j = 0; j < value.cents.size(); ++j)
        {
            value.cents[j] = 1200.0 * std::log2(1.0 - average.pitchStep[j] / frameCount);
            value.hz[j] = average.phaseHz[j] / frameCount;
        }
        const double panGain = average.panGain / frameCount;
        value.panDb = 20.0 * std::log10((1.0 + panGain) / (1.0 - panGain));
        value.gapMs = average.gapSeconds / frameCount * 1000.0;
        if (average.midEnergy > 0)
        {
            value.sideFraction = std::sqrt(average.energy[side] / average.midEnergy);
            value.mixFraction = std::sqrt(average.energy[mix] / average.midEnergy);
        }
        if (average.leftEnergy > 0 && average.rightEnergy > 0)
            value.correlation = std::clamp(average.cross / std::sqrt(average.leftEnergy * average.rightEnergy), -1.0, 1.0);
        return value;
    }

    std::array<Frame, averagingFrames> frames {};
    std::array<Column, columns> history {};
    Reading latest;
    uint32_t sequence = 0;
    unsigned enabled = 0;
    double mixGain = 0;
    int frameAt = 0, frameCount = 0, hop = 0, columnAt = 0;
    bool haveSequence = false, hasTrace = false;
};
} // namespace vibrate
