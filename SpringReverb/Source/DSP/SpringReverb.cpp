#include "DSP/SpringReverb.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
constexpr int kSprings = 3;
// Short in-loop cascades. A long chain here stretches every repeat and the
// tail collapses even when the reflection gain is set for a multi-second RT60.
constexpr int kLfStages = 6;
constexpr int kHfStages = 4;
// Pickup cascade for the falling boing. Outside the feedback loop.
constexpr int kDripStages = 64;
constexpr float kMaxCoeff = 0.93f;

constexpr float kDelayMs[kSprings] = { 39.0f, 52.5f, 68.0f };
constexpr float kHfRatio[kSprings] = { 0.55f, 0.63f, 0.71f };
constexpr float kPickup[kSprings] = { 0.62f, -0.54f, 0.46f };
constexpr float kAOffset[kSprings] = { -0.025f, 0.0f, 0.03f };
constexpr float kChDelay[2][kSprings] = {
    { 1.000f, 1.000f, 1.000f },
    { 1.017f, 0.984f, 1.026f }
};

constexpr float kInputTrim = 0.62f;
constexpr float kMakeup = 3.6f;

float sanitize(float value, float fallback, float lo, float hi)
{
    if (!std::isfinite(value))
        return fallback;
    return juce::jlimit(lo, hi, value);
}

float expMap(float t, float lo, float hi)
{
    return lo * std::pow(hi / lo, juce::jlimit(0.0f, 1.0f, t));
}

// Circular buffer. readAge(0) is the most recently written sample.
class DelayLine
{
public:
    void prepare(int maxDelaySamples)
    {
        const int cap = juce::nextPowerOfTwo(std::max(16, maxDelaySamples + 8));
        buffer.assign(static_cast<size_t>(cap), 0.0f);
        mask = cap - 1;
        writePos = 0;
        maxAge = static_cast<float>(cap - 6);
    }

    void clear()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    void write(float x)
    {
        buffer[static_cast<size_t>(writePos & mask)] = x;
        writePos = (writePos + 1) & mask;
    }

    float readAge(float age) const
    {
        const float a = std::clamp(age, 0.0f, maxAge);
        const float readPos = static_cast<float>(writePos) - 1.0f - a;
        const int i0 = static_cast<int>(std::floor(readPos));
        const float frac = readPos - static_cast<float>(i0);
        const float s0 = buffer[static_cast<size_t>(i0 & mask)];
        const float s1 = buffer[static_cast<size_t>((i0 + 1) & mask)];
        return s0 + frac * (s1 - s0);
    }

    int capacity() const noexcept { return mask + 1; }

private:
    std::vector<float> buffer;
    int mask = 0;
    int writePos = 0;
    float maxAge = 1.0f;
};

// Stretched allpass: H(z) = (a + z^{-d}) / (1 + a z^{-d}), |a| < 1.
struct StretchAllpass
{
    DelayLine line;

    void prepare(int maxDelay) { line.prepare(maxDelay); }
    void clear() { line.clear(); }

    // Integer delay keeps the section an exact allpass. Fractional reads would
    // lowpass the loop and kill the tail.
    float process(float x, int delaySamples, float a)
    {
        const int d = std::max(1, delaySamples);
        const float z = line.readAge(static_cast<float>(d - 1));
        const float y = a * x + z;
        line.write(x - a * y);
        return y;
    }
};

float cascade(StretchAllpass* stages, int count, float x, int delay, float a)
{
    a = std::clamp(a, -kMaxCoeff, kMaxCoeff);
    delay = std::max(1, delay);
    for (int i = 0; i < count; ++i)
        x = stages[i].process(x, delay, a);
    return x;
}

float saturate(float x)
{
    return std::tanh(x);
}

float dcBlock(float x, float& x1, float& y1, float r)
{
    const float y = x - x1 + r * y1;
    x1 = x;
    y1 = y;
    return y;
}

