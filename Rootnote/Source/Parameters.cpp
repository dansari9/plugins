#include "Parameters.h"

namespace rootnote
{
namespace
{
using Float = juce::AudioParameterFloat;
using Attributes = juce::AudioParameterFloatAttributes;
using Range = juce::NormalisableRange<float>;

// Order of ParameterReader::raw. Must match kIds below.
enum Index
{
    iBoostNote, iBoostFine, iBoostGain, iBoostQ, iSeries, iBands, iBlend, iFocusLink, iFocus, iRange, iTilt,
    iQMode, iCutOn, iCutNote, iCutDepth, iCutQ, iCutOvertones, iCutBands, iRing, iRingDecay, iRingDrop,
    iMidiMode, iGlide, iNoteOff, iKey, iScale, iKeyLock, iA4, iMix, iOutput, iLimiter, iBypass, iCount
};

const char* const kIds[iCount] = {
    ids::boostNote, ids::boostFine, ids::boostGain, ids::boostQ, ids::series, ids::bands, ids::blend,
    ids::focusLink, ids::focus, ids::range, ids::tilt, ids::qMode, ids::cutOn, ids::cutNote, ids::cutDepth,
    ids::cutQ, ids::cutOvertones, ids::cutBands, ids::ring, ids::ringDecay, ids::ringDrop, ids::midiMode,
    ids::glide, ids::noteOff, ids::key, ids::scale, ids::keyLock, ids::a4, ids::mix, ids::output,
    ids::limiter, ids::bypass
};

juce::ParameterID pid(const char* id) { return { id, 1 }; }

/** Equal steps per octave, for frequencies, Q and times. */
Range logRange(float low, float high)
{
    const float span = std::log(high / low);
    return { low, high,
             [=](float, float, float t) { return low * std::exp(span * t); },
             [=](float, float, float v) { return std::log(v / low) / span; },
             [=](float, float, float v) { return juce::jlimit(low, high, v); } };
}

/** "9.0 dB": a number with a fixed unit. */
Attributes withUnit(const char* unit, int decimals)
{
    const juce::String suffix = juce::String(" ") + unit;
    return Attributes().withStringFromValueFunction([=](float v, int) { return juce::String(v, decimals) + suffix; });
}

/** "46.2 Hz" or "1.20 kHz". */
Attributes hertz()
{
    return Attributes().withStringFromValueFunction([](float v, int) {
        return v < 1000.0f ? juce::String(v, 1) + " Hz" : juce::String(v / 1000.0f, 2) + " kHz";
    });
}

/** "600 ms" or "1.20 s". */
Attributes milliseconds()
{
    return Attributes().withStringFromValueFunction([](float v, int) {
        return v < 1000.0f ? juce::String(juce::roundToInt(v)) + " ms" : juce::String(v / 1000.0f, 2) + " s";
    });
}

/** "50 / 50": odd / even share. */
Attributes oddEven()
{
    return Attributes().withStringFromValueFunction([](float v, int) {
        const int even = juce::roundToInt(v * 100.0f);
        return juce::String(100 - even) + " / " + juce::String(even);
    });
}

/** "F#1", "F#1 +25 ct". */
Attributes noteText()
{
    return Attributes().withStringFromValueFunction([](float v, int) { return juce::String(formatNote(static_cast<double>(v))); });
}

/** A number ("-3.1", "+5", ".5", "2,5"), then nothing or one of the units. Text is already in lower case. */
std::optional<double> number(const juce::String& text, std::initializer_list<const char*> units)
{
    const auto t = text.replaceCharacter(',', '.');
    int end = t[0] == '+' || t[0] == '-' ? 1 : 0;
    const int start = end;
    while (juce::CharacterFunctions::isDigit(t[end]) || t[end] == '.')
        ++end;
    const auto digits = t.substring(start, end);
    if (digits.removeCharacters(".").isEmpty() || digits.indexOfChar('.') != digits.lastIndexOfChar('.'))
        return {};
    const auto unit = t.substring(end).trim();
    if (unit.isNotEmpty() && std::none_of(units.begin(), units.end(), [&](const char* u) { return unit == u; }))
        return {};
    return t.substring(0, end).getDoubleValue();
}

/** Index of the first group of names that holds the text, compared without spaces. */
std::optional<float> choice(const juce::String& text, std::initializer_list<std::initializer_list<const char*>> names)
{
    const auto wanted = text.removeCharacters(" ");
    float index = 0.0f;
    for (const auto& group : names)
    {
        for (const char* name : group)
            if (wanted == juce::String(name).removeCharacters(" "))
                return index;
        index += 1.0f;
    }
    return {};
}

/** Nothing when there is no value or it is beyond a float (a 400-digit number). */
std::optional<float> toFloat(std::optional<double> value, double scale = 1.0)
{
    if (!value)
        return {};
    const auto result = static_cast<float>(*value * scale);
    return std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
}

/** Blend: the even share ("70", "70%"), or both shares "odd / even" adding up to 100. */
std::optional<float> blendShare(const juce::String& text)
{
    juce::StringArray shares;
    shares.addTokens(text, "/", "");
    if (shares.size() == 1)
        return toFloat(number(text, { "%" }), 0.01);
    if (shares.size() != 2)
        return {};
    const auto odd = number(shares[0].trim(), { "%" }), even = number(shares[1].trim(), { "%" });
    if (!odd || !even || *odd < 0.0 || *even < 0.0 || std::abs(*odd + *even - 100.0) > 1.0e-6)
        return {};
    return toFloat(even, 0.01);
}
} // namespace

std::optional<float> parseValue(const juce::String& id, const juce::String& rawText, double a4)
{
    const auto text = rawText.trim().toLowerCase()
                          .replace(juce::String::fromUTF8("\xE2\x88\x92"), "-")
                          .replace(juce::String::fromUTF8("\xE2\x99\xAF"), "#")
                          .replace(juce::String::fromUTF8("\xE2\x99\xAD"), "b");
    if (text.isEmpty())
        return {};

    if (id == ids::boostNote || id == ids::cutNote)
    {
        double note = 0.0;
        if (!parseNote(text.toStdString(), a4, note))
            return {};
        return static_cast<float>(juce::jlimit(kMinNote, kMaxNote, note));
    }
    if (id == ids::cutOn || id == ids::focusLink || id == ids::keyLock || id == ids::limiter || id == ids::bypass)
        return choice(text, { { "off", "no", "false", "0" }, { "on", "yes", "true", "1" } });
    if (id == ids::series)
        return choice(text, { { "harmonic", "harm" }, { "octaves", "octave", "oct" }, { "drum" } });
    if (id == ids::qMode)
        return choice(text, { { "constant q", "const q" }, { "constant width", "const width", "const w" } });
    if (id == ids::midiMode)
        return choice(text, { { "off" }, { "ch 1+2", "ch1+2" }, { "omni > boost", "omni" } });
    if (id == ids::noteOff)
        return choice(text, { { "hold" }, { "return", "return to knob" } });
    if (id == ids::scale)
        return choice(text, { { "chromatic", "chr" }, { "major", "maj" }, { "minor", "min" }, { "dorian", "dor" },
                              { "phrygian", "phr" }, { "mixolydian", "mix" }, { "pent maj", "pentatonic major", "pmaj" },
                              { "pent min", "pentatonic minor", "pmin" }, { "harmonic minor", "hmin" } });
    if (id == ids::key)
    {
        if (text[0] < 'a' || text[0] > 'g')
            return {};
        static const int semitoneOf[7] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
        int pitchClass = semitoneOf[text[0] - 'a'];
        for (int i = 1; i < text.length(); ++i)
        {
            if (text[i] != '#' && text[i] != 'b')
                return {};
            pitchClass += text[i] == '#' ? 1 : -1;
        }
        return static_cast<float>((pitchClass % 12 + 12) % 12);
    }

    if (id == ids::boostFine)
        return toFloat(number(text, { "ct", "c", "cent", "cents" }));
    if (id == ids::boostGain || id == ids::cutDepth || id == ids::output)
        return toFloat(number(text, { "db" }));
    if (id == ids::tilt)
        return toFloat(number(text, { "db", "db/oct" }));
    if (id == ids::boostQ || id == ids::cutQ)
        return toFloat(number(text.startsWith("q") ? text.substring(1).trim() : text, { "q" }));
    if (id == ids::bands || id == ids::cutBands)
        return toFloat(number(text, { "b", "band", "bands" }));
    if (id == ids::blend) // the even share, or "odd / even"
        return blendShare(text);
    if (id == ids::focus || id == ids::a4)
    {
        const auto hz = number(text, { "hz", "khz", "k" });
        return toFloat(hz, text.containsChar('k') ? 1000.0 : 1.0);
    }
    if (id == ids::range)
        return toFloat(number(text, { "oct", "octave", "octaves" }));
    if (id == ids::ringDecay || id == ids::glide)
        return toFloat(number(text, { "ms", "s" }), text.endsWith("s") && !text.endsWith("ms") ? 1000.0 : 1.0);
    if (id == ids::ringDrop)
        return toFloat(number(text, { "st", "semitone", "semitones" }));
    return toFloat(number(text, { "%" })); // cut overtones, ring, mix
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    // Typed text, from the host or the editor, is read by parseValue(); text that is no value gives the default.
    const Float* a4Ref = nullptr; // owned by the layout, so it outlives the note parameters that read it
    const auto reader = [&a4Ref](const char* id, float fallback) {
        return [a4 = a4Ref, id, fallback](const juce::String& text) {
            return parseValue(id, text, a4 != nullptr ? static_cast<double>(a4->get()) : 440.0).value_or(fallback);
        };
    };
    auto a4 = std::make_unique<Float>(pid(ids::a4), "A4", Range(415.0f, 466.0f, 0.1f), 440.0f,
                                      withUnit("Hz", 1).withValueFromStringFunction(reader(ids::a4, 440.0f)));
    a4Ref = a4.get();

    const auto noteRange = Range(static_cast<float>(kMinNote), static_cast<float>(kMaxNote), 0.01f);
    const juce::StringArray keys { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const juce::StringArray scales { "Chromatic", "Major", "Minor", "Dorian", "Phrygian", "Mixolydian",
                                     "Pent Maj", "Pent Min", "Harmonic Minor" };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto addFloat = [&](const char* id, const char* name, Range range, float value, Attributes attributes) {
        layout.add(std::make_unique<Float>(pid(id), name, std::move(range), value,
                                           attributes.withValueFromStringFunction(reader(id, value))));
    };
    // Clamped before rounding, as the editor's path clamps: a huge number is the top of the range, not an overflow.
    const auto addInt = [&](const char* id, const char* name, int low, int high, int value) {
        const auto read = reader(id, static_cast<float>(value));
        layout.add(std::make_unique<juce::AudioParameterInt>(
            pid(id), name, low, high, value,
            juce::AudioParameterIntAttributes().withValueFromStringFunction([read, low, high](const juce::String& t) {
                return juce::roundToInt(juce::jlimit(static_cast<float>(low), static_cast<float>(high), read(t)));
            })));
    };
    const auto addBool = [&](const char* id, const char* name, bool value) {
        const auto read = reader(id, value ? 1.0f : 0.0f);
        layout.add(std::make_unique<juce::AudioParameterBool>(
            pid(id), name, value,
            juce::AudioParameterBoolAttributes().withValueFromStringFunction([read](const juce::String& t) { return read(t) >= 0.5f; })));
    };
    const auto addChoice = [&](const char* id, const char* name, const juce::StringArray& choices, int value = 0) {
        const auto read = reader(id, static_cast<float>(value));
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            pid(id), name, choices, value,
            juce::AudioParameterChoiceAttributes().withValueFromStringFunction([read, last = choices.size() - 1](const juce::String& t) {
                return juce::roundToInt(juce::jlimit(0.0f, static_cast<float>(last), read(t)));
            })));
    };

