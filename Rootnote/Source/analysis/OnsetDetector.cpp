#include "OnsetDetector.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
namespace
{
constexpr double kCompression = 100.0;    // log(1 + k |X|), makes the flux level-tolerant
constexpr double kMedianFactor = 1.5;
constexpr double kAbsoluteFloor = 1.5;    // in compressed-flux units
constexpr double kRelativeFloor = 0.08;   // of the largest recent flux
constexpr double kEnergyRiseDb = 4.0;
constexpr double kEnergyFloorDb = -55.0;
constexpr double kSlowReleaseDbPerSecond = 40.0;
} // namespace

OnsetDetector::OnsetDetector()
    : fft(10)
{
    history.assign(kHistory, 0.0f);
    window.resize(kFrameSize);
    for (int i = 0; i < kFrameSize; ++i)
        window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / kFrameSize);
    re.assign(kFrameSize, 0.0);
    im.assign(kFrameSize, 0.0);
    previous.assign(kBins + 1, 0.0);
    fluxHistory.assign(64, 0.0);
    sortScratch.reserve(64);
}

void OnsetDetector::prepare(double sampleRate)
{
    fs = sampleRate;
    medianFrames = std::clamp(static_cast<int>(std::lround(0.5 * sampleRate / kHop)), 8, 64);
    reset();
}

void OnsetDetector::reset()
{
    std::fill(history.begin(), history.end(), 0.0f);
    std::fill(previous.begin(), previous.end(), 0.0);
    std::fill(fluxHistory.begin(), fluxHistory.end(), 0.0);
    fluxWrite = 0;
    fluxCount = 0;
    total = 0;
    sinceHop = 0;
    framesSeen = 0;
    fluxPrev1 = fluxPrev2 = 0.0;
    thresholdPrev1 = 0.0;
    recentPeak = 0.0;
    slowDb = -120.0;
    levelPrevDb = -120.0;
    lastOnset = -1000000;
}

float OnsetDetector::sampleAt(int64_t index) const noexcept
{
    if (index < 0 || index >= total)
        return 0.0f;
    return history[static_cast<size_t>(index & (kHistory - 1))];
}

void OnsetDetector::process(const float* in, size_t count, std::vector<int64_t>& onsets)
{
    for (size_t i = 0; i < count; ++i)
    {
        history[static_cast<size_t>(total & (kHistory - 1))] = in[i];
        ++total;
        if (++sinceHop == kHop)
        {
            sinceHop = 0;
            if (total >= kFrameSize)
                analyseHop(onsets);
        }
    }
}

int64_t OnsetDetector::refine(int64_t frameEnd) const
{
    // Steepest rise of a 1.5 ms envelope inside the last five hops.
    const int64_t searchStart = frameEnd - 5 * kHop;
    const int lag = std::max(1, static_cast<int>(std::lround(0.002 * fs)));
    const int warmUp = 3 * lag;
    const double coefficient = 1.0 - std::exp(-1.0 / (0.0015 * fs));

    const int span = static_cast<int>(frameEnd - searchStart) + warmUp;
    double envelope = 0.0;
    double best = -1.0;
    int64_t bestIndex = frameEnd - 2 * kHop;

    // A short ring of past envelope values gives env[i - lag] without a buffer.
    double ring[256];
    const int ringSize = std::min(lag, 255) + 1;
    for (int i = 0; i < ringSize; ++i)
        ring[i] = 0.0;
    int ringPosition = 0;

    for (int i = 0; i < span; ++i)
    {
        const int64_t index = searchStart - warmUp + i;
        envelope += (std::abs(static_cast<double>(sampleAt(index))) - envelope) * coefficient;
        const double delayed = ring[ringPosition];
        ring[ringPosition] = envelope;
        ringPosition = (ringPosition + 1) % ringSize;

        if (i >= warmUp + ringSize)
        {
            const double rise = envelope - delayed;
            if (rise > best)
            {
                best = rise;
                bestIndex = index;
            }
        }
    }

    return std::max<int64_t>(0, bestIndex - (ringSize - 1));
}

void OnsetDetector::analyseHop(std::vector<int64_t>& onsets)
{
    const int64_t frameStart = total - kFrameSize;
    for (int i = 0; i < kFrameSize; ++i)
    {
        re[static_cast<size_t>(i)] = static_cast<double>(sampleAt(frameStart + i)) * window[static_cast<size_t>(i)];
        im[static_cast<size_t>(i)] = 0.0;
    }
    fft.forward(re.data(), im.data());

    double flux = 0.0;
    for (int k = 1; k <= kBins; ++k)
    {
        const size_t bin = static_cast<size_t>(k);
        const double magnitude = std::sqrt(re[bin] * re[bin] + im[bin] * im[bin]) / (kFrameSize / 4.0);
        const double compressed = std::log1p(kCompression * magnitude);
        const double rise = compressed - previous[bin];
        if (rise > 0.0)
            flux += rise * (0.1 + static_cast<double>(k) / kBins);
        previous[bin] = compressed;
    }
    if (framesSeen == 0)
        flux = 0.0;

    double energy = 0.0;
    for (int i = 0; i < kHop; ++i)
    {
        const double x = static_cast<double>(sampleAt(total - kHop + i));
        energy += x * x;
    }
    const double levelDb = 10.0 * std::log10(energy / kHop + 1.0e-12);

    // Threshold for this frame from the frames before it.
    double median = 0.0;
    if (fluxCount > 0)
    {
        sortScratch.assign(fluxHistory.begin(), fluxHistory.begin() + std::min(fluxCount, medianFrames));
        const auto middle = sortScratch.begin() + static_cast<long>(sortScratch.size() / 2);
        std::nth_element(sortScratch.begin(), middle, sortScratch.end());
        median = *middle;
    }
    recentPeak = std::max(recentPeak * std::exp(-static_cast<double>(kHop) / (3.0 * fs)), flux);
    const double threshold = kMedianFactor * median + std::max(kAbsoluteFloor, kRelativeFloor * recentPeak);

    const int64_t minGap = static_cast<int64_t>(std::lround(kMinGapSeconds * fs));
    const auto report = [&](int64_t frameEnd)
    {
        const int64_t when = refine(frameEnd);
        if (when - lastOnset >= minGap)
        {
            lastOnset = when;
            onsets.push_back(when);
        }
    };

    // Flux peak one frame back: it needs the frame after it to be known.
    if (framesSeen >= 2 && fluxPrev1 > fluxPrev2 && fluxPrev1 >= flux && fluxPrev1 > thresholdPrev1)
        report(total - kHop);

    // Soft low hits: the level of this hop jumps above the slow envelope.
    if (framesSeen >= 1 && levelDb > kEnergyFloorDb && levelDb > levelPrevDb && levelDb - slowDb > kEnergyRiseDb)
        report(total);

    if (levelDb > slowDb)
        slowDb += (levelDb - slowDb) * 0.6;
    else
        slowDb = std::max(levelDb, slowDb - kSlowReleaseDbPerSecond * kHop / fs);
    levelPrevDb = levelDb;

    fluxHistory[static_cast<size_t>(fluxWrite)] = flux;
    fluxWrite = (fluxWrite + 1) % medianFrames;
    fluxCount = std::min(fluxCount + 1, medianFrames);

    fluxPrev2 = fluxPrev1;
    fluxPrev1 = flux;
    thresholdPrev1 = threshold;
    ++framesSeen;
}
} // namespace rootnote
