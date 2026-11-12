#pragma once

#include "Fft.h"

#include <vector>

namespace rootnote
{
struct PitchResult
{
    bool valid = false;
    double hz = 0.0;             // settled pitch: where the hit ends up
    double coarseHz = 0.0;       // first estimate over the opening 120 ms
    double glideSemitones = 0.0; // how far the pitch fell from the first frame to the last
    double clarity = 0.0;        // 0..1, how tonal the hit is
    bool usedMpm = false;
    int frames = 0;
};

/**
    Fundamental of one drum hit, 25..500 Hz. Analysis thread only.

    The segment starts 15 ms after the onset (past the click) and runs for as
    long as the hit is free of the next one, up to about 0.36 s.

    1. Coarse: McLeod pitch method (normalised square difference) over the
       first 120 ms, and the peak of a Hann-windowed 16k FFT of the same
       window with parabolic interpolation. MPM is used when its clarity is
       above 0.8, the FFT peak otherwise.
    2. Settled pitch: kicks glide down, so the hit is cut into overlapping
       frames (at least 3.5 periods long) and the spectral peak near the
       coarse estimate is followed from the last frame backwards. The last
       frame that is still above the noise gives the settled pitch; the first
       frame gives the glide. The peak of a windowed decaying sine sits on its
       true frequency, so the decay does not bias the result.
*/
class PitchDetector
{
public:
    static constexpr double kMinHz = 25.0;
    static constexpr double kMaxHz = 500.0;
    static constexpr double kCoarseSeconds = 0.120;
    static constexpr double kMpmClarity = 0.8;
    static constexpr int kMaxFrames = 8;

    PitchDetector();

    PitchResult analyse(const float* samples, int count, double sampleRate);

private:
    struct Peak
    {
        double hz = 0.0;
        double magnitude = 0.0;
        bool atEdge = false;
    };

    bool mpm(const double* x, int count, double sampleRate, double& hz, double& clarity);
    void spectrum(const Fft& fft, const double* x, int count);
    Peak peakBetween(const Fft& fft, double sampleRate, double loHz, double hiHz) const;
    Peak lowestStrongPeak(const Fft& fft, double sampleRate, double& tonality) const;

    Fft coarseFft; // 16384
    Fft frameFft;  // 4096
    std::vector<double> segment;
    std::vector<double> nsdf;
    std::vector<double> re, im, magnitude;
};
} // namespace rootnote
