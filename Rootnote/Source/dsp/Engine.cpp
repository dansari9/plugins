#include "Engine.h"

#include <algorithm>
#include <cmath>

namespace rootnote
{
namespace
{
BandBank::Settings boostSettings(const EngineParams& p) noexcept
{
    BandBank::Settings s;
    s.series = p.series;
    s.qMode = p.qMode;
    s.bands = p.bands;
    s.gainDb = std::clamp(p.boostGainDb, 0.0, kMaxBandGainDb);
    s.q = p.boostQ;
    s.blend = p.blend;
    s.focusLink = p.focusLink;
    s.focusHz = p.focusHz;
    s.rangeOct = p.rangeOct;
    s.tiltDbPerOct = p.tiltDbPerOct;
    return s;
}

BandBank::Settings cutSettings(const EngineParams& p) noexcept
{
    BandBank::Settings s;
    s.series = p.series;
    s.qMode = p.qMode;
    s.bands = p.cutBands;
    s.gainDb = p.cutOn ? std::clamp(p.cutDepthDb, -kMaxCutDb, 0.0) : 0.0;
    s.q = p.cutQ;
    s.overtones = p.cutOvertones;
    return s;
}

ModalRing::Settings ringSettings(const EngineParams& p) noexcept
{
    ModalRing::Settings s;
    s.shape.series = p.series;
    s.shape.bands = p.bands;
    s.shape.blend = p.blend;
    s.shape.focusLink = p.focusLink;
    s.shape.focusHz = p.focusHz;
    s.shape.rangeOct = p.rangeOct;
    s.shape.tiltDbPerOct = p.tiltDbPerOct;
    s.decaySeconds = p.ringDecaySeconds;
    s.dropSemitones = p.ringDropSemitones;
    return s;
}

double decibelsToGain(double db) noexcept
{
    return std::exp(db * (kLn10 / 20.0));
}

void retarget(LinearRamp& ramp, double target, int samples, bool snap) noexcept
{
    if (snap)
        ramp.snap(target);
    else if (target != ramp.target)
        ramp.setTarget(target, samples);
}
} // namespace

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    fs = sampleRate;
    maxBlock = std::max(16, maxBlockSize);
    levelRampSamples = std::max(1, static_cast<int>(std::lround(kLevelRampSeconds * sampleRate)));

    for (int ch = 0; ch < 2; ++ch)
    {
        work[ch].assign(static_cast<size_t>(maxBlock), 0.0);
        ringOut[ch].assign(static_cast<size_t>(maxBlock), 0.0);
    }

    boost.prepare(sampleRate, BandBank::Kind::boost);
    cut.prepare(sampleRate, BandBank::Kind::cut);
    ring.prepare(sampleRate);
    limiter.prepare(sampleRate);

    prepared = true;
    reset();
}

void Engine::reset() noexcept
{
    clearProcessingState();
    snapLevels = true;
    statesCleared = true;
}

void Engine::clearProcessingState() noexcept
{
    boost.reset();
    cut.reset();
    ring.reset();
    limiter.reset();
    limiterGainNow = 1.0;
    ringCleared = true;
}

void Engine::applyPitch(unsigned targets, double rampSeconds) noexcept
{
    if ((targets & NoteControl::boostChanged) != 0u)
    {
        const double hz = noteToHz(control.note(NoteControl::boost) + params.boostFineCents / 100.0, params.a4);
        boost.setTargetFrequency(hz, rampSeconds);
        ring.setTargetFrequency(hz, rampSeconds);
    }
    if ((targets & NoteControl::cutChanged) != 0u)
        cut.setTargetFrequency(noteToHz(control.note(NoteControl::cut), params.a4), rampSeconds);
}

void Engine::setParams(const EngineParams& newParams) noexcept
{
    params = newParams;

    NoteControl::Config config;
    config.mode = params.midiMode;
    config.noteOff = params.noteOff;
    config.keyLock = params.keyLock;
    config.key = params.key;
    config.scale = params.scale;
    config.boostKnobNote = params.boostNote;
    config.cutKnobNote = params.cutNote;
    control.setConfig(config);

    // The banks ignore a target they already have, so this only starts a ramp
    // when the knob, fine tune, A4 or the key lock really moved the pitch.
    applyPitch(NoteControl::boostChanged | NoteControl::cutChanged, kParamPitchRampSeconds);

    boost.setSettings(boostSettings(params));
    cut.setSettings(cutSettings(params));
    ring.setSettings(ringSettings(params));

    const bool snap = snapLevels;
    snapLevels = false;
    retarget(mixRamp, std::clamp(params.mix, 0.0, 1.0), levelRampSamples, snap);
    retarget(outputRamp, decibelsToGain(params.outputDb), levelRampSamples, snap);
    retarget(ringRamp, std::clamp(params.ring, 0.0, 1.0), levelRampSamples, snap);
    retarget(limiterRamp, params.limiter ? 1.0 : 0.0, levelRampSamples, snap);
    retarget(wetRamp, params.bypass ? 0.0 : 1.0, levelRampSamples, snap);
}

void Engine::handleMidi(const MidiEvent& event) noexcept
{
    const unsigned changed = control.handleEvent(event);
    if (changed != 0u)
        applyPitch(changed, std::max(params.glideSeconds, kMinMidiRampSeconds));
}

