#include "SubHiss.h"
#include "SubHissNoiseData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>
#include <numbers>
#include <string>

namespace snap
{
namespace
{
using namespace subHissConstants;
using Filter = juce::dsp::StateVariableTPTFilterType;
constexpr double twoPi = 2.0 * std::numbers::pi;

float percent(float value) noexcept { return std::clamp(value, 0.0f, 100.0f) * 0.01f; }

void setValue(Smoother& smoother, float value, bool immediate) noexcept
{
    if (immediate)
        smoother.reset(value);
    else
        smoother.setTarget(value);
}

float fold(float x) noexcept
{
    const float magnitude = std::abs(x);
    const float triangle = 1.0f - std::abs(std::fmod(magnitude + 1.0f, 4.0f) - 2.0f);
    return x < 0.0f ? -triangle : triangle;
}
} // namespace

void SubHiss::Detector::prepare(const juce::dsp::ProcessSpec& spec, int lookaheadSamples)
{
    using namespace constants;
    for (auto& filter : highpass)
    {
        filter.prepare(spec);
        filter.setType(Filter::highpass);
    }
    for (auto& filter : lowpass)
    {
        filter.prepare(spec);
        filter.setType(Filter::lowpass);
    }
    lowHz = highHz = 0.0f;
    highestHz = (float) (spec.sampleRate * crossoverMaxFraction);
    envelope.prepare(spec.sampleRate, hitAttackMs, hitReleaseMs);
    retrigger = (int) std::lround(spec.sampleRate * hitRetriggerMs * 0.001);
    hysteresis = juce::Decibels::decibelsToGain(-hitHysteresisDb);
    lookahead = lookaheadSamples;
    reset();
}

void SubHiss::Detector::reset() noexcept
{
    for (auto& filter : highpass)
        filter.reset();
    for (auto& filter : lowpass)
        filter.reset();
    envelope.reset();
    sinceHit = retrigger;
    sinceQuiet = 0;
    pending = -1;
    armed = true;
}

void SubHiss::Detector::setRange(float lo, float hi) noexcept
{
    const auto [bottom, top] = std::minmax(lo, hi);
    const auto low = std::clamp(bottom, constants::focusMinHz, highestHz);
    const auto high = std::clamp(top, constants::focusMinHz, highestHz);
    if (std::abs(low - lowHz) > constants::denormalFloor)
    {
        for (auto& filter : highpass)
            filter.setCutoffFrequency(low);
        lowHz = low;
    }
    if (std::abs(high - highHz) > constants::denormalFloor)
    {
        for (auto& filter : lowpass)
            filter.setCutoffFrequency(high);
        highHz = high;
    }
}

float SubHiss::Detector::next(const std::array<float, 2>& input, int channels, float threshold) noexcept
{
    // Detect on the louder channel in the chosen band so a stereo hit cannot cancel in the key.
    float level = 0.0f;
    for (int c = 0; c < channels; ++c)
    {
        float y = input[(size_t) c];
        for (auto& filter : highpass)
            y = filter.processSample(c, y);
        for (auto& filter : lowpass)
            y = filter.processSample(c, y);
        level = std::max(level, std::abs(y));
    }
    const float e = envelope.next(level);
    const bool quiet = e < threshold * hysteresis;
    sinceHit = std::min(sinceHit + 1, retrigger);
    sinceQuiet = quiet ? 0 : std::min(sinceQuiet + 1, lookahead + 1);
    armed = armed || quiet;
    if (armed && e >= threshold && sinceHit >= retrigger && pending < 0)
    {
        armed = false;
        sinceHit = 0;
        // Recover the onset before threshold crossing, up to the available dry delay.
        pending = std::max(0, lookahead - std::max(0, sinceQuiet - 1));
        pendingThreshold = threshold;
        peak = e;
    }

    if (pending >= 0)
    {
        peak = std::max(peak, e);
        if (pending-- == 0)
        {
            using namespace constants;
            const float over = std::min(1.0f, juce::Decibels::gainToDecibels(peak / pendingThreshold) / hitVelocityRangeDb);
            const float velocity = std::lerp(hitMinVelocity, 1.0f, over);
            return std::lerp(1.0f, velocity, velocityTracking);
        }
    }
    return 0.0f;
}

void SubHiss::Layer::prepare(const juce::dsp::ProcessSpec& spec, int lookaheadSamples)
{
    detector.prepare(spec, lookaheadSamples);
    ducker.prepare(spec.sampleRate);
    for (auto* smoother : { &amount, &decayRate, &duck, &post, &lowHz, &highHz, &threshold })
        smoother->prepare(spec.sampleRate);
    decayScale = (float) (1000.0 * std::log(decayEndGain) / spec.sampleRate);
}

void SubHiss::Layer::reset() noexcept
{
    detector.reset();
    ducker.reset();
    for (auto* smoother : { &amount, &decayRate, &duck, &post, &lowHz, &highHz, &threshold })
        smoother->reset(smoother->targetValue());
    detector.setRange(lowHz.value(), highHz.value());
}

void SubHiss::Layer::set(const LayerSettings& layer, bool listen, bool audible, float defaultThresholdDb, bool immediate) noexcept
{
    if (listening && !listen)
        detector.reset();
    listening = listen;

    const float sense = percent(layer.sensitivity);
    const float db = sense <= 0.5f
                   ? std::lerp(lowSensitivityDb, defaultThresholdDb, sense * 2.0f)
                   : std::lerp(defaultThresholdDb, highSensitivityDb, sense * 2.0f - 1.0f);
    setValue(threshold, juce::Decibels::decibelsToGain(db), immediate);
    setValue(lowHz, layer.detectLoHz, immediate);
    setValue(highHz, layer.detectHiHz, immediate);
    setValue(amount, audible ? percent(layer.amount) : 0.0f, immediate);
    setValue(decayRate, std::exp(decayScale / std::clamp(layer.decayMs, 20.0f, 2000.0f)), immediate);
    setValue(duck, percent(layer.duck), immediate);
    setValue(post, layer.post ? 1.0f : 0.0f, immediate);
}

float SubHiss::Layer::detect(const std::array<float, 2>& input, int channels, bool updateFilters) noexcept
{
    const float lo = lowHz.next(), hi = highHz.next(), limit = threshold.next();
    if (updateFilters)
        detector.setRange(lo, hi);
    return listening ? detector.next(input, channels, limit) : 0.0f;
}

float SubHiss::Layer::gain(float key) noexcept { return amount.next() * ducker.next(key, duck.next()); }

void SubHiss::Voices::prepare(double fs, double attackMs) noexcept
{
    attackStep = (float) (1000.0 / (attackMs * fs));
    fadeStep = (float) (1000.0 / (retriggerFadeMs * fs));
    reset();
}

void SubHiss::Voices::trigger(float velocity, int type) noexcept
{
    previous = current;
    previousGain = previous.level > tailFloor ? 1.0f : 0.0f;
    current = { 0, velocity, 0, 1, type };
}

void SubHiss::Voices::reset() noexcept
{
    current = previous = {};
    previousGain = 0.0f;
}

float SubHiss::Voices::advance(Voice& voice, float decay) const noexcept
{
    const float gain = voice.level * voice.attack;
    voice.attack = std::min(1.0f, voice.attack + attackStep);
    voice.level *= decay;
    if (voice.level < tailFloor)
        voice.level = 0.0f;
    return gain;
}

void SubHiss::prepare(const juce::dsp::ProcessSpec& spec, const Settings& initial, bool lowLatency)
{
    sampleRate = spec.sampleRate;
    const int lookahead = latencyFor(sampleRate, lowLatency);
    dryDelay.prepare((int) spec.numChannels, std::max(latencyFor(sampleRate, false), latencyFor(sampleRate, true)));
    dryDelay.setDelay(lookahead);
    const int sideDelay = (int) std::lround(sampleRate * stereoDelayMs * 0.001);
    stereoDelay.prepare(1, sideDelay);
    stereoDelay.setDelay(sideDelay);

    const juce::dsp::ProcessSpec mono { sampleRate, spec.maximumBlockSize, 1 };
    stereoHighpass.prepare(mono);
    stereoHighpass.setType(juce::dsp::LinkwitzRileyFilterType::highpass);
    stereoHighpass.setCutoffFrequency(stereoCrossoverHz);
    acidLowpass.prepare(mono);
    acidLowpass.setCutoffFrequency(acidCutoffHz);
    acidLowpass.setResonance(acidResonance);
    hissLowpass.prepare(mono);
    highestHz = (float) (sampleRate * constants::crossoverMaxFraction);
    pitchFall = EnvelopeFollower::coef(sampleRate, pitchFallMs);
    filterFall = EnvelopeFollower::coef(sampleRate, filterFallMs);
    dcAlpha = (float) -std::expm1(-twoPi * dcCutoffHz / sampleRate);
    toneAlpha = (float) -std::expm1(-twoPi * hissTonePivotHz / sampleRate);
    filterInterval = std::max(1, (int) std::lround(sampleRate / filterUpdateHz));
    sub.prepare(spec, lookahead);
    hiss.prepare(spec, lookahead);
    subVoices.prepare(sampleRate, subAttackMs);
    hissVoices.prepare(sampleRate, hissAttackMs);
    for (auto* smoother : { &dryGain, &subPitch, &subColor, &subStereo, &subAcid, &hissPitch, &hissTone, &hissEnv, &hissRes })
        smoother->prepare(sampleRate);

    if (noise.front().empty())
        loadNoise();
    set(initial, true);
    reset();
}

void SubHiss::reset() noexcept
{
    dryDelay.reset();
    stereoDelay.reset();
    stereoHighpass.reset();
    acidLowpass.reset();
    hissLowpass.reset();
    sub.reset();
    hiss.reset();
    subVoices.reset();
    hissVoices.reset();
    subDc = hissTiltLow = 0.0f;
    untilFilterUpdate = 0;
    for (auto* smoother : { &dryGain, &subPitch, &subColor, &subStereo, &subAcid, &hissPitch, &hissTone, &hissEnv, &hissRes })
        smoother->reset(smoother->targetValue());
}

void SubHiss::setLowLatency(bool lowLatency) noexcept
{
    const int lookahead = latencyFor(sampleRate, lowLatency);
    if (lookahead == dryDelay.getDelay())
        return;
    dryDelay.setDelay(lookahead);
    sub.detector.lookahead = hiss.detector.lookahead = lookahead;
    reset();
}

void SubHiss::loadNoise()
{
    juce::WavAudioFormat wav;
    for (size_t i = 0; i < noise.size(); ++i)
    {
        if (!noise[i].empty())
            continue;
        const auto key = std::string(noiseNames[i]) + "_wav";
        int size = 0;
        const auto* data = SubHissNoiseData::getNamedResource(key.c_str(), size);
        if (data == nullptr)
        {
            jassertfalse;
            continue;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(
            new juce::MemoryInputStream(data, (size_t) size, false), true));
        if (reader == nullptr || reader->numChannels != 1 || std::abs(reader->sampleRate - hissSampleRate) > 0.5
            || reader->lengthInSamples < 2 || reader->lengthInSamples > std::numeric_limits<int>::max())
        {
            jassertfalse;
            continue;
        }
        const auto length = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> decoded(1, length);
        if (reader->read(&decoded, 0, length, 0, true, false))
            noise[i].assign(decoded.getReadPointer(0), decoded.getReadPointer(0) + length);
        else
            jassertfalse;
    }
}

void SubHiss::set(const Settings& s, bool immediate) noexcept
{
    const bool subActive = s.enabled && s.sub.amount > 0.0f;
    const bool hissActive = s.enabled && s.hiss.amount > 0.0f;
    const bool subSolo = subActive && s.sub.solo;
    const bool hissSolo = hissActive && s.hiss.solo;
    const bool solo = subSolo || hissSolo;

    sub.set(s.sub, subActive && s.sub.sensitivity > 0.0f, subActive && (!solo || subSolo), subDefaultThresholdDb, immediate);
    hiss.set(s.hiss, hissActive && s.hiss.sensitivity > 0.0f, hissActive && (!solo || hissSolo), hissDefaultThresholdDb, immediate);
    setValue(dryGain, solo ? 0.0f : 1.0f, immediate);
    setValue(subPitch, (float) juce::MidiMessage::getMidiNoteInHertz(juce::roundToInt(s.sub.pitch)), immediate);
    setValue(subColor, percent(s.sub.color), immediate);
    setValue(subStereo, percent(s.sub.stereo), immediate);
    setValue(subAcid, s.sub.acid ? 1.0f : 0.0f, immediate);
    setValue(hissPitch, (float) (hissSampleRate / sampleRate) * std::exp2(s.hiss.pitch / 12.0f), immediate);
    setValue(hissTone, std::clamp(s.hiss.tone, -100.0f, 100.0f) * 0.01f, immediate);
    setValue(hissEnv, percent(s.hiss.env), immediate);
    setValue(hissRes, percent(s.hiss.res), immediate);
    hissType = std::clamp(s.hiss.type, 0, (int) noise.size() - 1);
}

std::array<float, 3> SubHiss::osc(Voice& voice, double pitchHz, float decay, float color, float weight) noexcept
{
    if (voice.level <= 0.0f)
        return {};
    const float sine = (float) std::sin(voice.phase), cosine = (float) std::cos(voice.phase);
    const float gain = weight * subVoices.advance(voice, decay);
    const float driven = std::tanh(colorDrive * sine + colorBias) - std::tanh(colorBias);
    const float normal = std::lerp(sine, driven, color);
    const float folded = fold(sine * (acidMinDrive + color * acidColorDrive));
    const float second = 2.0f * sine * cosine;
    const float third = sine * (3.0f - 4.0f * sine * sine);
    const float harmonic = stereoSecondHarmonic * second + stereoThirdHarmonic * third;
    voice.phase += twoPi * pitchHz * std::exp2(voice.sweep * pitchDropSemitones / 12.0f) / sampleRate;
    if (voice.phase >= twoPi)
        voice.phase -= twoPi;
    voice.sweep *= pitchFall;
    return { gain * normal, gain * folded, gain * harmonic };
}

std::array<float, 2> SubHiss::renderSub(float velocity) noexcept
{
    if (velocity > 0.0f)
        subVoices.trigger(velocity);
    const float color = subColor.next(), stereo = subStereo.next(), acid = subAcid.next();
    const float decay = sub.decayRate.next();
    const double pitch = subPitch.next();
    const float fade = subVoices.previousGain;

    const auto [normal, folded, harmonics] = osc(subVoices.current, pitch, decay, color, 1.0f - fade);
    const auto [oldNormal, oldFolded, oldHarmonics] = fade > 0.0f ? osc(subVoices.previous, pitch, decay, color, fade)
                                                                : std::array<float, 3> {};
    subVoices.previousGain = std::max(0.0f, fade - subVoices.fadeStep);

    const float body = std::lerp(normal + oldNormal, acidLowpass.processSample(0, folded + oldFolded), acid);
    subDc += dcAlpha * (body - subDc);
    const float center = subGain * (body - subDc);
    const float harmonic = stereoHighpass.processSample(0, harmonics + oldHarmonics);
    const float delayed = stereoDelay.next(0, harmonic);
    const float side = subGain * stereoGain * stereo * std::max(color, acid) * (harmonic - delayed);
    return { center, side };
}

float SubHiss::playNoise(Voice& voice, double increment, float decay) const noexcept
{
    const auto& samples = noise[(size_t) voice.type];
    if (voice.level <= 0.0f || samples.empty())
        return 0.0f;
    const auto index = (size_t) voice.phase;
    const auto next = index + 1 < samples.size() ? index + 1 : 0;
    const float sample = std::lerp(samples[index], samples[next], (float) (voice.phase - (double) index));
    voice.phase += increment;
    if (voice.phase >= (double) samples.size())
        voice.phase -= (double) samples.size();
    voice.sweep *= filterFall;
    return sample * hissVoices.advance(voice, decay);
}

float SubHiss::renderHiss(float velocity, bool updateFilters) noexcept
{
    if (velocity > 0.0f)
        hissVoices.trigger(velocity, hissType);
    const float increment = hissPitch.next(), tone = hissTone.next(), env = hissEnv.next(), res = hissRes.next();
    const float decay = hiss.decayRate.next(), fade = hissVoices.previousGain;
    if (updateFilters)
    {
        const float sweep = std::lerp(hissVoices.current.sweep, hissVoices.previous.sweep, fade);
        const float cutoff = hissOpenHz * std::exp2(-env * hissClosedOctaves * (1.0f - sweep));
        hissLowpass.setCutoffFrequency(std::min(cutoff, highestHz));
        hissLowpass.setResonance(std::lerp(juce::MathConstants<float>::sqrt2 * 0.5f, hissMaxResonance, res));
    }
    const float current = playNoise(hissVoices.current, increment, decay);
    const float previous = fade > 0.0f ? playNoise(hissVoices.previous, increment, decay) : 0.0f;
    hissVoices.previousGain = std::max(0.0f, fade - hissVoices.fadeStep);
    const float dry = std::lerp(current, previous, fade);
    hissTiltLow += toneAlpha * (dry - hissTiltLow);
    const float tilted = dry + hissTilt * tone * (dry - 2.0f * hissTiltLow);
    return hissGain * hissLowpass.processSample(0, tilted);
}

void SubHiss::process(juce::AudioBuffer<float>& io, juce::AudioBuffer<float>& post, const Settings& settings) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    const int channels = io.getNumChannels(), samples = io.getNumSamples();
    jassert(channels == dryDelay.getNumChannels() && channels >= 1 && channels <= 2);
    jassert(post.getNumChannels() == channels && post.getNumSamples() == samples);
    set(settings, false);

