#pragma once
#include "ModuleBase.h"
#include "PushConstants.h"
#include "../common/TargetFocusMix.h"

namespace snap
{
class Push final : public ModuleBase
{
public:
    struct Params
    {
        float amount = 0, mix = 1, target = 0, focusLo = constants::focusMinHz, focusHi = constants::focusMaxHz;
        int mult = 0;
        float tone = 0;
    };

    void setParams(const Params& p)
    {
        amount.setTarget(p.amount), tone.setTarget(p.tone);
        const auto nextMult = static_cast<size_t>(std::clamp(p.mult, 0, static_cast<int>(weights.size()) - 1));
        if (nextMult != mult)
        {
            mult = nextMult;
            for (size_t k = 0; k < weights.size(); ++k)
            {
                weights[k].reset(weights[k].value());
                weights[k].setTarget(k == mult ? 1.0f : 0.0f);
            }
        }
        tfm.setParams(p.mix, p.target, p.focusLo, p.focusHi);
    }

    void prepare(double fs, int channels, int maxBlock, bool lowLatency) override
    {
        using namespace pushConstants;
        tfm.prepare(fs, channels, maxBlock, lowLatency);
        amount.prepare(fs), tone.prepare(fs);
        for (auto& weight : weights)
            weight.prepare(fs);
        const double hz = std::min(tonePivotHz, fs * constants::crossoverMaxFraction);
        pole = static_cast<float>(-std::expm1(-juce::MathConstants<double>::twoPi * hz / fs));
        for (auto& channel : stages)
            for (auto& stage : channel)
            {
                stage.reference.prepare(fs, peakAttackMs, peakReleaseMs);
                stage.limiter.prepare(fs, peakAttackMs, peakReleaseMs);
            }
        reset();
    }

    void reset() override
    {
        tfm.reset();
        for (auto& channel : stages)
            for (auto& stage : channel)
            {
                stage.pre = stage.post = 0;
                stage.reference.reset(), stage.limiter.reset();
            }
        amount.reset(amount.targetValue()), tone.reset(tone.targetValue());
        for (size_t k = 0; k < weights.size(); ++k)
            weights[k].reset(k == mult ? 1.0f : 0.0f);
    }

    int latency() const override { return tfm.latency(); }

    void process(juce::AudioBuffer<float>& io, int n) override
    {
        using namespace pushConstants;
        const juce::ScopedNoDenormals noDenormals;
        tfm.begin(io, n);
        for (int i = 0; i < n; ++i)
        {
            const float a = amount.next(), high = std::exp2(toneOctaves * tone.next());
            const float shelf = 1.0f / high - high, inverse = 1.0f / (high + shelf * pole);
            const float drive = std::lerp(minDrive, maxDrive, a), makeup = 1.0f / std::tanh(drive);
            std::array<float, stageCounts.size()> w;
            size_t last = 0;
            for (size_t k = 0; k < w.size(); ++k)
                if ((w[k] = weights[k].next()) > 0.0f)
                    last = k;

            for (int c = 0; c < io.getNumChannels(); ++c)
            {
                const float x = io.getSample(c, i);
                float y = x, delta = 0;
                int stage = 0;
                for (size_t k = 0; k <= last; ++k)
                {
                    for (; stage < stageCounts[k]; ++stage)
                        y = runStage(stages[static_cast<size_t>(c)][static_cast<size_t>(stage)], y, a, high, shelf, inverse, drive, makeup);
                    delta += w[k] * (y - x);
                }
                if (a > 0.0f)
                    io.setSample(c, i, x + delta);
            }
        }
        tfm.end(io, n);
    }

private:
    struct Stage
    {
        EnvelopeFollower reference, limiter;
        float pre = 0, post = 0;
    };

    float runStage(Stage& s, float x, float a, float high, float shelf, float inverse, float drive, float makeup) const
    {
        using namespace pushConstants;
        s.pre += pole * (x - s.pre);
        const float pre = high * x + shelf * s.pre;

        // Anchor the curve at the recent peak so drive increases density while keeping peaks in place.
        const float mag = std::abs(pre);
        const float ref = std::max({ referenceFloor, mag, s.reference.next(mag) });
        float y = ref * std::tanh(drive * (pre / ref)) * makeup;
        const float peak = s.limiter.next(std::abs(y));
        y *= limiterThreshold / std::max(limiterThreshold, peak);
        if (const float excess = std::abs(y) - clipKnee; excess > 0.0f)
            y = std::copysign(clipKnee + excess / (1.0f + excess / (clipCeiling - clipKnee)), y);

        // Solve the pre-shelf equation for its input using the inverse's previous low-pass state.
        const float post = (y - shelf * (1.0f - pole) * s.post) * inverse;
        s.post += pole * (post - s.post);
        return x + a * (post - x);
    }

    TargetFocusMix tfm;
    Smoother amount, tone;
    std::array<Smoother, pushConstants::stageCounts.size()> weights;
    std::array<std::array<Stage, pushConstants::stageCounts.back()>, 2> stages;
    size_t mult = 0;
    float pole = 0;
};
} // namespace snap
