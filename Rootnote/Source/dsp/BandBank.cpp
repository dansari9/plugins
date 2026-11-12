#include "BandBank.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
namespace
{
constexpr double kGainRampSeconds = 0.020;
constexpr double kQRampSeconds = 0.030;
constexpr double kGainEpsilonDb = 1.0e-9;

inline void tickStereo(const BellCoeffs& c, BellState& l, BellState& r, double& xl, double& xr) noexcept
{
    xl = bellTick(c, l, xl);
    xr = bellTick(c, r, xr);
}
} // namespace

void BandBank::prepare(double sampleRate, Kind bankKind) noexcept
{
    fs = sampleRate;
    kind = bankKind;

    // About 3 kHz control rate at every sample rate: 16 samples up to 48 kHz,
    // 32 at 88.2/96 kHz, 64 at 176.4/192 kHz.
    const int multiple = std::max(1, static_cast<int>(std::lround(sampleRate / 48000.0)));
    controlInterval = 16 * multiple;
    gainRampSamples = std::max(1, static_cast<int>(std::lround(kGainRampSeconds * sampleRate)));
    qRampSamples = std::max(1, static_cast<int>(std::lround(kQRampSeconds * sampleRate)));

    pitch.snap(std::log2(65.4));
    logQ.snap(std::log2(settings.q));
    reset();
}

void BandBank::reset() noexcept
{
    for (auto& slot : slots)
    {
        slot.state[0] = {};
        slot.state[1] = {};
        slot.coeffs = {};
        slot.target = {};
        slot.delta = { 0.0, 0.0, 0.0, 0.0 };
        slot.gain.snap(0.0);
        slot.active = false;
        slot.ramping = false;
        slot.retire = false;
        slot.gHz = -1.0;
    }
    numActive = 0;
    pitch.snap(pitch.target);
    logQ.snap(std::log2(settings.q));
    samplesToTick = 0;
    midPeriod = false;
    moving = true;
    snapPending = true;
}

void BandBank::markDirty() noexcept
{
    if (moving && samplesToTick > 0)
    {
        // The smoothers already sit at the end of the period that is being cut
        // short, so the next control point only moves them by the part of that
        // period that really elapsed.
        midPeriod = true;
        elapsedInPeriod = controlInterval - samplesToTick;
    }
    moving = true;
    samplesToTick = 0;
}

void BandBank::setSettings(const Settings& newSettings) noexcept
{
    if (newSettings == settings)
        return;

    settings = newSettings;
    settings.q = std::max(0.05, settings.q);
    settings.rangeOct = std::max(0.01, settings.rangeOct);
    settings.focusHz = std::max(1.0, settings.focusHz);

    shape = settings.shape();

    const double q = std::log2(settings.q);
    if (snapPending)
        logQ.snap(q);
    else if (q != logQ.target)
        logQ.setTarget(q, qRampSamples);

    markDirty();
}

void BandBank::setTargetFrequency(double hz, double rampSeconds) noexcept
{
    const double logHz = std::log2(std::clamp(hz, 1.0, 0.5 * fs));
    if (snapPending)
    {
        pitch.snap(logHz);
        markDirty();
        return;
    }

    if (logHz == pitch.target)
        return;

    pitch.setTarget(logHz, static_cast<int>(std::lround(std::max(0.0, rampSeconds) * fs)));
    markDirty();
}

double BandBank::currentRootHz() const noexcept
{
    return std::exp2(pitch.value);
}

double BandBank::targetRootHz() const noexcept
{
    return std::exp2(pitch.target);
}

double BandBank::targetGainFor(int slot, double rootHz) const noexcept
{
    if (kind == Kind::boost)
        return boostSlotGainDb(shape, settings.gainDb, slot, rootHz, fs);
    return cutSlotGainDb(settings.series, settings.bands, settings.gainDb, settings.overtones, slot, rootHz, fs);
}

