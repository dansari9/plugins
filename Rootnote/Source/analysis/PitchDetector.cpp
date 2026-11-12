#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
namespace
{
constexpr double kPiD = 3.14159265358979323846;
constexpr double kMinTonality = 6.0;      // spectral peak over mean magnitude; noise sits near 2..3
constexpr double kUsableFloor = 0.0316;   // -30 dB under the loudest 10 ms block
constexpr double kMpmPeakThreshold = 0.9; // McLeod's "k"

double semitones(double st) noexcept
{
    return std::exp2(st / 12.0);
}
} // namespace

PitchDetector::PitchDetector()
    : coarseFft(14), frameFft(12)
{
    re.assign(coarseFft.length(), 0.0);
    im.assign(coarseFft.length(), 0.0);
    magnitude.assign(coarseFft.length() / 2 + 1, 0.0);
    segment.reserve(4096);
    nsdf.reserve(512);
}

bool PitchDetector::mpm(const double* x, int count, double sampleRate, double& hz, double& clarity)
{
    const int tauMin = std::max(2, static_cast<int>(std::floor(sampleRate / kMaxHz)));
    const int tauMax = std::min(count / 2, static_cast<int>(std::ceil(sampleRate / kMinHz)) + 2);
    if (tauMax <= tauMin + 2)
        return false;

    nsdf.assign(static_cast<size_t>(tauMax + 2), 0.0);
    for (int tau = 0; tau <= tauMax + 1; ++tau)
    {
        double correlation = 0.0;
        double energy = 0.0;
        const int limit = count - tau;
        for (int j = 0; j < limit; ++j)
        {
            correlation += x[j] * x[j + tau];
            energy += x[j] * x[j] + x[j + tau] * x[j + tau];
        }
        nsdf[static_cast<size_t>(tau)] = energy > 0.0 ? 2.0 * correlation / energy : 0.0;
    }

    // Key maxima: the highest point of each positive lobe after the first zero crossing.
    int tau = 1;
    while (tau <= tauMax && nsdf[static_cast<size_t>(tau)] > 0.0)
        ++tau;

    int candidateTau[64];
    double candidateValue[64];
    int candidates = 0;
    double highest = 0.0;

    while (tau <= tauMax && candidates < 64)
    {
        if (nsdf[static_cast<size_t>(tau)] <= 0.0)
        {
            ++tau;
            continue;
        }
        int bestTau = tau;
        double best = nsdf[static_cast<size_t>(tau)];
        while (tau <= tauMax && nsdf[static_cast<size_t>(tau)] > 0.0)
        {
            if (nsdf[static_cast<size_t>(tau)] > best)
            {
                best = nsdf[static_cast<size_t>(tau)];
                bestTau = tau;
            }
            ++tau;
        }
        // A lobe cut off by the search limit has no true maximum yet.
        if (bestTau >= tauMax || bestTau < tauMin)
            continue;
        candidateTau[candidates] = bestTau;
        candidateValue[candidates] = best;
        ++candidates;
        highest = std::max(highest, best);
    }

    if (candidates == 0)
        return false;

    for (int i = 0; i < candidates; ++i)
    {
        if (candidateValue[i] < kMpmPeakThreshold * highest)
            continue;

        const size_t t = static_cast<size_t>(candidateTau[i]);
        const double a = nsdf[t - 1];
        const double b = nsdf[t];
        const double c = nsdf[t + 1];
        const double curvature = a - 2.0 * b + c;
        const double shift = curvature < 0.0 ? 0.5 * (a - c) / curvature : 0.0;
        const double refinedTau = static_cast<double>(candidateTau[i]) + std::clamp(shift, -1.0, 1.0);
        hz = sampleRate / refinedTau;
        clarity = std::clamp(b - 0.25 * (a - c) * shift, 0.0, 1.0);
        return true;
    }
    return false;
}