    addFloat(ids::boostNote, "Boost Note", noteRange, 36.0f, noteText());
    addFloat(ids::boostFine, "Fine", { -50.0f, 50.0f, 0.1f }, 0.0f, withUnit("ct", 1));
    addFloat(ids::boostGain, "Boost", { 0.0f, 24.0f, 0.1f }, 9.0f, withUnit("dB", 1));
    addFloat(ids::boostQ, "Resonance (Q)", logRange(0.5f, 200.0f), 12.0f, withUnit("Q", 1));
    addChoice(ids::series, "Series", { "Harmonic", "Octaves", "Drum" });
    addInt(ids::bands, "Bands", 1, kMaxSlots, 8);
    addFloat(ids::blend, "Odd / Even", { 0.0f, 1.0f, 0.01f }, 0.5f, oddEven());
    addBool(ids::focusLink, "Link Focus", true);
    addFloat(ids::focus, "Focus", logRange(20.0f, 8000.0f), 65.0f, hertz());
    addFloat(ids::range, "Range", { 0.1f, 6.0f, 0.01f }, 1.5f, withUnit("oct", 2));
    addFloat(ids::tilt, "Tilt", { -12.0f, 6.0f, 0.1f }, -3.0f, withUnit("dB/oct", 1));
    addChoice(ids::qMode, "Q Mode", { "Constant Q", "Constant Width" });

