#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace snap
{
// What the chain sees of a module. prepare() runs on the message thread and may allocate; everything else is audio-thread
// safe. Each module also has a plain `Params` struct (normalized values: amounts/mix 0..1, bipolar -1..1, Hz, indices) set
// once per block with setParams() before process(); continuous values are smoothed inside the module.
class ModuleBase
{
public:
    virtual ~ModuleBase() = default;
    virtual void prepare(double fs, int channels, int maxBlock, bool lowLatency) = 0;
    virtual void reset() = 0;
    virtual int latency() const = 0; // samples, fixed between prepare() / setLowLatency() calls
    virtual void setLowLatency(bool) {} // audio thread: switch Low Latency Mode without allocating (prepare() covers both)
    virtual void process(juce::AudioBuffer<float>& io, int n) = 0; // in place, n <= maxBlock
};
} // namespace snap