float onePoleLp(float x, float& state, float coeff)
{
    state += coeff * (x - state);
    return state;
}

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void setPeaking(double sampleRate, float freq, float q, float gainDb)
    {
        const float nyquist = static_cast<float>(sampleRate) * 0.45f;
        freq = juce::jlimit(40.0f, nyquist, freq);
        q = std::max(0.2f, q);
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = juce::MathConstants<float>::twoPi * freq / static_cast<float>(sampleRate);
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0;
        b1 = (-2.0f * cosw) / a0;
        b2 = (1.0f - alpha * A) / a0;
        a1 = (-2.0f * cosw) / a0;
        a2 = (1.0f - alpha / A) / a0;
    }

    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void clear() { z1 = z2 = 0.0f; }
};

struct Modulator
{
    uint32_t rng = 1;
    float value = 0.0f;
    float target = 0.0f;
    int hold = 0;

    void prepare(uint32_t seed)
    {
        rng = seed == 0 ? 1u : seed;
        value = 0.0f;
        target = 0.0f;
        hold = 0;
    }

    float next(float depth, int holdSamples, float smooth)
    {
        if (--hold <= 0)
        {
            hold = std::max(1, holdSamples);
            rng = rng * 1664525u + 1013904223u;
            const float u = static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
            target = (u * 2.0f - 1.0f) * depth;
        }
        value += smooth * (target - value);
        return value;
    }
};

struct Spring
{
    DelayLine lowDelay;
    DelayLine highDelay;
    std::array<StretchAllpass, kLfStages> lowDisp {};
    std::array<StretchAllpass, kHfStages> highDisp {};
    std::array<StretchAllpass, kDripStages> dripDisp {};
    float lowLp = 0.0f;
    float lowDcX = 0.0f, lowDcY = 0.0f;
    float highLp = 0.0f;
    float highHpX = 0.0f, highHpY = 0.0f;
    float baseLow = 1000.0f;
    float baseHigh = 600.0f;
    Modulator mod;

    void clear()
    {
        lowDelay.clear();
        highDelay.clear();
        for (auto& s : lowDisp) s.clear();
        for (auto& s : highDisp) s.clear();
        for (auto& s : dripDisp) s.clear();
        lowLp = lowDcX = lowDcY = 0.0f;
        highLp = highHpX = highHpY = 0.0f;
        mod.value = mod.target = 0.0f;
        mod.hold = 0;
    }
};

struct Channel
{
    DelayLine preDelay;
    std::array<Spring, kSprings> springs;
    float inDcX = 0.0f, inDcY = 0.0f;
    float outHpX = 0.0f, outHpY = 0.0f;
    Biquad peak;
};

struct SharedCoeffs
{
    float aLf[kSprings] {};
    float aHf[kSprings] {};
    float aDrip[kSprings] {};
    float fbLf[kSprings] {};
    float fbHf[kSprings] {};
    float lpLf = 0.2f;
    float lpHf = 0.4f;
    float hpHf = 0.05f;
    float preSamples = 0.0f;
    float modDepth = 1.0f;
    float bodyG = 1.0f;
    float dripG = 0.5f;
    float dryG = 1.0f;
    float wetG = 0.0f;
    float outG = 1.0f;
    float maxDelay = 1000.0f;
};

int argMaxAbs(const std::vector<float>& x)
{
    int index = 0;
    float best = 0.0f;
    for (int i = 0; i < static_cast<int>(x.size()); ++i)
    {
        const float a = std::abs(x[static_cast<size_t>(i)]);
        if (a > best)
        {
            best = a;
            index = i;
        }
    }
    return index;
}

// Group delay in samples, from the impulse-response phase slope.
float groupDelaySamples(const std::vector<float>& ir, float freq, float sampleRate)
{
    // Small step so a multi-hundred-sample delay stays inside one phase wrap.
    const float df = 2.0f;
    const auto phaseAt = [&ir, sampleRate](float f)
    {
        double re = 0.0;
        double im = 0.0;
        const double w = juce::MathConstants<double>::twoPi * static_cast<double>(f) / sampleRate;
        for (int n = 0; n < static_cast<int>(ir.size()); ++n)
        {
            const double ph = w * static_cast<double>(n);
            const double s = ir[static_cast<size_t>(n)];
            re += s * std::cos(ph);
            im -= s * std::sin(ph);
        }
        return std::atan2(im, re);
    };

    double dp = phaseAt(freq + df) - phaseAt(freq - df);
    const double pi = juce::MathConstants<double>::pi;
    while (dp > pi)
        dp -= 2.0 * pi;
    while (dp < -pi)
        dp += 2.0 * pi;
    return static_cast<float>(-(dp) * sampleRate / (juce::MathConstants<double>::twoPi * (2.0 * df)));
}