void PitchDetector::spectrum(const Fft& fft, const double* x, int count)
{
    const int length = static_cast<int>(fft.length());
    const int used = std::min(count, length);
    for (int i = 0; i < used; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPiD * (static_cast<double>(i) + 0.5) / static_cast<double>(used));
        re[static_cast<size_t>(i)] = x[i] * window;
    }
    std::fill(re.begin() + used, re.begin() + length, 0.0);
    std::fill(im.begin(), im.begin() + length, 0.0);
    fft.forward(re.data(), im.data());
    for (int k = 0; k <= length / 2; ++k)
    {
        const size_t bin = static_cast<size_t>(k);
        magnitude[bin] = std::sqrt(re[bin] * re[bin] + im[bin] * im[bin]);
    }
}

PitchDetector::Peak PitchDetector::peakBetween(const Fft& fft, double sampleRate, double loHz, double hiHz) const
{
    const int length = static_cast<int>(fft.length());
    const double binHz = sampleRate / static_cast<double>(length);
    const int lo = std::max(1, static_cast<int>(std::ceil(loHz / binHz)));
    const int hi = std::min(length / 2 - 1, static_cast<int>(std::floor(hiHz / binHz)));

    Peak peak;
    if (hi <= lo)
        return peak;

    int best = lo;
    for (int k = lo + 1; k <= hi; ++k)
        if (magnitude[static_cast<size_t>(k)] > magnitude[static_cast<size_t>(best)])
            best = k;

    const size_t b = static_cast<size_t>(best);
    peak.magnitude = magnitude[b];
    peak.atEdge = magnitude[b - 1] > magnitude[b] || magnitude[b + 1] > magnitude[b];

    double shift = 0.0;
    if (!peak.atEdge && magnitude[b] > 0.0)
    {
        const double left = std::log(magnitude[b - 1] + 1.0e-300);
        const double centre = std::log(magnitude[b] + 1.0e-300);
        const double right = std::log(magnitude[b + 1] + 1.0e-300);
        const double curvature = left - 2.0 * centre + right;
        if (curvature < 0.0)
            shift = std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5);
    }
    peak.hz = (static_cast<double>(best) + shift) * binHz;
    return peak;
}

PitchDetector::Peak PitchDetector::lowestStrongPeak(const Fft& fft, double sampleRate, double& tonality) const
{
    const int length = static_cast<int>(fft.length());
    const double binHz = sampleRate / static_cast<double>(length);
    const int lo = std::max(2, static_cast<int>(std::ceil(kMinHz / binHz)));
    const int hi = std::min(length / 2 - 2, static_cast<int>(std::floor(kMaxHz / binHz)));

    Peak peak;
    tonality = 0.0;
    if (hi <= lo)
        return peak;

    double highest = 0.0;
    double sum = 0.0;
    for (int k = lo; k <= hi; ++k)
    {
        highest = std::max(highest, magnitude[static_cast<size_t>(k)]);
        sum += magnitude[static_cast<size_t>(k)];
    }
    if (highest <= 0.0)
        return peak;
    tonality = highest / (sum / static_cast<double>(hi - lo + 1));

    // The fundamental of a drum is its lowest strong partial, not always its loudest.
    for (int k = lo; k <= hi; ++k)
    {
        const size_t b = static_cast<size_t>(k);
        if (magnitude[b] >= 0.5 * highest && magnitude[b] >= magnitude[b - 1] && magnitude[b] > magnitude[b + 1])
            return peakBetween(fft, sampleRate, (k - 1) * binHz, (k + 1) * binHz);
    }
    return peakBetween(fft, sampleRate, kMinHz, kMaxHz);
}

