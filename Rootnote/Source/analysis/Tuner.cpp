#include "Tuner.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
namespace
{
constexpr size_t kChunk = 2048;
constexpr double kDetectorLagSeconds = 0.080; // an onset is reported at most this long after it happened
} // namespace

void Tuner::prepare(double sampleRate)
{
    inputFs = sampleRate;
    const int factor = std::max(1, static_cast<int>(std::lround(sampleRate / 24000.0)));
    onsetFs = sampleRate / static_cast<double>(factor);
    pitchFs = onsetFs / static_cast<double>(kPitchFactor);

    toOnsetRate.prepare(factor);
    toPitchRate.prepare(kPitchFactor);
    onsetDetector.prepare(onsetFs);

    pitchHistory.assign(kPitchHistory, 0.0f);
    onsetBlock.reserve(kChunk);
    pitchBlock.reserve(kChunk);
    newOnsets.reserve(16);
    segment.reserve(kPitchHistory);
    reset();
}

void Tuner::resetStream()
{
    toOnsetRate.reset();
    toPitchRate.reset();
    onsetDetector.reset();
    std::fill(pitchHistory.begin(), pitchHistory.end(), 0.0f);
    pitchCount = 0;
    hasPending = false;
    pendingOnset = 0;
}

void Tuner::reset()
{
    resetStream();
    recentCount = 0;
    recentWrite = 0;
    current = {};
    onsetSeconds.clear();
}

int64_t Tuner::pitchIndexForOnsetTime(int64_t onsetTime) const noexcept
{
    // Pitch sample j is produced from onset-rate input (j + 1) * factor - 1
    // and stands for the time that many samples minus the filter delay.
    const int64_t shifted = onsetTime + toPitchRate.delaySamples() + 1;
    return (shifted + kPitchFactor - 1) / kPitchFactor - 1;
}

bool Tuner::finishHit(int64_t onset, int64_t spanEnd)
{
    const int64_t first = std::max<int64_t>(0, pitchIndexForOnsetTime(onset + static_cast<int64_t>(std::lround(kSkipSeconds * onsetFs))));
    const int64_t last = std::min(pitchCount, pitchIndexForOnsetTime(spanEnd));
    const int64_t count = last - first;
    if (count <= 0 || pitchCount - first > kPitchHistory)
        return false;

    segment.resize(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i)
        segment[static_cast<size_t>(i)] = pitchHistory[static_cast<size_t>((first + i) & (kPitchHistory - 1))];

    const PitchResult result = pitchDetector.analyse(segment.data(), static_cast<int>(count), pitchFs);
    if (!result.valid)
        return false;

    recent[recentWrite] = std::log2(result.hz);
    recentWrite = (recentWrite + 1) % kMedianHits;
    recentCount = std::min(recentCount + 1, kMedianHits);

    double sorted[kMedianHits];
    for (int i = 0; i < recentCount; ++i)
    {
        int at = i;
        for (; at > 0 && sorted[at - 1] > recent[i]; --at)
            sorted[at] = sorted[at - 1];
        sorted[at] = recent[i];
    }
    const double median = (recentCount % 2) == 1 ? sorted[recentCount / 2]
                                                 : 0.5 * (sorted[recentCount / 2 - 1] + sorted[recentCount / 2]);

    current.valid = true;
    current.hz = std::exp2(median);
    current.lastHitHz = result.hz;
    current.glideSemitones = result.glideSemitones;
    current.clarity = result.clarity;
    ++current.pitchedHits;
    return true;
}

bool Tuner::process(const float* samples, size_t count)
{
    bool updated = false;

    for (size_t offset = 0; offset < count; offset += kChunk)
    {
        const size_t chunk = std::min(kChunk, count - offset);

        onsetBlock.clear();
        toOnsetRate.process(samples + offset, chunk, onsetBlock);
        if (onsetBlock.empty())
            continue;

        newOnsets.clear();
        onsetDetector.process(onsetBlock.data(), onsetBlock.size(), newOnsets);

        pitchBlock.clear();
        toPitchRate.process(onsetBlock.data(), onsetBlock.size(), pitchBlock);
        for (float sample : pitchBlock)
        {
            pitchHistory[static_cast<size_t>(pitchCount & (kPitchHistory - 1))] = sample;
            ++pitchCount;
        }

        for (int64_t onset : newOnsets)
        {
            ++current.onsets;
            if (recordOnsets)
            {
                const double factor = static_cast<double>(toOnsetRate.getFactor());
                onsetSeconds.push_back((static_cast<double>(onset + 1) * factor - 1.0 - toOnsetRate.delaySamples()) / inputFs);
            }

            if (hasPending)
                updated = finishHit(pendingOnset, onset - static_cast<int64_t>(std::lround(kGuardSeconds * onsetFs))) || updated;
            pendingOnset = onset;
            hasPending = true;
        }

        if (hasPending)
        {
            const int64_t fullEnd = pendingOnset + static_cast<int64_t>(std::lround((kSkipSeconds + kMaxSpanSeconds) * onsetFs));
            const int64_t settledBy = fullEnd + static_cast<int64_t>(std::lround(kDetectorLagSeconds * onsetFs));
            if (onsetDetector.samplesSeen() >= settledBy)
            {
                updated = finishHit(pendingOnset, fullEnd) || updated;
                hasPending = false;
            }
        }
    }

    return updated;
}
} // namespace rootnote