std::vector<float> renderCascade(int stages, int delay, float coeff, int length)
{
    std::array<StretchAllpass, kDripStages> bank {};
    const int n = std::min(stages, kDripStages);
    for (int i = 0; i < n; ++i)
        bank[static_cast<size_t>(i)].prepare(64);

    std::vector<float> out(static_cast<size_t>(length), 0.0f);
    for (int i = 0; i < length; ++i)
    {
        const float x = i == 0 ? 1.0f : 0.0f;
        out[static_cast<size_t>(i)] = cascade(bank.data(), n, x, delay, coeff);
    }
    return out;
}
} // namespace

struct SpringReverb::Impl
{
    double sampleRate = 48000.0;
    int activeChannels = 2;
    bool prepared = false;
    float maxLineDelay = 4000.0f;
    int stretchLf[kSprings] = { 2, 2, 3 };
    int stretchHf[kSprings] = { 2, 2, 2 };

    float dcR = 0.995f;
    float outHpR = 0.99f;

    SpringReverb::Params params {};
    juce::SmoothedValue<float> decaySm, tensionSm, toneSm, dripSm, preSm, mixSm, outSm;

    std::array<Channel, 2> channels {};

    void clearAudio()
    {
        for (auto& ch : channels)
        {
            ch.preDelay.clear();
            ch.inDcX = ch.inDcY = 0.0f;
            ch.outHpX = ch.outHpY = 0.0f;
            ch.peak.clear();
            for (auto& spring : ch.springs)
                spring.clear();
        }
    }

    void prepareSmoothers(bool snap)
    {
        const double sr = sampleRate;
        decaySm.reset(sr, 0.08);
        tensionSm.reset(sr, 0.06);
        toneSm.reset(sr, 0.03);
        dripSm.reset(sr, 0.02);
        preSm.reset(sr, 0.04);
        mixSm.reset(sr, 0.02);
        outSm.reset(sr, 0.02);

        decaySm.setCurrentAndTargetValue(params.decaySeconds);
        tensionSm.setCurrentAndTargetValue(params.tension);
        toneSm.setCurrentAndTargetValue(params.tone);
        dripSm.setCurrentAndTargetValue(params.drip);
        preSm.setCurrentAndTargetValue(params.preDelayMs);
        mixSm.setCurrentAndTargetValue(params.mix);
        outSm.setCurrentAndTargetValue(params.outputDb);
        juce::ignoreUnused(snap);
    }