    auto* const* const preOutput = io.getArrayOfWritePointers();
    auto* const* const postOutput = post.getArrayOfWritePointers();
    for (int i = 0; i < samples; ++i)
    {
        const bool updateFilters = untilFilterUpdate == 0;
        if (updateFilters)
            untilFilterUpdate = filterInterval;
        --untilFilterUpdate;

        const std::array input { preOutput[0][i], channels == 2 ? preOutput[1][i] : preOutput[0][i] };
        const float subHit = sub.detect(input, channels, updateFilters);
        const float hissHit = hiss.detect(input, channels, updateFilters);
        if (subHit > 0.0f)
            hitCount[0].fetch_add(1, std::memory_order_relaxed);
        if (hissHit > 0.0f)
            hitCount[1].fetch_add(1, std::memory_order_relaxed);
        const std::array dry { dryDelay.next(0, input[0]), channels == 2 ? dryDelay.next(1, input[1]) : 0.0f };
        const float key = std::max(std::abs(dry[0]), std::abs(dry[1]));
        const auto [center, side] = renderSub(subHit);
        const float subLevel = sub.gain(key), subPost = sub.post.next();
        const float hissSignal = renderHiss(hissHit, updateFilters) * hiss.gain(key);
        const float hissPost = hiss.post.next(), dryLevel = dryGain.next();

        for (int c = 0; c < channels; ++c)
        {
            const float low = (center + (channels == 1 ? 0.0f : c == 0 ? side : -side)) * subLevel;
            postOutput[c][i] = subPost * low + hissPost * hissSignal;
            const float pre = (1.0f - subPost) * low + (1.0f - hissPost) * hissSignal;
            const float base = dry[(size_t) c] * dryLevel;
            preOutput[c][i] = std::abs(pre) > 0.0f ? base + pre : base;
        }
    }
}
} // namespace snap
