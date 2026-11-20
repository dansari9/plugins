#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

// Original synthetic stand-ins for drum loops, generated in code (no recorded audio): 120 BPM, stereo.
namespace loops
{
inline constexpr const char* names[] = { "kick", "break", "kit", "pad" };

inline float noise(juce::uint32 i) // integer hash as white noise in [-1, 1)
{
    i *= 2654435761u;
    i = (i ^ (i >> 16)) * 0x7feb352du, i = (i ^ (i >> 15)) * 0x846ca68bu, i ^= i >> 16;
    return (float) (i / 2147483648.0 - 1);
}

inline float kick(double t) // pitch-dropping sine with a click
{
    // frequency 45 + 110 * exp(-t / 30 ms) Hz, integrated
    const double phase = 2 * juce::MathConstants<double>::pi * (45 * t + 110 * 0.03 * (1 - std::exp(-t / 0.03)));
    return (float) (0.9 * std::sin(phase) * std::exp(-t / 0.25) + 0.3 * std::exp(-t / 0.002));
}
inline float snare(double t, juce::uint32 i) { return (float) (0.5 * noise(i) * std::exp(-t / 0.09) + 0.4 * std::sin(2 * juce::MathConstants<double>::pi * 190 * t) * std::exp(-t / 0.05)); }
inline float hat(double t, juce::uint32 i) { return (float) (0.2 * (noise(i) - noise(i - 1)) * std::exp(-t / 0.02)); }

// Renders `seconds` of loop `which` (index into names) at `fs`.
inline juce::AudioBuffer<float> make(int which, double fs, double seconds)
{
    const int n = (int) (fs * seconds);
    juce::AudioBuffer<float> b(2, n);
    const double beat = 0.5, sixteenth = beat / 4;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, bar = std::fmod(t, 4 * beat), s = std::fmod(t, sixteenth), q = std::fmod(t, beat);
        const int step = (int) (bar / sixteenth); // 0..15
        float l = 0, r = 0;
        switch (which)
        {
            case 0: l = r = kick(q); break; // four on the floor
            case 1: // syncopated break: kicks on 0, 6, 10; snares on 4, 12, ghost on 15
            {
                const double kickAt = step >= 10 ? 10 : step >= 6 ? 6 : 0, snareAt = step >= 15 ? 15 : step >= 12 ? 12 : step >= 4 ? 4 : -1;
                l = r = kick(bar - kickAt * sixteenth);
                if (snareAt >= 0)
                    l += (snareAt == 15 ? 0.3f : 1.0f) * snare(bar - snareAt * sixteenth, (juce::uint32) i), r = l;
                l += hat(s, (juce::uint32) i * 3u), r += hat(s, (juce::uint32) i * 5u);
                break;
            }
            case 2: // kit: kick, snare on 2 and 4, eighth-note hats panned
            {
                const double half = std::fmod(t, 2 * beat);
                l = r = kick(half) + (half >= beat ? snare(half - beat, (juce::uint32) i) : 0.0f);
                const float h = hat(std::fmod(t, beat / 2), (juce::uint32) i);
                l += 0.8f * h, r += 0.4f * h;
                break;
            }
            default: // pad: slow detuned saw-ish chord, for checking that sustained material passes cleanly
                for (double f : { 110.0, 164.8, 220.0, 277.2 })
                {
                    l += (float) (0.08 * std::sin(2 * juce::MathConstants<double>::pi * f * t) + 0.03 * std::sin(4 * juce::MathConstants<double>::pi * f * t));
                    r += (float) (0.08 * std::sin(2 * juce::MathConstants<double>::pi * f * 1.003 * t));
                }
        }
        b.setSample(0, i, 0.7f * l), b.setSample(1, i, 0.7f * r);
    }
    return b;
}
} // namespace loops