    SharedCoeffs tick()
    {
        const float sr = static_cast<float>(sampleRate);
        const float srScale = sr / 48000.0f;
        const float tension = tensionSm.getNextValue();
        const float tone = toneSm.getNextValue();
        const float drip = dripSm.getNextValue();
        const float decay = std::max(0.05f, decaySm.getNextValue());
        const float preMs = preSm.getNextValue();
        const float mix = mixSm.getNextValue();
        const float outDb = outSm.getNextValue();

        const float t = juce::jlimit(0.0f, 1.0f, tension);
        // Negative coefficients: group delay falls with frequency, so the
        // transient starts bright and slides down. Loose tension is chirpier.
        const float aLfBase = juce::jmap(t, -0.58f, -0.20f);
        const float aHfBase = juce::jmap(t, -0.70f, -0.26f);
        const float aDripBase = juce::jmap(t, -0.90f, -0.46f);

        const float nyquist = sr * 0.45f;
        // In-loop damping stays gentle so Decay, not Tone, sets the RT60.
        // Tone's obvious brightness lives on the output filters below.
        const float fcLf = std::min(nyquist, expMap(tone, 6500.0f, 15000.0f));
        const float fcHf = std::min(nyquist, expMap(tone, 9000.0f, 17000.0f));
        const float fcHfHp = std::min(nyquist * 0.5f, 720.0f);

        SharedCoeffs c;
        c.lpLf = 1.0f - std::exp(-juce::MathConstants<float>::twoPi * fcLf / sr);
        c.lpHf = 1.0f - std::exp(-juce::MathConstants<float>::twoPi * fcHf / sr);
        c.hpHf = std::exp(-juce::MathConstants<float>::twoPi * fcHfHp / sr);
        c.preSamples = std::max(0.0f, preMs * 0.001f * sr);
        // Kept at zero: stepping the feedback delay clicks, and a fractional
        // read inside the loop lowpasses every round trip.
        c.modDepth = 0.0f;
        juce::ignoreUnused(srScale);
        c.maxDelay = maxLineDelay;

        // Drip brings up the falling high loop without dropping the body entirely.
        c.bodyG = 1.0f - 0.18f * drip;
        c.dripG = 0.10f + 1.20f * drip;
        const float norm = 1.0f / std::sqrt(c.bodyG * c.bodyG + 0.45f * c.dripG * c.dripG);
        c.bodyG *= norm;
        c.dripG *= norm;

        const float halfPi = juce::MathConstants<float>::halfPi;
        c.dryG = std::cos(mix * halfPi);
        c.wetG = std::sin(mix * halfPi);
        c.outG = juce::Decibels::decibelsToGain(outDb);

        for (int s = 0; s < kSprings; ++s)
        {
            c.aLf[s] = std::clamp(aLfBase + kAOffset[s], -kMaxCoeff, kMaxCoeff);
            c.aHf[s] = std::clamp(aHfBase - 0.6f * kAOffset[s], -kMaxCoeff, kMaxCoeff);
            c.aDrip[s] = std::clamp(aDripBase - 0.4f * kAOffset[s], -kMaxCoeff, kMaxCoeff);

            const float lowBulk = static_cast<float>(kLfStages * stretchLf[s]);
            const float highBulk = static_cast<float>(kHfStages * stretchHf[s]);
            const float lowSeconds = (channels[0].springs[static_cast<size_t>(s)].baseLow + lowBulk) / sr;
            const float highSeconds = (channels[0].springs[static_cast<size_t>(s)].baseHigh + highBulk) / sr;
            const float hfRt60 = decay * juce::jmap(tone, 0.62f, 0.92f);

            const float magLf = std::pow(10.0f, -3.0f * lowSeconds / decay);
            const float magHf = std::pow(10.0f, -3.0f * highSeconds / std::max(0.05f, hfRt60));
            // Negative end reflection. The allpasses are unity-gain, so this
            // ceiling is the whole loop gain apart from the damping lowpass.
            c.fbLf[s] = -std::clamp(magLf, 0.0f, 0.972f);
            c.fbHf[s] = -std::clamp(magHf, 0.0f, 0.94f);
        }

        const float peakHz = std::min(nyquist, expMap(tone, 320.0f, 2600.0f));
        for (int ch = 0; ch < 2; ++ch)
            channels[static_cast<size_t>(ch)].peak.setPeaking(sampleRate, peakHz, 0.85f, 3.2f);

        return c;
    }

