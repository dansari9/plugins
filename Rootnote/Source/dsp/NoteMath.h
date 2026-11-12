#pragma once

#include <cmath>
#include <string>

namespace rootnote
{
// MIDI note 12 is C0 and 60 is C4 (scientific pitch notation), so the
// parameter range 12..96 reads C0..C7.
constexpr double kMinNote = 12.0;
constexpr double kMaxNote = 96.0;
constexpr double kMinA4 = 415.0;
constexpr double kMaxA4 = 466.0;

inline double noteToHz(double note, double a4 = 440.0) noexcept
{
    return a4 * std::exp2((note - 69.0) / 12.0);
}

inline double hzToNote(double hz, double a4 = 440.0) noexcept
{
    return 69.0 + 12.0 * std::log2(hz / a4);
}

/** Moves a MIDI note by whole octaves until it sits inside the parameter range. */
inline int foldNoteIntoRange(int note) noexcept
{
    while (note > static_cast<int>(kMaxNote))
        note -= 12;
    while (note < static_cast<int>(kMinNote))
        note += 12;
    return note;
}

/** "F#1" for MIDI note 30. Sharps only. */
std::string noteName(int midiNote);

/** "F#1" for a whole note, "F#1 +25 ct" when the note sits between semitones. */
std::string formatNote(double note);

/**
    Parses "F#1", "Gb1", "c2 +15 ct", "46 Hz", "1.2 kHz" or a plain MIDI note
    number ("36", "36.5"). Hz values are converted with the given A4.
    The result is not clamped to the parameter range.
*/
bool parseNote(const std::string& text, double a4, double& noteOut);
} // namespace rootnote