void BandBank::controlTick() noexcept
{
    const bool snap = snapPending;
    const bool periodCompleted = !midPeriod;
    const int advance = midPeriod ? elapsedInPeriod : controlInterval;
    snapPending = false;
    midPeriod = false;

    if (snap)
    {
        pitch.snap(pitch.target);
        logQ.snap(std::log2(settings.q));
    }

    const double rootHz = std::exp2(pitch.advance(advance));
    const double q = std::exp2(logQ.advance(advance));
    bool stillMoving = pitch.isActive() || logQ.isActive();

    const int slotCount = seriesSlotCount(settings.series);
    const double perSample = 1.0 / static_cast<double>(controlInterval);
    numActive = 0;

    for (int i = 0; i < kMaxSlots; ++i)
    {
        Slot& slot = slots[i];

        if (periodCompleted && slot.active)
        {
            slot.coeffs = slot.target;
            if (slot.retire)
            {
                slot.active = false;
                slot.state[0] = {};
                slot.state[1] = {};
            }
        }
        slot.retire = false;
        slot.ramping = false;

        const bool addressable = i < slotCount;
        const double wantedRatio = addressable ? seriesRatio(settings.series, i) : slot.ratio;
        double wantedGain = addressable ? targetGainFor(i, rootHz) : 0.0;

        if (slot.ratio != wantedRatio)
        {
            // A band only changes its ratio while it is at 0 dB, where it is
            // inaudible. Until then it fades out first.
            if (snap || (!slot.active && slot.gain.value == 0.0))
            {
                slot.ratio = wantedRatio;
            }
            else
            {
                wantedGain = 0.0;
                stillMoving = true;
            }
        }

        if (snap)
            slot.gain.snap(wantedGain);
        else if (std::abs(wantedGain - slot.gain.target) > kGainEpsilonDb)
            slot.gain.setTarget(wantedGain, gainRampSamples);

        const double gainDb = slot.gain.advance(advance);
        if (slot.gain.isActive())
            stillMoving = true;

        const bool wasActive = slot.active;
        if (!wasActive && gainDb == 0.0)
            continue;

        const double hz = std::clamp(rootHz * slot.ratio, 1.0, 0.49 * fs);
        if (hz != slot.gHz)
        {
            slot.g = bellPrewarp(hz, fs);
            slot.gHz = hz;
        }
        slot.target = makeBellCoeffs(slot.g, slotQ(settings.qMode, q, slot.ratio), gainDb);

        if (snap)
        {
            slot.coeffs = slot.target;
        }
        else if (!wasActive)
        {
            // With m1 = 0 the bell passes its input unchanged whatever a1..a3
            // are, so a new band starts there and only m1 is ramped in.
            slot.coeffs = slot.target;
            slot.coeffs.m1 = 0.0;
            slot.state[0] = {};
            slot.state[1] = {};
        }

        slot.delta.a1 = (slot.target.a1 - slot.coeffs.a1) * perSample;
        slot.delta.a2 = (slot.target.a2 - slot.coeffs.a2) * perSample;
        slot.delta.a3 = (slot.target.a3 - slot.coeffs.a3) * perSample;
        slot.delta.m1 = (slot.target.m1 - slot.coeffs.m1) * perSample;
        slot.ramping = slot.delta.a1 != 0.0 || slot.delta.a2 != 0.0 || slot.delta.a3 != 0.0 || slot.delta.m1 != 0.0;
        slot.active = true;

        if (slot.ramping)
            stillMoving = true;

        if (gainDb == 0.0 && !slot.gain.isActive())
        {
            slot.retire = true;
            stillMoving = true;
        }

        activeSlots[numActive++] = i;
    }

    moving = stillMoving;
}

void BandBank::runSlot(Slot& slot, double* left, double* right, int numSamples) noexcept
{
    BellCoeffs c = slot.coeffs;
    BellState l = slot.state[0];
    BellState r = slot.state[1];

    if (slot.ramping)
    {
        const BellCoeffs d = slot.delta;
        if (right != nullptr)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                c.a1 += d.a1;
                c.a2 += d.a2;
                c.a3 += d.a3;
                c.m1 += d.m1;
                tickStereo(c, l, r, left[i], right[i]);
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                c.a1 += d.a1;
                c.a2 += d.a2;
                c.a3 += d.a3;
                c.m1 += d.m1;
                left[i] = bellTick(c, l, left[i]);
            }
        }
        slot.coeffs = c;
    }
    else if (right != nullptr)
    {
        for (int i = 0; i < numSamples; ++i)
            tickStereo(c, l, r, left[i], right[i]);
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
            left[i] = bellTick(c, l, left[i]);
    }

    slot.state[0] = l;
    slot.state[1] = r;
}

void BandBank::process(double* left, double* right, int numSamples) noexcept
{
    int position = 0;
    while (position < numSamples)
    {
        if (moving && samplesToTick <= 0)
        {
            controlTick();
            samplesToTick = controlInterval;
        }

        const int chunk = moving ? std::min(numSamples - position, samplesToTick) : numSamples - position;

        for (int a = 0; a < numActive; ++a)
            runSlot(slots[activeSlots[a]], left + position, right != nullptr ? right + position : nullptr, chunk);

        if (moving)
            samplesToTick -= chunk;
        position += chunk;
    }
}
} // namespace rootnote