    float processSpring(Spring& spring, float input, const SharedCoeffs& c, int index,
                        int chIndex, float mod)
    {
        juce::ignoreUnused(chIndex);
        // Integer reads: linear interpolation in the feedback loop was lowpassing
        // every round trip and swallowing the decay. Modulation still moves the
        // tap, one sample at a time.
        const int lowDelay = juce::jlimit(1, static_cast<int>(c.maxDelay),
                                          static_cast<int>(std::lround(spring.baseLow + mod)));
        const int highDelay = juce::jlimit(1, static_cast<int>(c.maxDelay),
                                           static_cast<int>(std::lround(spring.baseHigh + mod * 0.65f)));

        const int stretch = stretchLf[index];
        const float lowTap = spring.lowDelay.readAge(static_cast<float>(lowDelay - 1));
        float low = cascade(spring.lowDisp.data(), kLfStages, lowTap, stretch, c.aLf[index]);
        low = dcBlock(low, spring.lowDcX, spring.lowDcY, dcR);
        low = onePoleLp(low, spring.lowLp, c.lpLf);
        if (std::abs(low) < 1.0e-15f)
            low = 0.0f;
        spring.lowDelay.write(input + c.fbLf[index] * low);

        const float highTap = spring.highDelay.readAge(static_cast<float>(highDelay - 1));
        float high = cascade(spring.highDisp.data(), kHfStages, highTap, stretch, c.aHf[index]);
        high = dcBlock(high, spring.highHpX, spring.highHpY, c.hpHf);
        high = onePoleLp(high, spring.highLp, c.lpHf);
        if (std::abs(high) < 1.0e-15f)
            high = 0.0f;
        spring.highDelay.write(input + c.fbHf[index] * high);

        const float drip = cascade(spring.dripDisp.data(), kDripStages, high, stretch, c.aDrip[index]);
        return kPickup[index] * (c.bodyG * low + c.dripG * drip);
    }

    // Wet tank only. Dry/wet mix and output gain happen in process() so a fully
    // dry setting stays sample-accurate and crossfeed never touches the dry path.
    float processChannel(Channel& channel, float input, const SharedCoeffs& c, int chIndex)
    {
        if (!std::isfinite(input))
            input = 0.0f;

        const float blocked = dcBlock(input, channel.inDcX, channel.inDcY, dcR);
        channel.preDelay.write(blocked);
        const float excited = saturate(channel.preDelay.readAge(c.preSamples) * kInputTrim);

        float wet = 0.0f;
        const int hold = std::max(1, static_cast<int>(0.018 * sampleRate));
        const float smooth = 1.0f - std::exp(-juce::MathConstants<float>::twoPi * 6.0f
                                              / static_cast<float>(sampleRate));
        for (int s = 0; s < kSprings; ++s)
        {
            auto& spring = channel.springs[static_cast<size_t>(s)];
            const float mod = spring.mod.next(c.modDepth, hold, smooth);
            wet += processSpring(spring, excited, c, s, chIndex, mod);
        }

        wet *= kMakeup;
        wet = channel.peak.process(wet);
        wet = dcBlock(wet, channel.outHpX, channel.outHpY, outHpR);

        if (!std::isfinite(wet))
        {
            channel.outHpX = channel.outHpY = 0.0f;
            channel.peak.clear();
            for (auto& spring : channel.springs)
                spring.clear();
            wet = 0.0f;
        }

        return wet;
    }
};

SpringReverb::SpringReverb()
    : impl(std::make_unique<Impl>())
{
}

SpringReverb::~SpringReverb() = default;

bool SpringReverb::isPrepared() const noexcept
{
    return impl->prepared;
}

