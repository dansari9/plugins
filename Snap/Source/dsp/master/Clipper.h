#pragma once
#include "MasterConstants.h"
#include "../common/Delay.h"
#include "../common/Oversampler.h"
#include "../common/Smoother.h"
#include <atomic>

namespace snap
{
class Clipper
{
public:
    void prepare(double fs, int channels, int maxBlock, bool lowLatency)
    {
        using namespace masterConstants;
        fir.prepare(channels, maxBlock, clipOversamplingStages, false);
        iir.prepare(channels, maxBlock, clipOversamplingStages, true);
        dryDelay.prepare(channels, std::max(fir.latency(), iir.latency()));
        dry.setSize(channels, maxBlock);
        knee = juce::Decibels::decibelsToGain(clipCeilingDb - clipKneeDb);
        ceiling = juce::Decibels::decibelsToGain(clipCeilingDb);
        reconstructionKnee = juce::Decibels::decibelsToGain(clipCeilingDb - clipReconstructionKneeDb);
        mix.prepare(fs);
        active = nullptr;
        setLowLatency(lowLatency);
        reset();
    }
    void reset()
    {
        fir.reset(), iir.reset(), dryDelay.reset();
        mix.reset(mix.targetValue());
        reduced.store(false, std::memory_order_relaxed);
    }
    void setOn(bool on) { mix.setTarget(on ? 1.0f : 0.0f); }
    void setLowLatency(bool lowLatency)
    {
        auto* next = lowLatency ? &iir : &fir;
        if (active == next)
            return;

        active = next;
        active->reset();
        dryDelay.setDelay(active->latency());
    }

    static int latencyFor(bool lowLatency)
    {
        if (lowLatency)
        {
            static const int samples = measureLatency(true);
            return samples;
        }
        static const int samples = measureLatency(false);
        return samples;
    }
    int latency() const { return active != nullptr ? active->latency() : 0; }

    void process(juce::AudioBuffer<float>& io, int n)
    {
        if (n == 0)
        {
            reduced.store(false, std::memory_order_relaxed);
            return;
        }

        const auto wet = juce::dsp::AudioBlock<float>(io).getSubBlock(0, static_cast<size_t>(n));
        const auto up = active->up(wet);
        bool clipped = false;
        for (size_t c = 0; c < up.getNumChannels(); ++c)
        {
            auto* samples = up.getChannelPointer(c);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                const float x = samples[i], mag = std::abs(x);
                if (mag > knee)
                {
                    samples[i] = limit(x, mag, knee);
                    clipped = true;
                }
            }
        }

        const bool wetAudible = mix.value() > 0 || mix.targetValue() > 0;
        reduced.store(wetAudible && clipped, std::memory_order_relaxed);
        for (int c = 0; c < io.getNumChannels(); ++c)
        {
            const auto* input = io.getReadPointer(c);
            auto* delayed = dry.getWritePointer(c);
            for (int i = 0; i < n; ++i)
                delayed[i] = dryDelay.next(c, input[i]);
        }
        active->down(wet);

        const auto output = io.getArrayOfWritePointers();
        const auto delayed = dry.getArrayOfReadPointers();
        for (int i = 0; i < n; ++i)
        {
            const float w = mix.next();
            for (int c = 0; c < io.getNumChannels(); ++c)
            {
                const float drySample = delayed[c][i];
                if (w == 0)
                    output[c][i] = drySample;
                else
                {
                    // Round off any overshoot from the reconstruction filter.
                    const float x = output[c][i], mag = std::abs(x);
                    const float limited = mag > reconstructionKnee ? limit(x, mag, reconstructionKnee) : x;
                    output[c][i] = w == 1 ? limited : drySample + w * (limited - drySample);
                }
            }
        }
    }
    bool clipping() const { return reduced.load(std::memory_order_relaxed); }

private:
    float limit(float x, float magnitude, float start) const
    {
        const float width = ceiling - start;
        return std::copysign(start + width * std::tanh((magnitude - start) / width), x);
    }

    static int measureLatency(bool lowLatency)
    {
        Oversampler os;
        os.prepare(1, 1, masterConstants::clipOversamplingStages, lowLatency);
        return os.latency();
    }

    Oversampler fir, iir;
    Oversampler* active = nullptr;
    Delay dryDelay;
    Smoother mix;
    juce::AudioBuffer<float> dry;
    std::atomic<bool> reduced { false };
    float knee = 0, ceiling = 0, reconstructionKnee = 0;
};
} // namespace snap
