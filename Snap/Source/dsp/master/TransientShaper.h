#pragma once
#include "MasterConstants.h"
#include "../common/Smoother.h"
#include "../common/TransientMask.h"
#include <atomic>
#include <vector>

namespace snap
{
class TransientShaper
{
public:
    void prepare(double fs, int channels, int maxBlock, bool lowLatency)
    {
        normalLookahead = lookaheadFor(fs, false);
        mask.prepare(fs, channels, std::max(normalLookahead, constants::lowLatencyMaskLookahead));
        setLowLatency(lowLatency);
        transient.resize(static_cast<size_t>(maxBlock));
        attack.prepare(fs), sustain.prepare(fs);
        reset();
    }

    void reset()
    {
        mask.reset();
        attack.reset(attack.targetValue()), sustain.reset(sustain.targetValue());
        gainDb.store(0, std::memory_order_relaxed);
    }

    void setParams(float a, float s) { attack.setTarget(a), sustain.setTarget(s); }
    void setLowLatency(bool lowLatency) { mask.setLookahead(lowLatency ? constants::lowLatencyMaskLookahead : normalLookahead); }
    int latency() const { return mask.getLookahead(); }
    static int lookaheadFor(double fs, bool lowLatency)
    {
        return lowLatency ? constants::lowLatencyMaskLookahead
                          : static_cast<int>(std::lround(fs * constants::maskLookaheadMs / 1000));
    }

    void process(juce::AudioBuffer<float>& io, int n)
    {
        mask.process(io, n, transient.data());
        float peak = 0;
        if (attack.value() != 0 || sustain.value() != 0 || attack.isSmoothing() || sustain.isSmoothing())
        {
            const auto data = io.getArrayOfWritePointers();
            for (int i = 0; i < n; ++i)
            {
                const float t = transient[static_cast<size_t>(i)];
                const float db = masterConstants::shaperRangeDb * (attack.next() * t + sustain.next() * (1 - t));
                if (std::abs(db) > std::abs(peak))
                    peak = db;
                if (db != 0)
                {
                    const float gain = juce::Decibels::decibelsToGain(db);
                    for (int c = 0; c < io.getNumChannels(); ++c)
                        data[c][i] *= gain;
                }
            }
        }
        gainDb.store(peak, std::memory_order_relaxed);
    }

    float peakGainDb() const { return gainDb.load(std::memory_order_relaxed); }

private:
    TransientMask mask;
    std::vector<float> transient;
    Smoother attack, sustain;
    std::atomic<float> gainDb { 0 };
    int normalLookahead = 0;
};
} // namespace snap
