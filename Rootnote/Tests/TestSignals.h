#pragma once

// Signal generators and measurements shared by the tests. Everything is
// deterministic: the random source is a fixed linear congruential generator,
// so results are identical on every platform and standard library.

#include "dsp/Series.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace testsignals
{
constexpr double kTwoPi = 6.28318530717958647692;

class Random
{
public:
    explicit Random(uint32_t seed = 1) : state(seed) {}

    /** Uniform in [0, 1). */
    double uniform() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / 16777216.0;
    }

    /** Uniform in [-1, 1). */
    double bipolar() noexcept { return 2.0 * uniform() - 1.0; }

private:
    uint32_t state;
};

inline double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) noexcept { return 20.0 * std::log10(std::max(gain, 1.0e-30)); }

/** Amplitude of the component at hz in x[from, to), by correlation with a sine and a cosine. */
inline double toneAmplitude(const std::vector<double>& x, size_t from, size_t to, double hz, double sampleRate)
{
    double re = 0.0, im = 0.0;
    for (size_t i = from; i < to; ++i)
    {
        const double phase = kTwoPi * hz * static_cast<double>(i) / sampleRate;
        re += x[i] * std::cos(phase);
        im += x[i] * std::sin(phase);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

/**
    Steady-state gain in dB of a mono process at one frequency. The filter is
    given time to settle (several ring times), then a whole number of cycles
    is measured.
*/
inline double measureGainDb(const std::function<double(double)>& process, double hz, double sampleRate,
                            double settleSeconds, double amplitude = 0.25)
{
    const size_t settle = static_cast<size_t>(settleSeconds * sampleRate);
    const double cycles = std::max(8.0, std::ceil(hz * 0.05));
    const size_t measure = static_cast<size_t>(std::llround(cycles * sampleRate / hz));

    std::vector<double> out(settle + measure);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = process(amplitude * std::sin(kTwoPi * hz * static_cast<double>(i) / sampleRate));

    return gainToDb(toneAmplitude(out, settle, settle + measure, hz, sampleRate) / amplitude);
}

struct KickSpec
{
    double hz = 50.0;           // settled pitch
    double dropSemitones = 6.0; // how far above the settled pitch the hit starts
    double dropTau = 0.015;     // time constant of the pitch fall, seconds
    double decayTau = 0.20;     // time constant of the amplitude decay, seconds
    double click = 0.3;         // level of the noise click
    double level = 0.8;
    double attack = 0.0005;     // fade-in, seconds
};

/** Sine with an exponential pitch drop, an exponential decay and a short noise click. */
inline void addKick(std::vector<float>& out, size_t start, const KickSpec& spec, double sampleRate, Random& random)
{
    double phase = 0.0;
    const size_t length = static_cast<size_t>(std::min(8.0 * spec.decayTau, 2.5) * sampleRate);
    for (size_t i = 0; i < length && start + i < out.size(); ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        const double hz = spec.hz * std::exp2(spec.dropSemitones / 12.0 * std::exp(-t / spec.dropTau));
        phase += kTwoPi * hz / sampleRate;
        const double fadeIn = spec.attack > 0.0 ? std::min(1.0, t / spec.attack) : 1.0;
        double sample = std::sin(phase) * std::exp(-t / spec.decayTau) * fadeIn;
        sample += spec.click * random.bipolar() * std::exp(-t / 0.002);
        out[start + i] += static_cast<float>(spec.level * sample);
    }
}

struct MembraneSpec
{
    double hz = 110.0;      // fundamental, mode (0,1)
    int modes = 8;
    double decayTau = 0.18; // of the fundamental; higher modes decay faster
    double noise = 0.0;     // snare wires: level of a band-limited noise burst
    double noiseTau = 0.06;
    double click = 0.2;
    double level = 0.7;
};

/** Tom or snare: ideal-membrane mode ratios with decaying sines, optional noise. */
inline void addMembrane(std::vector<float>& out, size_t start, const MembraneSpec& spec, double sampleRate, Random& random)
{
    const size_t length = static_cast<size_t>(std::min(8.0 * spec.decayTau, 2.5) * sampleRate);
    double phases[rootnote::kMaxSlots] {};
    double highPassState = 0.0;
    double previousNoise = 0.0;
    for (size_t i = 0; i < length && start + i < out.size(); ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        double sample = 0.0;
        for (int m = 0; m < spec.modes; ++m)
        {
            const double ratio = rootnote::kDrumRatios[m];
            if (spec.hz * ratio > 0.4 * sampleRate)
                break;
            phases[m] += kTwoPi * spec.hz * ratio / sampleRate;
            const double amplitude = 1.0 / std::pow(ratio, 1.2);
            sample += amplitude * std::sin(phases[m]) * std::exp(-t * std::pow(ratio, 0.8) / spec.decayTau);
        }

        // One-pole high-pass on the noise keeps it out of the fundamental's range.
        const double white = random.bipolar();
        highPassState = 0.9 * (highPassState + white - previousNoise);
        previousNoise = white;
        sample += spec.noise * highPassState * std::exp(-t / spec.noiseTau);
        sample += spec.click * white * std::exp(-t / 0.002);

        out[start + i] += static_cast<float>(spec.level * sample);
    }
}
} // namespace testsignals
