#include "ModalRing.h"

#include "SvfBell.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
namespace
{
constexpr double kWeightRampSeconds = 0.020;
constexpr double kMaxModeWeight = 4.0;
constexpr double kDropSettled = 0.002; // semitones
constexpr int kMaxChunk = 64;
} // namespace

void ModalRing::prepare(double sampleRate) noexcept
{
    fs = sampleRate;
    const int multiple = std::max(1, static_cast<int>(std::lround(sampleRate / 48000.0)));
    controlInterval = std::min(kMaxChunk, 16 * multiple);
    weightRampSamples = std::max(1, static_cast<int>(std::lround(kWeightRampSeconds * sampleRate)));

    // A partial of amplitude a that lasts through one gate window leaves its
    // mode ringing at about amplitude a, at any sample rate.
    inputGain = 2.0 / (sampleRate * TransientGate::kEffectiveSeconds);

    gate.prepare(sampleRate);
    pitch.snap(std::log2(65.4));
    reset();
}

void ModalRing::reset() noexcept
{
    silence();
    gate.reset();
    lastGate = 0.0;
}

void ModalRing::silence() noexcept
{
    for (auto& mode : modes)
    {
        mode.re[0] = mode.re[1] = 0.0;
        mode.im[0] = mode.im[1] = 0.0;
        mode.dc = mode.ds = 0.0;
        mode.weight.snap(0.0);
        mode.active = false;
    }
    pitch.snap(pitch.target);
    dropNow = 0.0;
    samplesToTick = 0;
    pendingHit = false;
    snapPending = true;
    dirty = true;
    moving = true;
}

void ModalRing::setSettings(const Settings& newSettings) noexcept
{
    if (newSettings == settings)
        return;
    settings = newSettings;
    settings.decaySeconds = std::max(0.005, settings.decaySeconds);
    settings.shape.rangeOct = std::max(0.01, settings.shape.rangeOct);
    settings.shape.focusHz = std::max(1.0, settings.shape.focusHz);
    dirty = true;
}

void ModalRing::setTargetFrequency(double hz, double rampSeconds) noexcept
{
    const double logHz = std::log2(std::clamp(hz, 1.0, 0.5 * fs));
    if (snapPending)
    {
        pitch.snap(logHz);
        dirty = true;
        return;
    }
    if (logHz == pitch.target)
        return;
    pitch.setTarget(logHz, static_cast<int>(std::lround(std::max(0.0, rampSeconds) * fs)));
    dirty = true;
}

void ModalRing::controlTick() noexcept
{
    const bool snap = snapPending;
    snapPending = false;
    dirty = false;

    if (snap)
        pitch.snap(pitch.target);

    const double rootHz = std::exp2(pitch.advance(controlInterval));

    if (pendingHit)
    {
        dropNow = settings.dropSemitones;
        pendingHit = false;
    }
    const double dropFactor = std::exp2(dropNow / 12.0);
    if (dropNow > 0.0)
    {
        // exp(-4.6) = 1 %: the drop has settled after dropSeconds().
        const double tau = dropSeconds(settings.decaySeconds) / 4.6;
        dropNow *= std::exp(-static_cast<double>(controlInterval) / (tau * fs));
        if (dropNow < kDropSettled)
            dropNow = 0.0;
    }

    // The edge fade uses the pitch that sounds, drop included, so a mode
    // that the drop pushes past the top of the band fades out instead of
    // piling up at the guard frequency.
    const SeriesType series = settings.shape.series;
    const int count = modeCount(series);
    double raw[kMaxModes] {};
    double ratio[kMaxModes] {};
    double sumOfSquares = 0.0;
    for (int m = 0; m < count; ++m)
    {
        ratio[m] = seriesRatio(series, m);
        const double hz = rootHz * ratio[m] * dropFactor;
        raw[m] = std::clamp(boostShapeWeight(settings.shape, m, rootHz) * edgeWeight(hz, fs), 0.0, kMaxModeWeight);
        sumOfSquares += raw[m] * raw[m];
    }
    const double normalise = 1.0 / std::sqrt(std::max(1.0, sumOfSquares));

    bool stillMoving = pitch.isActive() || dropNow > 0.0;
    const double perSample = 1.0 / static_cast<double>(controlInterval);

    for (int m = 0; m < kMaxModes; ++m)
    {
        Mode& mode = modes[m];
        const double weightTarget = raw[m] * normalise;

        if (snap)
            mode.weight.snap(weightTarget);
        else if (std::abs(weightTarget - mode.weight.target) > 1.0e-9)
            mode.weight.setTarget(weightTarget, weightRampSamples);

        const bool wanted = mode.weight.value > 0.0 || mode.weight.isActive();
        if (!wanted)
        {
            if (mode.active)
            {
                mode.active = false;
                mode.re[0] = mode.re[1] = 0.0;
                mode.im[0] = mode.im[1] = 0.0;
            }
            continue;
        }

        const double hz = std::min(rootHz * ratio[m] * dropFactor, kNyquistGuard * fs);
        const double theta = 2.0 * kPi * hz / fs;
        const double t60 = settings.decaySeconds / std::pow(std::max(1.0, ratio[m]), kDamping);
        const double r = std::exp(-6.907755 / (t60 * fs));
        mode.tc = r * std::cos(theta);
        mode.ts = r * std::sin(theta);

        if (snap || !mode.active)
        {
            mode.c = mode.tc;
            mode.s = mode.ts;
            mode.dc = mode.ds = 0.0;
        }
        else
        {
            mode.dc = (mode.tc - mode.c) * perSample;
            mode.ds = (mode.ts - mode.s) * perSample;
            if (std::abs(mode.dc) < 1.0e-18 && std::abs(mode.ds) < 1.0e-18)
            {
                mode.c = mode.tc;
                mode.s = mode.ts;
                mode.dc = mode.ds = 0.0;
            }
            else
            {
                stillMoving = true;
            }
        }
        mode.active = true;

        if (mode.weight.isActive())
            stillMoving = true;
    }

    moving = stillMoving;
}

