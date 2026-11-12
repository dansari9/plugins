#pragma once

#include <cmath>

namespace rootnote
{
enum class Scale
{
    chromatic = 0,
    major,
    minor,
    dorian,
    phrygian,
    mixolydian,
    pentatonicMajor,
    pentatonicMinor,
    harmonicMinor
};

constexpr int kNumScales = 9;

/** Bit n is set when the note n semitones above the key root belongs to the scale. */
constexpr unsigned scaleMask(Scale scale) noexcept
{
    switch (scale)
    {
        case Scale::chromatic:       return 0b111111111111u;
        case Scale::major:           return 0b101010110101u; // 0 2 4 5 7 9 11
        case Scale::minor:           return 0b010110101101u; // 0 2 3 5 7 8 10
        case Scale::dorian:          return 0b011010101101u; // 0 2 3 5 7 9 10
        case Scale::phrygian:        return 0b010110101011u; // 0 1 3 5 7 8 10
        case Scale::mixolydian:      return 0b011010110101u; // 0 2 4 5 7 9 10
        case Scale::pentatonicMajor: return 0b001010010101u; // 0 2 4 7 9
        case Scale::pentatonicMinor: return 0b010010101001u; // 0 3 5 7 10
        case Scale::harmonicMinor:   return 0b100110101101u; // 0 2 3 5 7 8 11
    }
    return 0b111111111111u;
}

/** key: 0 = C .. 11 = B. */
inline bool isInScale(int midiNote, int key, Scale scale) noexcept
{
    const int degree = (((midiNote - key) % 12) + 12) % 12;
    return ((scaleMask(scale) >> degree) & 1u) != 0u;
}

/**
    Nearest scale note to a (possibly fractional) note. A tie between the note
    below and the note above resolves downwards.
*/
inline int snapToScale(double note, int key, Scale scale) noexcept
{
    const int below = static_cast<int>(std::floor(note));
    int best = below;
    double bestDistance = 1.0e9;
    for (int candidate = below - 6; candidate <= below + 7; ++candidate)
    {
        if (!isInScale(candidate, key, scale))
            continue;
        const double distance = std::abs(note - static_cast<double>(candidate));
        if (distance < bestDistance - 1.0e-9)
        {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}
} // namespace rootnote
