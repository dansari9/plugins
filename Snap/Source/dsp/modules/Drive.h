#pragma once
#include "DriveConstants.h"
#include "ModuleBase.h"
#include "../common/TargetFocusMix.h"

namespace snap
{
class Drive final : public ModuleBase
{
public:
    struct Params
    {
        float amount = 0, mix = 1, target = 0, focusLo = constants::focusMinHz, focusHi = constants::focusMaxHz;
        int mode = 0;
        float flavor = 0;
    };

    void setParams(const Params& p)
    {
        amount.setTarget(p.amount), flavor.setTarget(p.flavor);
        const auto nextMode = static_cast<size_t>(std::clamp(p.mode, 0, static_cast<int>(weights.size()) - 1));
        if (nextMode != mode)
        {
            mode = nextMode;
            for (size_t m = 0; m < weights.size(); ++m)
            {
                weights[m].reset(weights[m].value());
                weights[m].setTarget(m == mode ? 1.0f : 0.0f);
            }
        }
        tfm.setParams(p.mix, p.target, p.focusLo, p.focusHi);
    }

    void prepare(double fs, int channels, int maxBlock, bool lowLatency) override
    {
        using namespace driveConstants;
        tfm.prepare(fs, channels, maxBlock, lowLatency);
        amount.prepare(fs), flavor.prepare(fs);
        for (auto& weight : weights)
            weight.prepare(fs);

        const auto pole = [fs](double hz) {
            return static_cast<float>(-std::expm1(-juce::MathConstants<double>::twoPi * std::min(hz, fs * constants::crossoverMaxFraction) / fs));
        };
        bass = pole(crunchBassHz), emphasis = pole(reelEmphasisHz), magnet = pole(reelMemoryHz);
        bumpLo = pole(reelBumpLoHz), bumpHi = pole(reelBumpHiHz), carrier = pole(wobbleCarrierHz);
        dc = pole(dcBlockHz), level = static_cast<float>(-std::expm1(-1000.0 / (levelMatchMs * fs)));
        for (size_t k = 0; k < 2; ++k)
        {
            pocketRoll[k] = pole(pocket[k].rolloffHz), reelRoll[k] = pole(reel[k].rolloffHz), glowRoll[k] = pole(glow[k].rolloffHz);
            holdStep[k] = std::min(1.0, crumble[k].rateHz / fs);
        }
        compAttack = EnvelopeFollower::coef(fs, pocketAttackMs), compRelease = EnvelopeFollower::coef(fs, pocketReleaseMs);
        delayCentre = static_cast<float>(fs * wobbleDelaySeconds), delaySwing = static_cast<float>(fs * wobbleSwingSeconds);
        delay.setSize(channels, static_cast<int>(std::ceil(delayCentre + delaySwing)) + 2);
        reset();
    }

    void reset() override
    {
        tfm.reset(), delay.clear();
        state = {};
        amount.reset(amount.targetValue()), flavor.reset(flavor.targetValue());
        for (size_t m = 0; m < weights.size(); ++m)
            weights[m].reset(m == mode ? 1.0f : 0.0f);
    }

    int latency() const override { return tfm.latency(); }
    void setLowLatency(bool low) override { tfm.setLowLatency(low); }

    void process(juce::AudioBuffer<float>& io, int n) override
    {
        const juce::ScopedNoDenormals noDenormals;
        tfm.begin(io, n);
        inMeter = addedMeter = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = amount.next(), f = 0.5f * (flavor.next() + 1.0f);
            std::array<float, driveConstants::flavorLabels.size()> w;
            for (size_t m = 0; m < w.size(); ++m)
                w[m] = weights[m].next();
            for (int c = 0; c < io.getNumChannels(); ++c)
            {
                const float x = io.getSample(c, i);
                float delta = 0;
                auto& s = state[static_cast<size_t>(c)];
                for (size_t m = 0; m < w.size(); ++m)
                    if (w[m] > 0.0f)
                        delta += w[m] * (render(c, s, m, x, a, f) - x);
                inMeter = std::max(inMeter, std::abs(x));
                if (a > 0.0f)
                    io.setSample(c, i, x + a * delta), addedMeter = std::max(addedMeter, std::abs(a * delta));
            }
        }
        tfm.end(io, n);
    }
    // Audio thread, for the display: the largest input sample and the largest change the shaping made (before mix,
    // target and focus) in the last process call.
    float inPeak() const { return inMeter; }
    float addedPeak() const { return addedMeter; }

private:
    struct Tape { float pre = 0, memory = 0, de = 0, lo = 0, hi = 0, roll = 0; };
    struct Hold { double phase = 1; float sample = 0; };
    struct Level { float dry = 0, wet = 0, offset = 0; };
    struct Channel
    {
        float bass = 0, pocketEnvelope = 0, carrier = 0;
        std::array<float, 2> pocketRoll {}, glowRoll {};
        std::array<Tape, 2> tape;
        std::array<Hold, 2> hold;
        std::array<Level, driveConstants::flavorLabels.size()> levels;
        int write = 0;
    };

    static float lowpass(float x, float coefficient, float& z) { return z += coefficient * (x - z); }

    static float biasedClip(float x, float bias, float drive)
    {
        return (std::tanh(drive * x + bias) - std::tanh(bias)) / drive;
    }

