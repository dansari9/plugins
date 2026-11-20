#include "Parameters.h"

using namespace juce;

namespace snap
{
namespace
{
constexpr const char* hissTypes[] = { "fizz",    "hush",  "drizzle", "sizzle", "grit",   "rustle", "crinkle", "spray", "steam",
                                      "clatter", "jangle", "scrape", "breath", "buzz",   "flutter", "sand",   "chime", "swarm" };
constexpr const char* spaceTypes[] = { "nook",   "booth",  "den",    "attic", "garage", "cellar", "chapel", "hangar", "tunnel", "stairwell",
                                       "silo",   "canyon", "sheet",  "coil",  "unfurl",  "smear",  "gate",   "rewind", "shimmer", "fog" };
constexpr const char* pressModes[] = { "tight", "glue", "crush", "silk", "lull", "stack" };
constexpr const char* driveModes[] = { "crunch", "pocket", "reel", "glow", "crumble", "wobble" };
constexpr const char* pushMults[] = { "x1", "x2", "x4" };
constexpr const char* lowCutShapes[] = { "gentle", "steep" };
constexpr const char* highCutShapes[] = { "soft", "steep" };
constexpr const char* toneShapes[] = { "tilt", "low bump", "smile", "band focus" };

constexpr ParamSpec pct(const char* id, const char* name, float def, bool m = false) { return { id, name, Unit::percent, 0, 100, def, m }; }
constexpr ParamSpec sgn(const char* id, const char* name, float def = 0, bool m = false, bool a = true)
{
    return { id, name, Unit::signedPercent, -100, 100, def, m, a };
}
constexpr ParamSpec bip(const char* id, const char* name, float def = 0) { return { id, name, Unit::bipolar, -1, 1, def }; }
constexpr ParamSpec hz(const char* id, const char* name, float lo, float hi, float def, bool m = false) { return { id, name, Unit::hertz, lo, hi, def, m }; }
constexpr ParamSpec ms(const char* id, const char* name, float lo, float hi, float def) { return { id, name, Unit::milliseconds, lo, hi, def }; }
constexpr ParamSpec db(const char* id, const char* name) { return { id, name, Unit::decibels, -24, 24, 0, true }; }
constexpr ParamSpec tog(const char* id, const char* name, bool def, bool a = true) { return { id, name, Unit::toggle, 0, 1, def ? 1.0f : 0.0f, false, a }; }
constexpr ParamSpec cho(const char* id, const char* name, std::span<const char* const> labels)
{
    return { id, name, Unit::choice, 0, (float) labels.size() - 1, 0, false, true, labels };
}

constexpr ParamSpec table[] = {
    pct("magnitude", "Magnitude", 100), tog("bypass", "Bypass", false), tog("low_latency", "Low Latency", false, false),
    tog("oversampling", "Oversampling", false, false),
    // sub / hiss
    pct("sub_amt", "Sub Amount", 0, true), pct("hiss_amt", "Hiss Amount", 0, true), tog("subhiss_on", "Sub/Hiss On", true),
    pct("sub_sens", "Sub Sensitivity", 50), pct("hiss_sens", "Hiss Sensitivity", 50),
    hz("sub_det_lo", "Sub Detect Low", 20, 20000, 30), hz("sub_det_hi", "Sub Detect High", 20, 20000, 150),
    hz("hiss_det_lo", "Hiss Detect Low", 20, 20000, 1500), hz("hiss_det_hi", "Hiss Detect High", 20, 20000, 8000),
    { "sub_pitch", "Sub Pitch", Unit::note, 24, 60, 31 }, ms("sub_decay", "Sub Decay", 20, 2000, 300), ms("hiss_decay", "Hiss Decay", 20, 2000, 180),
    pct("sub_duck", "Sub Duck", 0), pct("hiss_duck", "Hiss Duck", 0), pct("sub_stereo", "Sub Stereo", 0), pct("sub_color", "Sub Color", 20),
    tog("sub_acid", "Sub Acid", false), cho("hiss_type", "Hiss Type", hissTypes), { "hiss_pitch", "Hiss Pitch", Unit::semitones, -12, 12, 0 },
    sgn("hiss_tone", "Hiss Tone"), pct("hiss_env", "Hiss Env", 30), pct("hiss_res", "Hiss Res", 10), tog("sub_solo", "Sub Solo", false),
    tog("sub_post", "Sub Post", false), tog("hiss_solo", "Hiss Solo", false), tog("hiss_post", "Hiss Post", false),
    // shift
    tog("shift_on", "Shift On", true), sgn("shift_amt", "Shift Amount", 0, true), bip("shift_balance", "Shift Balance"),
    tog("shift_invert", "Shift Invert", false), sgn("shift_stereo", "Shift Stereo"), sgn("shift_stretch", "Shift Stretch"),
    pct("shift_fmix", "Shift Mix", 100), bip("shift_target", "Shift Target"), hz("shift_focus_lo", "Shift Focus Low", 20, 20000, 20),
    hz("shift_focus_hi", "Shift Focus High", 20, 20000, 20000),
    // space
    tog("space_on", "Space On", true), pct("space_amt", "Space Amount", 0, true), cho("space_type", "Space Type", spaceTypes),
    { "space_decay", "Space Decay", Unit::percent, 0, 100, 50, false, false }, sgn("space_stretch", "Space Stretch", 0, false, false),
    ms("space_predelay", "Space Predelay", 0, 500, 0), pct("space_duck", "Space Duck", 0), sgn("space_pan", "Space Pan"),
    sgn("space_width", "Space Width", 100), hz("space_focus_lo", "Space Focus Low", 20, 20000, 150),
    hz("space_focus_hi", "Space Focus High", 20, 20000, 12000),
    // press
    tog("press_on", "Press On", true), pct("press_amt", "Press Amount", 0, true), cho("press_mode", "Press Mode", pressModes),
    bip("press_downup", "Press Down/Up", -1), pct("press_mix", "Press Mix", 100), { "press_bands", "Press Bands", Unit::bipolar, 0, 1, 0 },
    // drive
    tog("drive_on", "Drive On", true), pct("drive_amt", "Drive Amount", 0, true), cho("drive_mode", "Drive Mode", driveModes),
    bip("drive_flavor", "Drive Flavor"), pct("drive_mix", "Drive Mix", 100), bip("drive_target", "Drive Target"),
    hz("drive_focus_lo", "Drive Focus Low", 20, 20000, 20), hz("drive_focus_hi", "Drive Focus High", 20, 20000, 20000),
    // push
    tog("push_on", "Push On", true), pct("push_amt", "Push Amount", 0, true), cho("push_mult", "Push Multiplier", pushMults),
    bip("push_tone", "Push Tone"), pct("push_mix", "Push Mix", 100), bip("push_target", "Push Target"),
    hz("push_focus_lo", "Push Focus Low", 20, 20000, 20), hz("push_focus_hi", "Push Focus High", 20, 20000, 20000),
    // master
    db("in_gain", "Input"), db("out_gain", "Output"), tog("eq_on", "EQ On", true), hz("cut_lo", "Low Cut", 20, 2000, 20, true),
    hz("cut_hi", "High Cut", 1000, 20000, 20000, true), cho("lc_shape", "Low Cut Shape", lowCutShapes),
    cho("hc_shape", "High Cut Shape", highCutShapes), sgn("tone", "Tone", 0, true), cho("tone_shape", "Tone Shape", toneShapes),
    pct("air", "Air", 0, true), sgn("attack", "Attack", 0, true), sgn("sustain", "Sustain", 0, true), tog("clip_on", "Clip", false),
};

String signedText(float v, int decimals, const char* suffix) { return (v > 0.5f * std::pow(10.0f, (float) -decimals) ? "+" : "") + String(v, decimals) + suffix; }

String toText(const ParamSpec& s, float v)
{
    switch (s.unit)
    {
        case Unit::percent: return String(v, 0) + "%";
        case Unit::signedPercent: return signedText(v, 0, "%");
        case Unit::bipolar: return signedText(v, 2, "");
        case Unit::decibels: return signedText(v, 1, " dB");
        case Unit::hertz: return v < 1000 ? String(v, 0) + " Hz" : String(v / 1000, 2) + " kHz";
        case Unit::milliseconds: return v < 1000 ? String(v, 0) + " ms" : String(v / 1000, 2) + " s";
        case Unit::note: return MidiMessage::getMidiNoteName(roundToInt(v), true, true, 3);
        case Unit::semitones: return signedText(v, 1, " st");
        case Unit::toggle:
        case Unit::choice: break;
    }
    return String(v);
}

float fromText(const ParamSpec& s, const String& text)
{
    const auto t = text.trim().toLowerCase();
    const float v = t.getFloatValue();
    if (s.unit == Unit::hertz && t.endsWith("khz"))
        return v * 1000;
    if (s.unit == Unit::milliseconds && t.endsWith("s") && !t.endsWith("ms"))
        return v * 1000;
    return v;
}

NormalisableRange<float> rangeFor(const ParamSpec& s)
{
    if (std::string_view(s.id) == "attack" || std::string_view(s.id) == "sustain")
    {
        // Frozen since 0.1.0: 0.1 steps snapped as round(v * 10) / 10, because JUCE's start + interval * k gets
        // FMA-contracted on some compilers (0 -> 1.5e-6), so defaults and saved values would differ per platform.
        NormalisableRange<float> r(s.lo, s.hi, nullptr, nullptr, [](float a, float b, float v) { return jlimit(a, b, std::round(v * 10) / 10 + 0.0f); });
        r.interval = 0.1f; // still drives the slider step and the displayed decimals
        return r;
    }
    if (s.logScale()) // exact at both ends, so open-range defaults survive a normalised round trip
        return { s.lo, s.hi,
                 [](float a, float b, float x) { return x <= 0 ? a : x >= 1 ? b : a * std::pow(b / a, x); },
                 [](float a, float b, float v) { return v <= a ? 0.0f : v >= b ? 1.0f : std::log(v / a) / std::log(b / a); },
                 [](float a, float b, float v) { return jlimit(a, b, v); } };
    return { s.lo, s.hi, s.unit == Unit::note ? 1.0f : 0.0f };
}
} // namespace

std::span<const ParamSpec> parameterTable() { return table; }

const ParamSpec& spec(std::string_view id)
{
    for (auto& s : table)
        if (id == s.id)
            return s;
    jassertfalse;
    return table[0];
}

AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    for (auto& s : table)
    {
        const ParameterID id { s.id, s.versionHint() };
        if (s.unit == Unit::toggle)
            layout.add(std::make_unique<AudioParameterBool>(id, s.name, s.def > 0.5f, AudioParameterBoolAttributes().withAutomatable(s.automatable)));
        else if (s.unit == Unit::choice)
            layout.add(std::make_unique<AudioParameterChoice>(id, s.name, StringArray(s.choices.data(), (int) s.choices.size()), (int) s.def));
        else
            layout.add(std::make_unique<AudioParameterFloat>(
                id, s.name, rangeFor(s), s.def,
                AudioParameterFloatAttributes()
                    .withAutomatable(s.automatable)
                    .withStringFromValueFunction([&s](float v, int) { return toText(s, v); })
                    .withValueFromStringFunction([&s](const String& t) { return fromText(s, t); })));
    }
    return layout;
}
} // namespace snap