PitchResult PitchDetector::analyse(const float* samples, int count, double sampleRate)
{
    PitchResult result;
    if (samples == nullptr || count < static_cast<int>(0.040 * sampleRate))
        return result;

    segment.resize(static_cast<size_t>(count));
    double mean = 0.0;
    for (int i = 0; i < count; ++i)
        mean += static_cast<double>(samples[i]);
    mean /= static_cast<double>(count);

    double loudest = 0.0;
    for (int i = 0; i < count; ++i)
    {
        segment[static_cast<size_t>(i)] = static_cast<double>(samples[i]) - mean;
        loudest = std::max(loudest, std::abs(segment[static_cast<size_t>(i)]));
    }
    if (loudest < 1.0e-5)
        return result;

    const double* x = segment.data();

    // 1. Coarse estimate over the opening window.
    const int coarseCount = std::min(count, static_cast<int>(std::lround(kCoarseSeconds * sampleRate)));
    double mpmHz = 0.0;
    double mpmClarity = 0.0;
    const bool mpmFound = mpm(x, coarseCount, sampleRate, mpmHz, mpmClarity);

    spectrum(coarseFft, x, coarseCount);
    double tonality = 0.0;
    const Peak spectral = lowestStrongPeak(coarseFft, sampleRate, tonality);

    double coarse = 0.0;
    if (mpmFound && mpmClarity > kMpmClarity && mpmHz >= kMinHz && mpmHz <= kMaxHz)
    {
        coarse = mpmHz;
        result.usedMpm = true;
        result.clarity = mpmClarity;
    }
    else if (tonality >= kMinTonality && spectral.hz >= kMinHz && spectral.hz <= kMaxHz)
    {
        coarse = spectral.hz;
        result.clarity = std::clamp(std::max(mpmFound ? mpmClarity : 0.0, tonality / 30.0), 0.0, kMpmClarity);
    }
    else
    {
        return result;
    }

    result.valid = true;
    result.coarseHz = coarse;
    result.hz = coarse;

    // 2. How much of the hit is still above the noise.
    const int block = std::max(1, static_cast<int>(std::lround(0.010 * sampleRate)));
    const int blocks = count / block;
    double loudestBlock = 0.0;
    int lastGood = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int b = 0; b < blocks; ++b)
        {
            double energy = 0.0;
            for (int i = 0; i < block; ++i)
                energy += x[b * block + i] * x[b * block + i];
            const double rms = std::sqrt(energy / block);
            if (pass == 0)
                loudestBlock = std::max(loudestBlock, rms);
            else if (rms >= kUsableFloor * loudestBlock)
                lastGood = b;
        }
    }
    const int usable = std::min(count, (lastGood + 1) * block);

    // 3. Frames, followed from the settled end back to the attack.
    const double period = sampleRate / coarse;
    int frameLength = std::clamp(static_cast<int>(std::lround(3.5 * period)),
                                 static_cast<int>(std::lround(0.040 * sampleRate)),
                                 static_cast<int>(std::lround(0.120 * sampleRate)));
    frameLength = std::min({ frameLength, usable, static_cast<int>(frameFft.length()) });
    if (static_cast<double>(frameLength) < 2.0 * period)
        return result;

    int starts[kMaxFrames];
    int frames = 0;
    {
        const int lastStart = usable - frameLength;
        const int hop = std::max(1, frameLength / 2);
        const int available = lastStart / hop + (lastStart % hop != 0 ? 2 : 1);
        frames = std::min(available, kMaxFrames);
        for (int i = 0; i < frames; ++i)
            starts[i] = frames == 1 ? lastStart
                                    : static_cast<int>(std::lround(static_cast<double>(lastStart) * i / (frames - 1)));
    }

    const double topHz = std::min(1000.0, 0.45 * sampleRate);

    spectrum(frameFft, x + starts[frames - 1], frameLength);
    Peak settled = peakBetween(frameFft, sampleRate, coarse * semitones(-4.0), coarse * semitones(4.0));
    if (settled.atEdge)
        settled = peakBetween(frameFft, sampleRate, coarse * semitones(-7.0), coarse * semitones(7.0));
    const Peak strongest = peakBetween(frameFft, sampleRate, kMinHz, topHz);
    if (settled.atEdge || settled.magnitude < 0.05 * strongest.magnitude)
        return result; // no clear partial at the fundamental: keep the coarse estimate

    result.hz = settled.hz;
    result.frames = frames;

    double following = settled.hz;
    for (int i = frames - 2; i >= 0; --i)
    {
        spectrum(frameFft, x + starts[i], frameLength);
        const Peak earlier = peakBetween(frameFft, sampleRate, following * semitones(-1.5), following * semitones(7.0));
        if (earlier.magnitude <= 0.0)
            break;
        following = earlier.hz;
    }
    result.glideSemitones = 12.0 * std::log2(following / settled.hz);
    return result;
}
} // namespace rootnote
