#pragma once

#include "Feed.h"

namespace vibrate
{
inline constexpr std::array<uint32_t, numControls> inks { 0xff4f9a, 0xff6a2f, 0xffc83d, 0x4fd6c8, 0x8f86ff, 0xe9cf8f };
inline constexpr std::array<int, numControls> dialRows { 5, 12, 19, 26, 33, 40 };
inline constexpr int plotX = 3, plotWidth = 94, plotRows = 6, readoutX = 106;
inline constexpr uint32_t baselineInk = 0x333333;

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
    static constexpr int columns = plotWidth * 2, dotsHigh = plotRows * 4;
    static constexpr int framesPerColumn = 5, averagingFrames = 10;
    static constexpr double energyFloor = 6.30957344480193e-8; // -72 dBFS squared

    struct Cell
    {
        uint8_t dots = 0;
        uint32_t ink = 0;
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
        const unsigned disabled = enabled & ~frame.enabled;
        enabled = frame.enabled;
        if (disabled != 0)
        {
            latest.active &= ~disabled;
            for (auto& column : history)
                column.active &= ~disabled;
            changed = true;
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
        column.row[pitch] = { bipolar(value.cents[0] / 12.0), bipolar(value.cents[1] / 12.0) };
        column.row[pan][0] = bipolar(value.panDb / 3.0);
        column.row[delay][0] = bipolar(value.gapMs / 3.0);
        if (value.hz[0] > 0 && value.hz[1] > 0)
            column.row[phase] = { frequency(value.hz[0]), frequency(value.hz[1]) };
        column.row[side][0] = fillLevel(value.sideFraction / 0.5);
        column.row[mix][0] = fillLevel(value.mixFraction);
        history[static_cast<std::size_t>(columnAt)] = column;
        columnAt = (columnAt + 1) % columns;
        hasTrace = true;
        return true;
    }

    const Reading& readout() const { return latest; }

    Plot plot(std::size_t lane) const
    {
        Plot result {};
        const auto dot = [&](int x, int y, uint32_t ink) {
            auto& cell = result[static_cast<std::size_t>(y / 4 * plotWidth + x / 2)];
            cell.dots |= brailleBit(x % 2, y % 4);
            cell.ink = ink;
        };

        if (lane != phase)
            for (int x = 0; x < columns; x += 4)
                dot(x, lane < phase ? 11 : dotsHigh - 1, baselineInk);

        const auto column = [this](int x) -> const Column& {
            return history[static_cast<std::size_t>((columnAt + x) % columns)];
        };
        const unsigned mask = 1u << lane;
        const auto trace = [&](int voice, std::size_t shade) {
            int prev = -1;
            for (int x = 0; x < columns; ++x)
            {
                if ((column(x).active & mask) == 0)
                {
                    prev = -1;
                    continue;
                }
                const int row = column(x).row[lane][static_cast<std::size_t>(voice)];
                if (prev < 0) prev = row;
                const auto ink = palette()[lane][static_cast<std::size_t>(x / 2)][shade];
                for (int y = std::min(row, prev); y <= std::max(row, prev); ++y)
                    dot(x, y, ink);
                prev = row;
            }
        };

        if (lane >= side)
            for (int x = 0; x < columns; ++x)
                if ((column(x).active & mask) != 0)
                {
                    const int from = column(x).row[lane][0];
                    const auto ink = palette()[lane][static_cast<std::size_t>(x / 2)][2];
                    for (int y = from; y < dotsHigh; y += y == from ? 1 : 2)
                        dot(x, y, ink);
                }
        if (lane == pitch || lane == phase)
            trace(1, 1);
        trace(0, 0);
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

private:
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
    static int frequency(double hz) { return row(2 * std::log(hz / 300.0) / std::log(7.0) - 1.0); }
    static int fillLevel(double n) { return row(2 * std::sqrt(std::max(0.0, n)) - 1.0); }

    using Palette = std::array<std::array<std::array<uint32_t, 3>, plotWidth>, numControls>;
    static const Palette& palette()
    {
        static const auto colors = [] {
            Palette result {};
            for (std::size_t lane = 0; lane < result.size(); ++lane)
                for (std::size_t x = 0; x < plotWidth; ++x)
                {
                    const double fade = 0.25 + 0.75 * std::pow((2.0 * static_cast<double>(x)) / (columns - 1), 1.1);
                    result[lane][x] = { tint(inks[lane], 0x0d0d0d, fade),
                        tint(inks[lane], 0x0d0d0d, fade * 0.6), tint(inks[lane], 0x0d0d0d, fade * 0.55) };
                }
            return result;
        }();
        return colors;
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
            average.leftEnergy += f.leftEnergy;
            average.rightEnergy += f.rightEnergy;
            average.cross += f.cross;
        }
        Reading value;
        for (std::size_t j = 0; j < average.energy.size(); ++j)
            if (average.energy[j] >= energyFloor * frameCount && (enabled & (1u << j)) != 0)
                value.active |= 1u << j;
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
    int frameAt = 0, frameCount = 0, hop = 0, columnAt = 0;
    bool haveSequence = false, hasTrace = false;
};
} // namespace vibrate
