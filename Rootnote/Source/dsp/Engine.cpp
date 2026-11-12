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

/**
    Upper bound for the T60 of every band a bank can have while its root is
    anywhere in [lowRootHz, highRootHz]. The slowest band is at one end of the
    spectrum: resonances get longer towards low frequencies, and the bilinear
    transform stretches them again close to Nyquist.
*/
double bankRingBound(SeriesType series, int bands, QMode qMode, double q, double gainDb,
                     double lowRootHz, double highRootHz, double sampleRate) noexcept
{
    const double top = kNyquistGuard * sampleRate;
    const int slots = series == SeriesType::octaves ? kOctaveSlots : std::clamp(bands, 1, kMaxSlots);
    double longest = 0.0;
    for (int slot = 0; slot < slots; ++slot)
    {
        const double ratio = seriesRatio(series, slot);
        const double lowest = std::clamp(lowRootHz * ratio, kLowSkipHz, top);
        const double highest = std::clamp(highRootHz * ratio, kLowSkipHz, top);
        // Weighting can put the gain anywhere between zero and its maximum.
        // Both gain endpoints matter: deep, wide cuts have slow real poles.
        for (double hz : { lowest, highest })
            for (double db : { 0.0, gainDb })
                longest = std::max(longest, bellDigitalRingSeconds(hz, slotQ(qMode, q, ratio), db, sampleRate));
    }
    return longest;
}

double blendSignals(double dry, double wet, double amount) noexcept
{
    if (amount <= 0.0) return dry;
    if (amount >= 1.0) return wet;
    return (1.0 - amount) * dry + amount * wet;
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

    const auto boostConfig = boostSettings(params);
    boost.setSettings(boostConfig);
    cut.setSettings(cutSettings(params));
    ring.setSettings({ boostConfig.shape(), params.ringDecaySeconds, params.ringDropSemitones });

    const bool snap = snapLevels;
    snapLevels = false;
    retarget(mixRamp, std::clamp(params.mix, 0.0, 1.0), levelRampSamples, snap);
    retarget(outputRamp, decibelsToGain(params.outputDb), levelRampSamples, snap);
    retarget(ringRamp, std::clamp(params.ring, 0.0, 1.0), levelRampSamples, snap);
    retarget(limiterRamp, params.limiter ? 1.0 : 0.0, levelRampSamples, snap);
    retarget(wetRamp, params.bypass ? 0.0 : 1.0, levelRampSamples, snap);
}

void Engine::takeBackFromMidi(unsigned targets) noexcept
{
    if (targets == 0u)
        return;
    control.takeBack(targets);
    applyPitch(targets, kParamPitchRampSeconds);
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
    else
    {
        if (!ringCleared)
        {
            ring.silence();
            ringCleared = true;
        }
        ring.listen(left, right, numSamples);
    }

    // A non-finite upstream sample poisons recursive filters, even with the
    // limiter on. The original dry chunk is still available here.
    for (int i = 0; i < numSamples; ++i)
    {
        if (!std::isfinite(wl[i]) || (wr != nullptr && !std::isfinite(wr[i])))
        {
            clearProcessingState();
            for (int j = 0; j < numSamples; ++j)
            {
                if (!std::isfinite(left[j]))
                    left[j] = 0.0f;
                if (right != nullptr && !std::isfinite(right[j]))
                    right[j] = 0.0f;
            }
            return;
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryL = static_cast<double>(left[i]);
        const double dryR = right != nullptr ? static_cast<double>(right[i]) : dryL;

        const double mix = mixRamp.next();
        const double gain = outputRamp.next();
        double yl = blendSignals(dryL, wl[i], mix) * gain;
        double yr = wr != nullptr ? blendSignals(dryR, wr[i], mix) * gain : yl;

        // The limiter always runs so its envelope is current when it is
        // switched in; the switch itself is a short crossfade.
        double ll = yl;
        double lr = yr;
        limiter.tick(ll, lr);
        // Blended as a weighted sum: "y + (limited - y) * limit" would lose
        // the limited signal to rounding when y is huge.
        const double limit = limiterRamp.next();
        yl = blendSignals(yl, ll, limit);
        yr = blendSignals(yr, lr, limit);

        // Linear, not equal-power: dry and processed are the same signal apart
        // from the EQ, so an equal-power fade would bump the level by up to 3 dB.
        const double wet = wetRamp.next();
        left[i] = static_cast<float>(blendSignals(dryL, yl, wet));
        if (right != nullptr)
            right[i] = static_cast<float>(blendSignals(dryR, yr, wet));
    }

    limiterGainNow = blendSignals(1.0, limiter.gainReduction(), limiterRamp.value);
}

double Engine::tailSeconds(const EngineParams& p, double sampleRate) noexcept
{
    // MIDI can put a bank on any note, whatever the knob says, and the audio
    // thread's notes are not known here: with MIDI on, assume the whole range.
    const auto rootRange = [&p](double knobNote, bool midi, double& lowHz, double& highHz) {
        lowHz = noteToHz(midi ? kMinNote : knobNote, p.a4);
        highHz = noteToHz(midi ? kMaxNote : knobNote, p.a4);
    };

    double longest = 0.0;
    double lowHz = 0.0, highHz = 0.0;

    if (p.boostGainDb > 0.0)
    {
        // A band's gain is the Boost value times its weight. Only a tilt can push the weight above 1.
        const bool weightAboveOne = p.tiltDbPerOct > 0.0 || (p.series == SeriesType::octaves && p.tiltDbPerOct < 0.0);
        const double gainDb = weightAboveOne ? kMaxBandGainDb : std::min(p.boostGainDb, kMaxBandGainDb);
        rootRange(p.boostNote + p.boostFineCents / 100.0, p.midiMode != MidiMode::off, lowHz, highHz);
        longest = bankRingBound(p.series, p.bands, p.qMode, p.boostQ, gainDb, lowHz, highHz, sampleRate);
    }

    if (p.cutOn && p.cutDepthDb < 0.0)
    {
        rootRange(p.cutNote, p.midiMode == MidiMode::channels12, lowHz, highHz);
        longest = std::max(longest, bankRingBound(p.series, p.cutBands, p.qMode, p.cutQ, p.cutDepthDb, lowHz, highHz, sampleRate));
    }

    if (p.ring > 0.0)
        longest = std::max(longest, p.ringDecaySeconds);

    // Limiter release and the level ramps.
    return longest + 0.1;
}

int Engine::boostBands(const EngineParams& p, double rootHz, double sampleRate, BandView* out) noexcept
{
    const BandBank::Settings s = boostSettings(p);
    const BoostShape shape = s.shape();
    int count = 0;
    for (int slot = 0; slot < seriesSlotCount(s.series); ++slot)
    {
        const double gainDb = boostSlotGainDb(shape, s.gainDb, slot, rootHz, sampleRate);
        if (gainDb != 0.0)
            out[count++] = { rootHz * seriesRatio(s.series, slot), gainDb };
    }
    return count;
}

int Engine::cutBands(const EngineParams& p, double rootHz, double sampleRate, BandView* out) noexcept
{
    const BandBank::Settings s = cutSettings(p);
    int count = 0;
    for (int slot = 0; slot < seriesSlotCount(s.series); ++slot)
    {
        const double gainDb = cutSlotGainDb(s.series, s.bands, s.gainDb, s.overtones, slot, rootHz, sampleRate);
        if (gainDb != 0.0)
            out[count++] = { rootHz * seriesRatio(s.series, slot), gainDb };
    }
    return count;
}
} // namespace rootnote