    addBool(ids::cutOn, "Cut", false);
    addFloat(ids::cutNote, "Cut Note", noteRange, 36.0f, noteText());
    addFloat(ids::cutDepth, "Cut Depth", { -30.0f, 0.0f, 0.1f }, -12.0f, withUnit("dB", 1));
    addFloat(ids::cutQ, "Cut Q", logRange(0.5f, 60.0f), 8.0f, withUnit("Q", 1));
    addFloat(ids::cutOvertones, "Cut Overtones", { 0.0f, 100.0f, 0.1f }, 50.0f, withUnit("%", 0));
    addInt(ids::cutBands, "Cut Bands", 1, 16, 6);

    addFloat(ids::ring, "Ring", { 0.0f, 100.0f, 0.1f }, 0.0f, withUnit("%", 0));
    addFloat(ids::ringDecay, "Decay", logRange(50.0f, 4000.0f), 600.0f, milliseconds());
    addFloat(ids::ringDrop, "Drop", { 0.0f, 12.0f, 0.01f }, 0.0f, withUnit("st", 2));

    addChoice(ids::midiMode, "MIDI", { "Off", "Ch 1+2", "Omni > Boost" }, 0);
    addFloat(ids::glide, "Glide", { 0.0f, 500.0f, 1.0f }, 0.0f, milliseconds());
    addChoice(ids::noteOff, "Note-off", { "Hold", "Return" });
    addChoice(ids::key, "Key", keys);
    addChoice(ids::scale, "Scale", scales);
    addBool(ids::keyLock, "Key Lock", false);
    layout.add(std::move(a4));