void Engine::process(float* const* channels, int numChannels, int numSamples) noexcept
{
    if (!prepared || numChannels < 1)
        return;

    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : nullptr;

    int position = 0;
    while (position < numSamples)
    {
        const int chunk = std::min(numSamples - position, maxBlock);
        processChunk(left + position, right != nullptr ? right + position : nullptr, chunk);
        position += chunk;
    }
}

void Engine::processChunk(float* left, float* right, int numSamples) noexcept
{
    if (wetRamp.value == 0.0 && !wetRamp.isActive())
    {
        // Fully bypassed: the input passes untouched and nothing runs. The
        // state is cleared once so processing restarts clean.
        if (!statesCleared)
        {
            clearProcessingState();
            statesCleared = true;
        }
        mixRamp.advance(numSamples);
        outputRamp.advance(numSamples);
        ringRamp.advance(numSamples);
        limiterRamp.advance(numSamples);
        return;
    }
    statesCleared = false;

    double* wl = work[0].data();
    double* wr = right != nullptr ? work[1].data() : nullptr;
    for (int i = 0; i < numSamples; ++i)
        wl[i] = static_cast<double>(left[i]);
    if (wr != nullptr)
        for (int i = 0; i < numSamples; ++i)
            wr[i] = static_cast<double>(right[i]);

    cut.process(wl, wr, numSamples);
    boost.process(wl, wr, numSamples);

    const bool ringOn = ringRamp.value > 0.0 || ringRamp.isActive();
    if (ringOn)
    {
        double* rl = ringOut[0].data();
        double* rr = right != nullptr ? ringOut[1].data() : nullptr;
        ring.process(left, right, rl, rr, numSamples);
        ringCleared = false;

        LinearRamp amount = ringRamp;
        for (int i = 0; i < numSamples; ++i)
            wl[i] += rl[i] * (amount.next() * kRingMakeup);
        if (wr != nullptr)
        {
            amount = ringRamp;
            for (int i = 0; i < numSamples; ++i)
                wr[i] += rr[i] * (amount.next() * kRingMakeup);
        }
        ringRamp.advance(numSamples);
    }
    else if (!ringCleared)
    {
        ring.reset();
        ringCleared = true;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryL = static_cast<double>(left[i]);
        const double dryR = right != nullptr ? static_cast<double>(right[i]) : dryL;

        const double mix = mixRamp.next();
        const double gain = outputRamp.next();
        double yl = (dryL + (wl[i] - dryL) * mix) * gain;
        double yr = wr != nullptr ? (dryR + (wr[i] - dryR) * mix) * gain : yl;

        // The limiter always runs so its envelope is current when it is
        // switched in; the switch itself is a short crossfade.
        double ll = yl;
        double lr = yr;
        limiter.tick(ll, lr);
        const double limit = limiterRamp.next();
        yl += (ll - yl) * limit;
        yr += (lr - yr) * limit;

        // Linear, not equal-power: dry and processed are the same signal apart
        // from the EQ, so an equal-power fade would bump the level by up to 3 dB.
        const double wet = wetRamp.next();
        left[i] = static_cast<float>(dryL + (yl - dryL) * wet);
        if (right != nullptr)
            right[i] = static_cast<float>(dryR + (yr - dryR) * wet);
    }

    limiterGainNow = limiter.gainReduction();
}

double Engine::tailSeconds(const EngineParams& p, double sampleRate) noexcept
{
    double longest = 0.0;

    if (p.boostGainDb > 0.0)
    {
        const BandBank::Settings s = boostSettings(p);
        BoostShape shape;
        shape.series = s.series;
        shape.bands = s.bands;
        shape.blend = s.blend;
        shape.focusLink = s.focusLink;
        shape.focusHz = s.focusHz;
        shape.rangeOct = s.rangeOct;
        shape.tiltDbPerOct = s.tiltDbPerOct;

        const double rootHz = noteToHz(p.boostNote + p.boostFineCents / 100.0, p.a4);
        const int count = seriesSlotCount(s.series);
        for (int slot = 0; slot < count; ++slot)
        {
            const double gainDb = boostSlotGainDb(shape, s.gainDb, slot, rootHz, sampleRate);
            if (gainDb < 0.05)
                continue;
            const double ratio = seriesRatio(s.series, slot);
            longest = std::max(longest, bellRingSeconds(rootHz * ratio, slotQ(s.qMode, s.q, ratio), gainDb));
        }
    }

    if (p.cutOn && p.cutDepthDb < 0.0)
    {
        const double rootHz = noteToHz(p.cutNote, p.a4);
        const int root = seriesRootSlot(p.series);
        const double lowest = p.series == SeriesType::octaves ? std::max(kLowFullHz, rootHz / 64.0) : rootHz;
        longest = std::max(longest, bellRingSeconds(lowest, slotQ(p.qMode, p.cutQ, seriesRatio(p.series, root)), 0.0));
    }

    if (p.ring > 0.0)
        longest = std::max(longest, p.ringDecaySeconds);

    // Limiter release and the level ramps.
    return std::min(kMaxTailSeconds, longest + 0.1);
}
} // namespace rootnote
