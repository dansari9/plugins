#pragma once

#include "Fft.h"

#include <cstdint>
#include <vector>

namespace rootnote
{
/**
    Finds the start of each drum hit. Analysis thread only.

    Main detector: spectral flux with high-frequency weighting on a 1024-point
    FFT, hop 256, on a stream of about 24 kHz. A frame is an onset when its
    flux is a local peak above an adaptive threshold (1.5 x the median of the
    last 0.5 s plus a floor). A second detector catches soft, very low kicks
    that have no click: the level of the newest hop against a slow envelope.
    Onsets are at least 60 ms apart. The reported time is refined to the
    steepest rise of the waveform envelope.
*/
class OnsetDetector
{
public:
    static constexpr int kFrameSize = 1024;
    static constexpr int kHop = 256;
    static constexpr double kMinGapSeconds = 0.060;

    OnsetDetector();

    void prepare(double sampleRate);
    void reset();

    /** Appends the onset times, in samples since the last reset, to onsets. */
    void process(const float* in, size_t count, std::vector<int64_t>& onsets);

    int64_t samplesSeen() const noexcept { return total; }

private:
    void analyseHop(std::vector<int64_t>& onsets);
    int64_t refine(int64_t frameEnd) const;
    float sampleAt(int64_t index) const noexcept;

    static constexpr int kHistory = 8192;
    static constexpr int kBins = kFrameSize / 2;

    Fft fft;
    double fs = 24000.0;
    std::vector<float> history;      // circular, indexed by absolute sample
    std::vector<double> window;
    std::vector<double> re, im;
    std::vector<double> previous;    // compressed magnitudes of the last frame
    std::vector<double> fluxHistory; // circular, for the median
    std::vector<double> sortScratch;
    int fluxWrite = 0;
    int fluxCount = 0;
    int medianFrames = 47;

    int64_t total = 0;
    int sinceHop = 0;
    int framesSeen = 0;

    double fluxPrev1 = 0.0;          // flux of frame n-1
    double fluxPrev2 = 0.0;          // flux of frame n-2
    double thresholdPrev1 = 0.0;
    double recentPeak = 0.0;
    double slowDb = -120.0;
    double levelPrevDb = -120.0;
    int64_t lastOnset = -1000000;
};
} // namespace rootnote