void ModalRing::listen(const float* inLeft, const float* inRight, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float l = std::abs(inLeft[i]);
        gate.tick(static_cast<double>(inRight != nullptr ? std::max(l, std::abs(inRight[i])) : l));
    }
    gate.takeTrigger();
    lastGate = gate.isOpen() ? 1.0 : 0.0;
}

void ModalRing::process(const float* inLeft, const float* inRight, double* outLeft, double* outRight, int numSamples) noexcept
{
    double excitation[2][kMaxChunk];
    const int channels = (inRight != nullptr && outRight != nullptr) ? 2 : 1;
    double* outputs[2] = { outLeft, outRight };

    int position = 0;
    while (position < numSamples)
    {
        if (samplesToTick <= 0)
        {
            if (moving || dirty || pendingHit || snapPending)
                controlTick();
            samplesToTick = controlInterval;
        }

        const int chunk = std::min(numSamples - position, samplesToTick);

        for (int i = 0; i < chunk; ++i)
        {
            const double l = static_cast<double>(inLeft[position + i]);
            const double r = channels == 2 ? static_cast<double>(inRight[position + i]) : l;
            // The larger channel, not the sum: a hit with opposite polarity left and right is still a hit.
            const double g = gate.tick(std::max(std::abs(l), std::abs(r))) * inputGain;
            excitation[0][i] = g * l;
            excitation[1][i] = g * r;
            outLeft[position + i] = 0.0;
            if (channels == 2)
                outRight[position + i] = 0.0;
        }
        lastGate = gate.isOpen() ? 1.0 : 0.0;
        if (gate.takeTrigger())
        {
            pendingHit = true;
            ++hits;
        }

        for (auto& mode : modes)
        {
            if (!mode.active)
                continue;

            const double weightStart = mode.weight.value;
            const double weightStep = (mode.weight.advance(chunk) - weightStart) / static_cast<double>(chunk);

            double c = mode.c;
            double s = mode.s;
            for (int ch = 0; ch < channels; ++ch)
            {
                c = mode.c;
                s = mode.s;
                double re = mode.re[ch];
                double im = mode.im[ch];
                double weight = weightStart;
                const double* x = excitation[ch];
                double* y = outputs[ch] + position;
                for (int i = 0; i < chunk; ++i)
                {
                    c += mode.dc;
                    s += mode.ds;
                    weight += weightStep;
                    const double nextRe = c * re - s * im + x[i];
                    im = s * re + c * im;
                    re = nextRe;
                    y[i] += weight * re;
                }
                mode.re[ch] = re;
                mode.im[ch] = im;
            }
            mode.c = c;
            mode.s = s;
        }

        samplesToTick -= chunk;
        position += chunk;
    }
}
} // namespace rootnote
