#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <span>
#include <string_view>

// The full host-facing parameter table. IDs are stable forever: never rename, reuse or remove one.
// "attack" and "sustain" come from 0.1.x (version hint 1); everything else is new in 0.2 (version hint 2).
namespace snap
{
enum class Unit { percent, signedPercent, bipolar, decibels, hertz, milliseconds, note, semitones, toggle, choice };

struct ParamSpec
{
    const char* id;
    const char* name;
    Unit unit;
    float lo, hi, def;
    bool magnitude = false; // Magnitude scales it toward its default (its neutral value)
    bool automatable = true;
    std::span<const char* const> choices {};

    int versionHint() const { return std::string_view(id) == "attack" || std::string_view(id) == "sustain" ? 1 : 2; }
    bool logScale() const { return (unit == Unit::hertz || unit == Unit::milliseconds) && lo > 0; }
};

std::span<const ParamSpec> parameterTable();
const ParamSpec& spec(std::string_view id); // jasserts and returns the first entry for an unknown ID
juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

// Magnitude (0..1) pulls a value toward its neutral one; log-scaled parameters move in the log domain.
inline float applyMagnitude(const ParamSpec& s, float value, float magnitude)
{
    if (!s.magnitude)
        return value;
    if (s.logScale())
        return s.def * std::pow(value / s.def, magnitude);
    return s.def + magnitude * (value - s.def);
}
} // namespace snap
