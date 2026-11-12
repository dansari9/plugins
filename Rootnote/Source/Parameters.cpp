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

/** First number in the text; accepts a decimal comma and the typographic minus. */
double leadingNumber(const juce::String& text)
{
    return text.replace(juce::String::fromUTF8("\xE2\x88\x92"), "-").replaceCharacter(',', '.').getDoubleValue();
}

/** "9.0 dB": a number with a fixed unit. Typed text only needs the number. */
Attributes withUnit(const char* unit, int decimals)
{
    const juce::String suffix = juce::String(" ") + unit;
    return Attributes()
        .withStringFromValueFunction([=](float v, int) { return juce::String(v, decimals) + suffix; })
        .withValueFromStringFunction([](const juce::String& text) { return static_cast<float>(leadingNumber(text)); });
}

/** "46.2 Hz" or "1.20 kHz". */
Attributes hertz()
{
    return Attributes()
        .withStringFromValueFunction([](float v, int) {
            return v < 1000.0f ? juce::String(v, 1) + " Hz" : juce::String(v / 1000.0f, 2) + " kHz";
        })
        .withValueFromStringFunction([](const juce::String& text) {
            return static_cast<float>(leadingNumber(text) * (text.containsIgnoreCase("k") ? 1000.0 : 1.0));
        });
}

/** "600 ms" or "1.20 s". */
Attributes milliseconds()
{
    return Attributes()
        .withStringFromValueFunction([](float v, int) {
            return v < 1000.0f ? juce::String(juce::roundToInt(v)) + " ms" : juce::String(v / 1000.0f, 2) + " s";
        })
        .withValueFromStringFunction([](const juce::String& text) {
            const bool seconds = text.containsIgnoreCase("s") && !text.containsIgnoreCase("ms");
            return static_cast<float>(leadingNumber(text) * (seconds ? 1000.0 : 1.0));
        });
}

/** "50 / 50": odd / even share. Typed text is the even share, or "odd / even". */
Attributes oddEven()
{
    return Attributes()
        .withStringFromValueFunction([](float v, int) {
            const int even = juce::roundToInt(v * 100.0f);
            return juce::String(100 - even) + " / " + juce::String(even);
        })
        .withValueFromStringFunction([](const juce::String& text) {
            return static_cast<float>(leadingNumber(text.fromLastOccurrenceOf("/", false, false)) / 100.0);
        });
}

/** "F#1", "F#1 +25 ct". Typed text may also be a frequency ("46 Hz"), converted with the current A4. */
Attributes noteText(const Float* a4)
{
    return Attributes()
        .withStringFromValueFunction([](float v, int) { return juce::String(formatNote(static_cast<double>(v))); })
        .withValueFromStringFunction([a4](const juce::String& text) {
            double note = kMinNote;
            parseNote(text.toStdString(), static_cast<double>(a4->get()), note);
            return static_cast<float>(juce::jlimit(kMinNote, kMaxNote, note));
        });
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    auto a4 = std::make_unique<Float>(pid(ids::a4), "A4", Range(415.0f, 466.0f, 0.1f), 440.0f, withUnit("Hz", 1));
    const Float* a4Ref = a4.get(); // owned by the layout, so it outlives the note parameters that read it

    const auto noteRange = Range(static_cast<float>(kMinNote), static_cast<float>(kMaxNote), 0.01f);
    const juce::StringArray keys { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const juce::StringArray scales { "Chromatic", "Major", "Minor", "Dorian", "Phrygian", "Mixolydian",
                                     "Pent Maj", "Pent Min", "Harmonic Minor" };

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto addFloat = [&](const char* id, const char* name, Range range, float value, Attributes attributes) {
        layout.add(std::make_unique<Float>(pid(id), name, std::move(range), value, std::move(attributes)));
    };
    const auto addInt = [&](const char* id, const char* name, int low, int high, int value) {
        layout.add(std::make_unique<juce::AudioParameterInt>(pid(id), name, low, high, value));
    };
    const auto addBool = [&](const char* id, const char* name, bool value) {
        layout.add(std::make_unique<juce::AudioParameterBool>(pid(id), name, value));
    };
    const auto addChoice = [&](const char* id, const char* name, const juce::StringArray& choices, int value = 0) {
        layout.add(std::make_unique<juce::AudioParameterChoice>(pid(id), name, choices, value));
    };

    addFloat(ids::boostNote, "Boost Note", noteRange, 36.0f, noteText(a4Ref));
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
    addFloat(ids::cutNote, "Cut Note", noteRange, 36.0f, noteText(a4Ref));
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
