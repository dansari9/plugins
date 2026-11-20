#pragma once
#include "Constants.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

namespace snap
{
// BS.1770 loudness of the last few seconds (short-term, ungated) for Loudness Match: K-weighting (high shelf + RLB
// high-pass), mean square summed over channels in 100 ms hops. lufs() is safe to read from any thread.
class LoudnessMeter
{
public:
    void prepare(double fs, int channels)
    {
        const double pi = juce::MathConstants<double>::pi;
        double k = std::tan(pi * 1681.974450955533 / fs), q = 0.7071752369554196; // shelf
        const double vh = std::pow(10.0, 3.999843853973347 / 20.0), vb = std::pow(vh, 0.4996667741545416);
        double a0 = 1 + k / q + k * k;
        shelf = { (vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                  2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0 };
        k = std::tan(pi * 38.13547087602444 / fs), q = 0.5003270373238773; // high-pass
        a0 = 1 + k / q + k * k;
        highPass = { 1, -2, 1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0 };
        hopLength = std::max(1, (int) std::lround(fs * constants::loudnessHopSeconds));
        numChannels = std::min(channels, (int) state.size());
        reset();
    }
    void reset()
    {
        state = {}, hops = {}, hopSum = 0, hopFill = 0, hopIndex = 0, hopsFilled = 0;
        loudness.store(silence);
    }

    void process(const juce::AudioBuffer<float>& in, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < std::min(numChannels, in.getNumChannels()); ++c)
            {
                const double y = run(highPass, state[(size_t) c][1], run(shelf, state[(size_t) c][0], in.getSample(c, i)));
                hopSum += y * y;
            }
            if (++hopFill == hopLength)
                endHop();
        }
    }
    float lufs() const { return loudness.load(); }

    static constexpr float silence = -100.0f;

private:
    struct Biquad { double b0, b1, b2, a1, a2; }; // transposed direct form II
    static double run(const Biquad& f, std::array<double, 2>& z, double x)
    {
        const double y = f.b0 * x + z[0];
        z[0] = f.b1 * x - f.a1 * y + z[1], z[1] = f.b2 * x - f.a2 * y;
        return y;
    }
    void endHop()
    {
        hops[(size_t) hopIndex] = hopSum, hopIndex = (hopIndex + 1) % numHops, hopsFilled = std::min(hopsFilled + 1, numHops);
        hopSum = 0, hopFill = 0;
        double sum = 0;
        for (double h : hops)
            sum += h;
        const double meanSquare = sum / ((double) hopsFilled * hopLength);
        loudness.store(meanSquare > 1.0e-10 ? (float) (-0.691 + 10.0 * std::log10(meanSquare)) : silence);
    }

    static constexpr int numHops = (int) (constants::loudnessWindowSeconds / constants::loudnessHopSeconds + 0.5);
    Biquad shelf {}, highPass {};
    std::array<std::array<std::array<double, 2>, 2>, 2> state {}; // [channel][filter][z]
    std::array<double, numHops> hops {};
    double hopSum = 0;
    int hopLength = 4800, hopFill = 0, hopIndex = 0, hopsFilled = 0, numChannels = 2;
    std::atomic<float> loudness { silence };
};
} // namespace snap