    addFloat(ids::mix, "Mix", { 0.0f, 100.0f, 0.1f }, 100.0f, withUnit("%", 0));
    addFloat(ids::output, "Output", { -24.0f, 12.0f, 0.1f }, 0.0f, withUnit("dB", 1));
    addBool(ids::limiter, "Limiter", true);
    addBool(ids::bypass, "Bypass", false);
    return layout;
}

ParameterReader::ParameterReader(const juce::AudioProcessorValueTreeState& apvts)
{
    static_assert(iCount == kCount, "one raw pointer per parameter");
    for (int i = 0; i < iCount; ++i)
        raw[i] = apvts.getRawParameterValue(kIds[i]);
}

EngineParams ParameterReader::read() const noexcept
{
    const auto value = [this](Index i) { return static_cast<double>(raw[i]->load(std::memory_order_relaxed)); };
    const auto index = [this](Index i) { return juce::roundToInt(raw[i]->load(std::memory_order_relaxed)); };
    const auto isOn = [this](Index i) { return raw[i]->load(std::memory_order_relaxed) >= 0.5f; };

    EngineParams p;
    p.boostNote = value(iBoostNote);
    p.boostFineCents = value(iBoostFine);
    p.boostGainDb = value(iBoostGain);
    p.boostQ = value(iBoostQ);
    p.series = static_cast<SeriesType>(index(iSeries));
    p.bands = index(iBands);
    p.blend = value(iBlend);
    p.focusLink = isOn(iFocusLink);
    p.focusHz = value(iFocus);
    p.rangeOct = value(iRange);
    p.tiltDbPerOct = value(iTilt);
    p.qMode = static_cast<QMode>(index(iQMode));

    p.cutOn = isOn(iCutOn);
    p.cutNote = value(iCutNote);
    p.cutDepthDb = value(iCutDepth);
    p.cutQ = value(iCutQ);
    p.cutOvertones = value(iCutOvertones) / 100.0;
    p.cutBands = index(iCutBands);

    p.ring = value(iRing) / 100.0;
    p.ringDecaySeconds = value(iRingDecay) / 1000.0;
    p.ringDropSemitones = value(iRingDrop);

    p.midiMode = static_cast<MidiMode>(index(iMidiMode));
    p.glideSeconds = value(iGlide) / 1000.0;
    p.noteOff = static_cast<NoteOffMode>(index(iNoteOff));
    p.key = index(iKey);
    p.scale = static_cast<Scale>(index(iScale));
    p.keyLock = isOn(iKeyLock);
    p.a4 = value(iA4);

    p.mix = value(iMix) / 100.0;
    p.outputDb = value(iOutput);
    p.limiter = isOn(iLimiter);
    p.bypass = isOn(iBypass);
    return p;
}
} // namespace rootnote
