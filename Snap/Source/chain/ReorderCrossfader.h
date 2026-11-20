#pragma once
#include "../dsp/common/Constants.h"
#include "../dsp/modules/Drive.h"
#include "../dsp/modules/Press.h"
#include "../dsp/modules/Shift.h"
#include "../dsp/modules/Space.h"
#include "ChainOrder.h"

namespace snap
{
// The four reorderable modules, run in a given order.
struct MiddleChain
{
    Shift shift;
    Space space;
    Press press;
    Drive drive;

    void prepare(double fs, int channels, int maxBlock, bool lowLatency, const Space::Settings& initial)
    {
        space.prepare({ fs, (juce::uint32) maxBlock, (juce::uint32) channels }, initial);
        for (ModuleBase* m : std::array<ModuleBase*, 3> { &shift, &press, &drive })
            m->prepare(fs, channels, maxBlock, lowLatency);
    }
    void reset()
    {
        space.reset();
        for (ModuleBase* m : std::array<ModuleBase*, 3> { &shift, &press, &drive })
            m->reset();
    }
    void setLowLatency(bool low)
    {
        for (ModuleBase* m : std::array<ModuleBase*, 3> { &shift, &press, &drive })
            m->setLowLatency(low);
    }
    int latency() const { return shift.latency() + space.getLatencySamples() + press.latency() + drive.latency(); }

    // io holds exactly the samples to process.
    void process(const ChainOrder& order, juce::AudioBuffer<float>& io, const Space::Settings& spaceSettings)
    {
        const int n = io.getNumSamples();
        for (auto m : order)
            switch (m)
            {
                case Module::shift: shift.process(io, n); break;
                case Module::space: space.process(io, spaceSettings); break;
                case Module::press: press.process(io, n); break;
                case Module::drive: drive.process(io, n); break;
                case Module::subHiss:
                case Module::push: break;
            }
    }
};

// Click-free reorder. Two copies of the middle chain: the active one runs the current order. On a new order the spare
// one is reset (press keeps its level detectors, which settle slowly) and runs the new order on the same input,
// silently, for its latency plus a warm-up, then the outputs
// crossfade linearly (both have the same latency, so they are sample-aligned) and the spare becomes the active one.
// A request that arrives during a transition waits for it to finish. Audio thread only, apart from prepare().
class ReorderCrossfader
{
public:
    std::array<MiddleChain, 2> sets; // the processor sets parameters on both every block

    void prepare(double fs, int channels, int maxBlock, bool lowLatency, const Space::Settings& initial, const ChainOrder& order)
    {
        for (auto& s : sets)
            s.prepare(fs, channels, maxBlock, lowLatency, initial);
        spare.setSize(channels, maxBlock);
        sampleRate = fs, active = 0, running = order, phase = Phase::idle;
        fadeLength = (int) std::lround(fs * constants::reorderFadeMs * 0.001);
    }
    void setLowLatency(bool low)
    {
        for (auto& s : sets)
            s.setLowLatency(low);
    }
    int latency() const { return sets[0].latency(); }
    const ChainOrder& order() const { return running; }
    bool transitioning() const { return phase != Phase::idle; }
    int fadeSamples() const { return fadeLength; }
    const MiddleChain& current() const { return sets[active]; } // audio thread

    void process(const ChainOrder& wanted, juce::AudioBuffer<float>& io, const Space::Settings& spaceSettings)
    {
        const int n = io.getNumSamples(), channels = io.getNumChannels();
        auto& next = sets[1 - active];
        if (phase == Phase::idle && wanted != running)
        {
            next.reset();
            next.press.seedDetectors(sets[active].press);
            pending = wanted, phase = Phase::warming, position = 0;
            warmup = next.latency() + (int) std::lround(sampleRate * constants::reorderWarmupMs * 0.001);
        }
        if (phase == Phase::idle)
        {
            sets[active].process(running, io, spaceSettings);
            juce::AudioBuffer<float> none(spare.getArrayOfWritePointers(), channels, 0); // keeps the spare's IR request current
            next.space.process(none, spaceSettings);
            return;
        }
        juce::AudioBuffer<float> other(spare.getArrayOfWritePointers(), channels, n);
        for (int c = 0; c < channels; ++c)
            other.copyFrom(c, 0, io, c, 0, n);
        sets[active].process(running, io, spaceSettings);
        next.process(pending, other, spaceSettings);
        for (int i = 0; i < n; ++i, ++position)
        {
            if (phase == Phase::warming && position >= warmup)
                phase = Phase::fading, position = 0;
            const float g = phase == Phase::fading ? std::min(1.0f, (float) position / (float) fadeLength) : 0.0f;
            for (int c = 0; c < channels; ++c)
                io.setSample(c, i, io.getSample(c, i) + g * (other.getSample(c, i) - io.getSample(c, i)));
        }
        if (phase == Phase::fading && position >= fadeLength)
            active = 1 - active, running = pending, phase = Phase::idle;
    }

private:
    enum class Phase { idle, warming, fading };
    juce::AudioBuffer<float> spare;
    double sampleRate = 48000;
    size_t active = 0;
    int warmup = 0, position = 0, fadeLength = 1;
    ChainOrder running = defaultChainOrder, pending = defaultChainOrder;
    Phase phase = Phase::idle;
};
} // namespace snap