    float render(int c, Channel& s, size_t m, float x, float a, float f)
    {
        using namespace driveConstants;
        float y = x;
        switch (m)
        {
            case 0:
            {
                const float low = lowpass(x, bass, s.bass);
                const auto voice = [=](const CrunchVoice& v) {
                    const float pre = x + a * (v.bass * low + v.treble * (x - low));
                    const float clipped = biasedClip(pre, a * v.bias, std::lerp(1.0f, v.inputDrive * driveScale, a));
                    return biasedClip(clipped, 0, std::lerp(1.0f, v.outputDrive, a));
                };
                y = std::lerp(voice(crunch[0]), voice(crunch[1]), f);
                break;
            }
            case 1:
            {
                const float mag = std::abs(x);
                const float env = lowpass(mag, 1.0f - (mag > s.pocketEnvelope ? compAttack : compRelease), s.pocketEnvelope);
                const auto voice = [&](size_t k) {
                    const auto& v = pocket[k];
                    const float compression = 1.0f + a * v.compand * env;
                    const float clipped = biasedClip(x / compression, a * v.bias, std::lerp(1.0f, v.drive * driveScale, a));
                    return lowpass(clipped * compression, pocketRoll[k], s.pocketRoll[k]);
                };
                y = std::lerp(voice(0), voice(1), f);
                break;
            }
            case 2:
            {
                const auto voice = [&](size_t k) {
                    const auto& v = reel[k];
                    auto& t = s.tape[k];
                    const float pre = x + v.emphasis * (x - lowpass(x, emphasis, t.pre));
                    const float drive = std::lerp(1.0f, v.drive * driveScale, a), feedback = a * v.memory;
                    const float recorded = std::tanh(drive * pre + feedback * t.memory);
                    lowpass(recorded, magnet, t.memory);
                    const float playback = recorded * ((1.0f - feedback) / drive);
                    const float flat = playback - (v.emphasis / (1.0f + v.emphasis)) * (playback - lowpass(playback, emphasis, t.de));
                    const float hi = lowpass(flat, bumpHi, t.hi), lo = lowpass(flat, bumpLo, t.lo);
                    return lowpass(flat + v.bump * (hi - lo), reelRoll[k], t.roll);
                };
                y = std::lerp(voice(0), voice(1), f);
                break;
            }
            case 3:
            {
                const auto voice = [&](size_t k) {
                    const auto& v = glow[k];
                    const float grid = std::max(0.0f, x - v.gridKnee);
                    const float bent = x - grid + grid / (1.0f + a * v.gridCompression * grid);
                    return lowpass(biasedClip(bent, a * v.bias, std::lerp(1.0f, v.drive * driveScale, a)), glowRoll[k], s.glowRoll[k]);
                };
                y = std::lerp(voice(0), voice(1), f);
                break;
            }
            case 4:
            {
                const auto voice = [&](size_t k) {
                    auto& h = s.hold[k];
                    if (h.phase >= 1.0)
                    {
                        h.phase -= 1.0;
                        h.sample = std::round(x * crumble[k].levels) / crumble[k].levels;
                    }
                    h.phase += holdStep[k];
                    return h.sample;
                };
                y = std::lerp(voice(0), voice(1), f);
                break;
            }
            case 5:
            {
                const float side = x - lowpass(x, carrier, s.carrier);
                const float ring = std::lerp(x, x * std::tanh(wobbleCarrierDrive * side), wobbleRingDepth);
                const float tap = delayCentre + delaySwing * std::tanh(wobbleDelayDrive * x);
                const int size = delay.getNumSamples(), whole = static_cast<int>(tap);
                auto* samples = delay.getWritePointer(c);
                samples[s.write] = x;
                const int first = (s.write + size - whole) % size, second = (first + size - 1) % size;
                const float delayed = std::lerp(samples[first], samples[second], tap - static_cast<float>(whole));
                s.write = (s.write + 1) % size;
                y = std::lerp(ring, std::lerp(x, delayed, wobblePhaseDepth), f);
                break;
            }
        }

        auto& match = s.levels[m];
        // Remove the asymmetric curves' DC and match their short-term power with bounded gain.
        y -= lowpass(y, dc, match.offset);
        const float dryPower = lowpass(x * x, level, match.dry), wetPower = lowpass(y * y, level, match.wet);
        return y * std::clamp(std::sqrt((dryPower + levelFloor) / (wetPower + levelFloor)), minMakeup, maxMakeup);
    }

    TargetFocusMix tfm;
    Smoother amount, flavor;
    std::array<Smoother, driveConstants::flavorLabels.size()> weights;
    std::array<Channel, 2> state;
    juce::AudioBuffer<float> delay;
    size_t mode = 0;
    float inMeter = 0, addedMeter = 0;
    float bass = 0, emphasis = 0, magnet = 0, bumpLo = 0, bumpHi = 0, carrier = 0, dc = 0, level = 0;
    float compAttack = 0, compRelease = 0, delayCentre = 0, delaySwing = 0;
    std::array<float, 2> pocketRoll {}, reelRoll {}, glowRoll {};
    std::array<double, 2> holdStep {};
};
} // namespace snap
