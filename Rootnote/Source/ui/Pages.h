#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    What the eight encoders do on each page, and the short texts the display
    shows. Pure data and formatting; no components.
*/
namespace rootnote::ui
{
enum class Cap { orange, blue, grey, white };

struct Slot
{
    const char* id = nullptr; // parameter; nullptr = the encoder is unused on this page
    const char* label = "";
    Cap cap = Cap::orange;
    bool cents = false;       // the cents of the note parameter, not the parameter itself
};

constexpr int kNumPages = 5;
constexpr int kNumEncoders = 8;
using PageSlots = std::array<Slot, kNumEncoders>;

const char* pageName(int page);
const PageSlots& pageSlots(int page);
juce::Colour capColour(Cap);

/** "F♯1"; lower case for the status lines ("f♯1"). */
juce::String noteText(int midiNote, bool lowerCase = false);
/** Nearest note and its offset in cents: "d1 −12ct". */
juce::String noteWithCents(double note);
/** "+9.0" / "−12": a sign and at most maxDecimals, without trailing zeros (keeping minDecimals). */
juce::String signedNumber(double value, int maxDecimals, int minDecimals = 0);
juce::String number(double value, int maxDecimals, int minDecimals = 0);

/** A parameter's value as the legend shows it: "+9.0", "65HZ", "50/50". */
juce::String legendValue(const Slot&, juce::AudioProcessorValueTreeState&);

/** Short scale names for the status line: "min", "maj", "pmin". */
const char* scaleShortName(int scale);
} // namespace rootnote::ui
