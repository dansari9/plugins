#include "Pages.h"

#include "Parameters.h"
#include "Theme.h"

namespace rootnote::ui
{
namespace
{
const juce::String kSharp = juce::String::fromUTF8("\xe2\x99\xaf");
const juce::String kMinus = juce::String::fromUTF8("\xe2\x88\x92");

const PageSlots kPages[kNumPages] = {
    { { { ids::boostGain, "Gain", Cap::orange }, { ids::boostQ, "Q", Cap::orange }, { ids::bands, "Bands", Cap::blue },
        { ids::focus, "Focus", Cap::blue }, { ids::range, "Range", Cap::grey }, { ids::blend, "Blend", Cap::grey },
        { ids::tilt, "Tilt", Cap::white }, { ids::boostFine, "Fine", Cap::white } } },
    { { { ids::cutDepth, "Depth", Cap::orange }, { ids::cutQ, "Q", Cap::orange },
        { ids::cutOvertones, "Overtones", Cap::blue }, { ids::cutBands, "Bands", Cap::blue },
        { ids::cutNote, "Note", Cap::grey }, { ids::cutNote, "Fine", Cap::grey, true },
        { ids::cutOn, "Cut", Cap::white }, {} } },
    { { { ids::ring, "Amount", Cap::orange }, { ids::ringDecay, "Decay", Cap::orange },
        { ids::ringDrop, "Drop", Cap::blue }, {}, {}, {}, {}, {} } },
    { { { ids::mix, "Mix", Cap::orange }, { ids::output, "Output", Cap::orange }, { ids::glide, "Glide", Cap::blue },
        { ids::a4, "A4", Cap::blue }, { ids::qMode, "Q Mode", Cap::grey }, {}, {}, {} } },
    { { { ids::midiMode, "Mode", Cap::orange }, { ids::noteOff, "Note-off", Cap::orange },
        { ids::key, "Key", Cap::blue }, { ids::scale, "Scale", Cap::blue }, {}, {}, {}, {} } },
};

bool is(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
} // namespace

const char* pageName(int page)
{
    static const char* const names[kNumPages] = { "boost", "cut", "ring", "out", "midi" };
    return names[juce::jlimit(0, kNumPages - 1, page)];
}

const PageSlots& pageSlots(int page)
{
    return kPages[juce::jlimit(0, kNumPages - 1, page)];
}

juce::Colour capColour(Cap cap)
{
    switch (cap)
    {
        case Cap::orange: return colours::orange;
        case Cap::blue:   return colours::blue;
        case Cap::grey:   return colours::grey;
        case Cap::white:  break;
    }
    return colours::white;
}

juce::String noteText(int midiNote, bool lowerCase)
{
    static const char* const names[] = { "C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B" };
    static const bool sharp[] = { false, true, false, true, false, false, true, false, true, false, true, false };
    const int pc = ((midiNote % 12) + 12) % 12;
    juce::String text = lowerCase ? juce::String(names[pc]).toLowerCase() : juce::String(names[pc]);
    if (sharp[pc])
        text << kSharp;
    return text << juce::String(midiNote / 12 - 1);
}

juce::String number(double value, int maxDecimals, int minDecimals)
{
    // juce::String(double, 0) would print every digit.
    juce::String text = maxDecimals > 0 ? juce::String(value, maxDecimals) : juce::String(juce::roundToInt(value));
    if (maxDecimals > 0)
    {
        int keep = text.length();
        const int point = text.indexOfChar('.');
        while (keep > point + 1 + minDecimals && text[keep - 1] == '0')
            --keep;
        if (keep == point + 1)
            --keep; // no decimals left: drop the point
        text = text.substring(0, keep);
    }
    if (text == "-0")
        text = "0";
    return text.replace("-", kMinus);
}

juce::String signedNumber(double value, int maxDecimals, int minDecimals)
{
    const juce::String text = number(value, maxDecimals, minDecimals);
    return text.startsWith(kMinus) ? text : "+" + text;
}

juce::String noteWithCents(double note)
{
    const int nearest = juce::roundToInt(note);
    return noteText(nearest, true) + " " + signedNumber((note - nearest) * 100.0, 0) + "ct";
}

const char* scaleShortName(int scale)
{
    static const char* const names[] = { "chr", "maj", "min", "dor", "phr", "mix", "pmaj", "pmin", "hmin" };
    return names[juce::jlimit(0, 8, scale)];
}

juce::String legendValue(const Slot& slot, juce::AudioProcessorValueTreeState& apvts)
{
    if (slot.id == nullptr)
        return {};
    const double v = static_cast<double>(apvts.getRawParameterValue(slot.id)->load());
    const char* id = slot.id;

    if (slot.cents)
        return signedNumber((v - std::round(v)) * 100.0, 0) + "CT";
    if (is(id, ids::boostGain)) return signedNumber(v, 1, 1);
    if (is(id, ids::boostQ) || is(id, ids::cutQ)) return number(v, v < 10.0 ? 1 : 0);
    if (is(id, ids::bands) || is(id, ids::cutBands)) return juce::String(juce::roundToInt(v));
    if (is(id, ids::focus)) return v < 1000.0 ? number(v, 0) + "HZ" : number(v / 1000.0, 2) + "KHZ";
    if (is(id, ids::range)) return number(v, 2, 1) + "OCT";
    if (is(id, ids::blend))
    {
        const int even = juce::roundToInt(v * 100.0);
        return juce::String(100 - even) + "/" + juce::String(even);
    }
    if (is(id, ids::tilt) || is(id, ids::cutDepth) || is(id, ids::output)) return signedNumber(v, 1) + "DB";
    if (is(id, ids::boostFine)) return signedNumber(v, 1) + "CT";
    if (is(id, ids::cutOvertones) || is(id, ids::ring) || is(id, ids::mix)) return number(v, 0) + "%";
    if (is(id, ids::cutNote)) return noteText(juce::roundToInt(v));
    if (is(id, ids::cutOn)) return v >= 0.5 ? "ON" : "OFF";
    if (is(id, ids::ringDecay) || is(id, ids::glide))
        return v < 1000.0 ? number(v, 0) + "MS" : number(v / 1000.0, 2) + "S";
    if (is(id, ids::ringDrop)) return number(v, 2) + "ST";
    if (is(id, ids::a4)) return number(v, 1) + "HZ";

    // Choices: the parameter's own text, shortened.
    const int index = juce::roundToInt(v);
    if (is(id, ids::qMode)) return index == 0 ? "CONST Q" : "CONST W";
    if (is(id, ids::midiMode)) return index == 0 ? "OFF" : (index == 1 ? "CH1+2" : "OMNI");
    if (is(id, ids::noteOff)) return index == 0 ? "HOLD" : "RETURN";
    if (is(id, ids::key)) return noteText(index + 12).dropLastCharacters(1);
    if (is(id, ids::scale)) return juce::String(scaleShortName(index)).toUpperCase();
    return apvts.getParameter(id)->getCurrentValueAsText().toUpperCase().removeCharacters(" ");
}
} // namespace rootnote::ui