void SpringReverb::prepare(double sampleRate, int samplesPerBlock, int numChannels)
{
    juce::ignoreUnused(samplesPerBlock);
    sampleRate = std::max(8000.0, sampleRate);
    impl->sampleRate = sampleRate;
    impl->activeChannels = juce::jlimit(1, 2, numChannels);

    const float sr = static_cast<float>(sampleRate);
    // One sample at 48 kHz. A unit delay makes the allpass group delay monotonic
    // through the audio band, so the cascade is one falling chirp rather than a
    // rippled smear. Scaled so the chirp lasts the same time at every rate.
    const int baseStretch = std::max(1, static_cast<int>(std::lround(sampleRate / 48000.0)));
    for (int s = 0; s < kSprings; ++s)
    {
        impl->stretchLf[s] = baseStretch;
        impl->stretchHf[s] = baseStretch;
    }
    const int allpassCap = baseStretch + 8;

    const float maxMs = 68.0f * 1.08f + 8.0f;
    const int lineCap = static_cast<int>(std::ceil(maxMs * 0.001 * sampleRate)) + 32;
    impl->maxLineDelay = static_cast<float>(lineCap - 8);

    const int preCap = static_cast<int>(std::ceil(0.120 * sampleRate)) + 16;

    impl->dcR = std::exp(-juce::MathConstants<float>::twoPi * 35.0f / sr);
    impl->outHpR = std::exp(-juce::MathConstants<float>::twoPi * 80.0f / sr);

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& channel = impl->channels[static_cast<size_t>(ch)];
        channel.preDelay.prepare(preCap);
        channel.peak.clear();
        for (int s = 0; s < kSprings; ++s)
        {
            auto& spring = channel.springs[static_cast<size_t>(s)];
            spring.lowDelay.prepare(lineCap);
            spring.highDelay.prepare(lineCap);
            spring.baseLow = kDelayMs[s] * 0.001f * sr * kChDelay[ch][s];
            spring.baseHigh = spring.baseLow * kHfRatio[s];
            for (auto& stage : spring.lowDisp)
                stage.prepare(allpassCap);
            for (auto& stage : spring.highDisp)
                stage.prepare(allpassCap);
            for (auto& stage : spring.dripDisp)
                stage.prepare(allpassCap);
            const uint32_t seed = 0xA341316Cu
                                ^ (static_cast<uint32_t>(ch) * 0x9E3779B9u)
                                ^ (static_cast<uint32_t>(s + 1) * 0x85EBCA6Bu);
            spring.mod.prepare(seed);
        }
    }

    impl->clearAudio();
    impl->prepareSmoothers(true);
    impl->prepared = true;
}

void SpringReverb::reset()
{
    impl->clearAudio();
    if (impl->prepared)
        impl->prepareSmoothers(true);
}

void SpringReverb::setParameters(const Params& newParams, bool snap)
{
    Params p;
    p.decaySeconds = sanitize(newParams.decaySeconds, 2.2f, 0.15f, 5.5f);
    p.tension = sanitize(newParams.tension, 0.28f, 0.0f, 1.0f);
    p.tone = sanitize(newParams.tone, 0.58f, 0.0f, 1.0f);
    p.drip = sanitize(newParams.drip, 0.70f, 0.0f, 1.0f);
    p.preDelayMs = sanitize(newParams.preDelayMs, 0.0f, 0.0f, 100.0f);
    p.mix = sanitize(newParams.mix, 0.42f, 0.0f, 1.0f);
    p.outputDb = sanitize(newParams.outputDb, 0.0f, -24.0f, 12.0f);
    impl->params = p;

    if (!impl->prepared)
        return;

    impl->decaySm.setTargetValue(p.decaySeconds);
    impl->tensionSm.setTargetValue(p.tension);
    impl->toneSm.setTargetValue(p.tone);
    impl->dripSm.setTargetValue(p.drip);
    impl->preSm.setTargetValue(p.preDelayMs);
    impl->mixSm.setTargetValue(p.mix);
    impl->outSm.setTargetValue(p.outputDb);

    if (snap)
    {
        impl->decaySm.setCurrentAndTargetValue(p.decaySeconds);
        impl->tensionSm.setCurrentAndTargetValue(p.tension);
        impl->toneSm.setCurrentAndTargetValue(p.tone);
        impl->dripSm.setCurrentAndTargetValue(p.drip);
        impl->preSm.setCurrentAndTargetValue(p.preDelayMs);
        impl->mixSm.setCurrentAndTargetValue(p.mix);
        impl->outSm.setCurrentAndTargetValue(p.outputDb);
    }
}

