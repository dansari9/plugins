#pragma once

#include "dsp/Engine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

namespace rootnote::ids
{
inline constexpr const char* boostNote = "boost_note";
inline constexpr const char* boostFine = "boost_fine";
inline constexpr const char* boostGain = "boost_gain";
inline constexpr const char* boostQ = "boost_q";
inline constexpr const char* series = "series";
inline constexpr const char* bands = "bands";
inline constexpr const char* blend = "blend";
inline constexpr const char* focusLink = "focus_link";
inline constexpr const char* focus = "focus";
inline constexpr const char* range = "range";
inline constexpr const char* tilt = "tilt";
inline constexpr const char* qMode = "q_mode";
inline constexpr const char* cutOn = "cut_on";
inline constexpr const char* cutNote = "cut_note";
inline constexpr const char* cutDepth = "cut_depth";
inline constexpr const char* cutQ = "cut_q";
inline constexpr const char* cutOvertones = "cut_overtones";
inline constexpr const char* cutBands = "cut_bands";
inline constexpr const char* ring = "ring";
inline constexpr const char* ringDecay = "ring_decay";
inline constexpr const char* ringDrop = "ring_drop";
inline constexpr const char* midiMode = "midi_mode";
inline constexpr const char* glide = "glide";
inline constexpr const char* noteOff = "note_off";
inline constexpr const char* key = "key";
inline constexpr const char* scale = "scale";
inline constexpr const char* keyLock = "key_lock";
inline constexpr const char* a4 = "a4";
inline constexpr const char* mix = "mix";
inline constexpr const char* output = "output";
inline constexpr const char* limiter = "limiter";
inline constexpr const char* bypass = "bypass";
} // namespace rootnote::ids

namespace rootnote
{
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/**
    Reads a typed value of a parameter in its units: "-3.1 dB", "-3.1db", "8",
    "50%", "C#1", "db1", "46 Hz", "+5 ct", "on", "minor". Case, spaces and the
    unit are optional; the typographic minus, sharp and flat are understood.
    Nothing for text that is not a value of the parameter. A note typed as a
    frequency is converted with a4. The parameters' own text conversion (host
    text entry) reads the same, falling back to the default.
*/
std::optional<float> parseValue(const juce::String& id, const juce::String& text, double a4);

/** Reads every parameter into the engine's units. Lock-free; used on the audio thread. */
class ParameterReader
{
public:
    explicit ParameterReader(const juce::AudioProcessorValueTreeState& apvts);

    EngineParams read() const noexcept;

private:
    static constexpr int kCount = 32;
    const std::atomic<float>* raw[kCount] {};
};
} // namespace rootnote
