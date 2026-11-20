#pragma once
#include "Air.h"
#include "Clipper.h"
#include "CutFilter.h"
#include "ToneShapes.h"
#include "TransientShaper.h"
#include "../common/LoudnessMeter.h"

namespace snap
{
class Master
{
public:
    struct Params
    {
        bool eqOn = true;
        float cutLo = masterConstants::cutLoOpenHz, cutHi = masterConstants::cutHiOpenHz;
        int lcShape = 0, hcShape = 0;
        float tone = 0;
        int toneShape = 0;
        float air = 0;
        float attack = 0, sustain = 0;
        bool clipOn = false;
        float outDb = 0;
        bool lowLatency = false;
    };

    Master() { setParams({}); }

    void setParams(const Params& p)
    {
        if (prepared && p.lowLatency != lowLatency)
        {
            shaper.setLowLatency(p.lowLatency);
            clip.setLowLatency(p.lowLatency);
        }
        lowLatency = p.lowLatency;
        eqMix.setTarget(p.eqOn ? 1.0f : 0.0f);
        cut.setParams(p.cutLo, p.cutHi, p.lcShape, p.hcShape);
        tone.setParams(p.tone, p.toneShape);
        air.setAmount(p.air);
        shaper.setParams(p.attack, p.sustain);
        clip.setOn(p.clipOn);
        outputDb.setTarget(p.outDb);
    }

    void prepare(double fs, int channels, int maxBlock)
    {
        eqMix.prepare(fs), outputDb.prepare(fs);
        eqMix.reset(eqMix.targetValue()), resetOutputGain();
        cut.prepare(fs), tone.prepare(fs), air.prepare(fs, channels);
        shaper.prepare(fs, channels, maxBlock, lowLatency);
        clip.prepare(fs, channels, maxBlock, lowLatency);
        meter.prepare(fs, channels);
        prepared = true;
    }
    void reset()
    {
        cut.reset(), tone.reset(), air.reset(), shaper.reset(), clip.reset(), meter.reset();
        eqMix.reset(eqMix.targetValue()), resetOutputGain();
    }

    int latency() const { return shaper.latency() + clip.latency(); }
    static int latencyFor(double fs, bool lowLatency)
    {
        return TransientShaper::lookaheadFor(fs, lowLatency) + Clipper::latencyFor(lowLatency);
    }

    void process(juce::AudioBuffer<float>& io, int n, const float* const* post)
    {
        const juce::ScopedNoDenormals noDenormals;
        const bool stereo = io.getNumChannels() == 2;
        auto* left = io.getWritePointer(0);
        auto* right = stereo ? io.getWritePointer(1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            cut.step();
            const float w = eqMix.next();
            const float dryLeft = left[i], dryRight = stereo ? right[i] : 0;
            float l = cut.processSample(0, dryLeft), r = stereo ? cut.processSample(1, dryRight) : 0;
            tone.process(l, r, stereo);
            if (w == 0)
                l = dryLeft, r = dryRight;
            else if (w != 1)
                l = dryLeft + w * (l - dryLeft), r = dryRight + w * (r - dryRight);

            air.process(l, r, stereo);
            left[i] = l;
            if (stereo)
                right[i] = r;
        }

        shaper.process(io, n);
        if (post != nullptr)
            for (int c = 0; c < io.getNumChannels(); ++c)
                io.addFrom(c, 0, post[c], n);

        clip.process(io, n);
        meter.process(io, n);
        if (!outputDb.isSmoothing())
        {
            if (outputGain != 1)
                io.applyGain(0, n, outputGain);
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            if (outputDb.isSmoothing())
                outputGain = juce::Decibels::decibelsToGain(outputDb.next());
            if (outputGain != 1)
            {
                left[i] *= outputGain;
                if (stereo)
                    right[i] *= outputGain;
            }
        }
    }

    float preOutputLufs() const { return meter.lufs(); }
    bool clipping() const { return clip.clipping(); }
    float shaperGainDb() const { return shaper.peakGainDb(); }

private:
    void resetOutputGain()
    {
        outputDb.reset(outputDb.targetValue());
        outputGain = juce::Decibels::decibelsToGain(outputDb.value());
    }

    CutFilter cut;
    ToneShapes tone;
    Air air;
    TransientShaper shaper;
    Clipper clip;
    LoudnessMeter meter;
    Smoother eqMix, outputDb;
    float outputGain = 1;
    bool prepared = false, lowLatency = false;
};
} // namespace snap
