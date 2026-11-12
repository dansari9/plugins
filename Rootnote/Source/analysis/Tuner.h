#pragma once

#include "Decimator.h"
#include "OnsetDetector.h"
#include "PitchDetector.h"

#include <cstdint>
#include <vector>

namespace rootnote
{
struct TunerReading
{
    bool valid = false;
    double hz = 0.0;             // median of the last four pitched hits
    double lastHitHz = 0.0;      // the newest pitched hit on its own
    double glideSemitones = 0.0; // pitch fall inside the newest hit
    double clarity = 0.0;
    uint32_t pitchedHits = 0;    // hits that produced a pitch
    uint32_t onsets = 0;         // every detected hit
};

/**
    The whole tuner: mono audio in, one reading per drum hit out. No JUCE, no
    threads; AnalysisWorker runs it off the audio thread.

    The input is decimated to about 24 kHz for the onset detector and on to
    about 4 kHz for the pitch detector. A hit is analysed from 15 ms after its
    onset until the next hit starts or 0.36 s have passed.
*/
class Tuner
{
public:
    static constexpr double kSkipSeconds = 0.015;
    static constexpr double kMaxSpanSeconds = 0.360;
    static constexpr double kGuardSeconds = 0.010;   // stop this far before the next hit
    static constexpr int kMedianHits = 4;

    void prepare(double sampleRate);

    /** Forgets everything, including the reading. */
    void reset();

    /** Forgets the audio seen so far (after a gap in the input) but keeps the reading. */
    void resetStream();

    /** Returns true when a new reading is available. */
    bool process(const float* samples, size_t count);

    const TunerReading& reading() const noexcept { return current; }

    double onsetRate() const noexcept { return onsetFs; }
    double pitchRate() const noexcept { return pitchFs; }

    /** Times of all onsets so far, in seconds of input. For tests. */
    const std::vector<double>& onsetTimes() const noexcept { return onsetSeconds; }
    void keepOnsetTimes(bool keep) noexcept { recordOnsets = keep; }

private:
    bool finishHit(int64_t onset, int64_t spanEnd);
    int64_t pitchIndexForOnsetTime(int64_t onsetTime) const noexcept;

    static constexpr int kPitchHistory = 4096;
    static constexpr int kPitchFactor = 6;

    Decimator toOnsetRate;
    Decimator toPitchRate;
    OnsetDetector onsetDetector;
    PitchDetector pitchDetector;

    std::vector<float> onsetBlock;
    std::vector<float> pitchBlock;
    std::vector<int64_t> newOnsets;
    std::vector<float> pitchHistory; // circular
    std::vector<float> segment;
    std::vector<double> onsetSeconds;

    double inputFs = 48000.0;
    double onsetFs = 24000.0;
    double pitchFs = 4000.0;
    int64_t pitchCount = 0;

    bool hasPending = false;
    int64_t pendingOnset = 0;

    double recent[kMedianHits] {};
    int recentCount = 0;
    int recentWrite = 0;

    TunerReading current;
    bool recordOnsets = false;
};
} // namespace rootnote