void SpringReverb::process(juce::AudioBuffer<float>& buffer)
{
    if (!impl->prepared)
        return;

    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min(2, buffer.getNumChannels());
    if (numSamples <= 0 || numChannels <= 0)
        return;

    float* data[2] = { buffer.getWritePointer(0),
                       numChannels > 1 ? buffer.getWritePointer(1) : nullptr };

    for (int i = 0; i < numSamples; ++i)
    {
        const SharedCoeffs c = impl->tick();
        float dry[2] = { data[0][i], 0.0f };
        float wet[2] = { 0.0f, 0.0f };

        for (int ch = 0; ch < numChannels; ++ch)
        {
            dry[ch] = data[ch][i];
            wet[ch] = impl->processChannel(impl->channels[static_cast<size_t>(ch)],
                                           dry[ch], c, ch);
        }

        // A light crossfeed on the wet path only: one tank, two pickups,
        // while the dry guitar stays where the player put it.
        if (numChannels == 2)
        {
            constexpr float kCross = 0.10f;
            const float l = wet[0];
            const float r = wet[1];
            wet[0] = l * (1.0f - kCross) + r * kCross;
            wet[1] = r * (1.0f - kCross) + l * kCross;
        }

        for (int ch = 0; ch < numChannels; ++ch)
            data[ch][i] = (c.dryG * dry[ch] + c.wetG * wet[ch]) * c.outG;
    }
}

bool springReverbDspSelfTest(juce::String& failureReason)
{
    auto fail = [&failureReason](const juce::String& message)
    {
        failureReason = message;
        return false;
    };

    DelayLine line;
    line.prepare(64);
    std::vector<float> delayed(32, 0.0f);
    for (int n = 0; n < 32; ++n)
    {
        const float in = n == 0 ? 1.0f : 0.0f;
        delayed[static_cast<size_t>(n)] = line.readAge(9.0f);
        line.write(in);
    }
    if (std::abs(delayed[10] - 1.0f) > 1.0e-4f)
        return fail("Delay line of 10 samples did not land on sample 10.");
    if (std::abs(delayed[9]) > 1.0e-4f || std::abs(delayed[11]) > 1.0e-4f)
        return fail("Delay line leaked the impulse onto a neighbouring sample.");

    const auto pure = renderCascade(5, 4, 0.0f, 64);
    const int purePeak = argMaxAbs(pure);
    if (purePeak != 20)
        return fail("Allpass cascade with a=0 should be a pure delay of stages*K (peak at "
                    + juce::String(purePeak) + ", expected 20).");

    double inEnergy = 0.0;
    double outEnergy = 0.0;
    {
        std::array<StretchAllpass, 16> bank {};
        for (auto& stage : bank)
            stage.prepare(32);
        uint32_t rng = 0x12345678u;
        for (int i = 0; i < 6000; ++i)
        {
            rng = rng * 1664525u + 1013904223u;
            const float x = (static_cast<float>(rng >> 8) / 16777216.0f) * 2.0f - 1.0f;
            const float y = cascade(bank.data(), 8, x * 0.2f, 3, 0.55f);
            if (i >= 1000)
            {
                inEnergy += static_cast<double>(x * 0.2f) * (x * 0.2f);
                outEnergy += static_cast<double>(y) * y;
            }
        }
    }
    const double ratio = outEnergy / std::max(1.0e-12, inEnergy);
    if (ratio < 0.9 || ratio > 1.1)
        return fail("Allpass cascade energy ratio is " + juce::String(ratio, 3)
                    + " (expected near 1).");

    constexpr float kSr = 48000.0f;
    const auto negative = renderCascade(kDripStages, 1, -0.85f, 4096);
    const auto positive = renderCascade(kDripStages, 1, 0.85f, 4096);
    const float negLow = groupDelaySamples(negative, 400.0f, kSr);
    const float negHigh = groupDelaySamples(negative, 6500.0f, kSr);
    const float posLow = groupDelaySamples(positive, 400.0f, kSr);
    const float posHigh = groupDelaySamples(positive, 16000.0f, kSr);

    // H(z) = (a + z^{-d}) / (1 + a z^{-d}). Negative a holds lows back, so the
    // drip falls. Positive a does the opposite, mostly near the top of the band.
    if (!(negHigh + 80.0f < negLow))
        return fail("Negative-coefficient cascade should chirp downward (high delay "
                    + juce::String(negHigh, 1) + " samples, low delay " + juce::String(negLow, 1) + ").");
    if (!(posLow + 15.0f < posHigh))
        return fail("Positive-coefficient cascade should chirp upward (low delay "
                    + juce::String(posLow, 1) + " samples, high delay " + juce::String(posHigh, 1) + ").");

    failureReason.clear();
    return true;
}
