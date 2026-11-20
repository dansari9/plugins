#pragma once
#include "FocusSplitter.h"
#include "TransientMask.h"
#include <vector>

namespace snap
{
// The shared wrapper for modules with Mix, Target and Focus (drive, push). begin() measures the mask on the full input,
// delays the audio by the lookahead (that delay is the module's latency) and leaves only the Focus band in the buffer.
// The module then processes the band in place with no further latency, and end() puts the rest back and crossfades the
// processed band against its own dry by Mix x Target. Full-range Focus with an unprocessed band is an exact pass-through.
class TargetFocusMix
{
public:
    TargetFocusMix() { mix.reset(1); }
    void prepare(double fs, int channels, int maxBlock, bool lowLatency)
    {
        const int lookahead = lowLatency ? constants::lowLatencyMaskLookahead : (int) std::lround(fs * constants::maskLookaheadMs * 0.001);
        mask.prepare(fs, channels, lookahead), mask.setLookahead(lookahead);
        focus.prepare(fs, channels, maxBlock, 0); // starts at the last edges set, without a glide
        mix.prepare(fs), target.prepare(fs);
        transient.assign((size_t) maxBlock, 0), weight.assign((size_t) maxBlock, 0);
        reset();
    }
    void reset() { mask.reset(), focus.reset(), mix.reset(mix.targetValue()), target.reset(target.targetValue()); }
    int latency() const { return mask.getLookahead(); }

    // mix 0..1, target -1..1, focus edges in Hz. Set before prepare() to start there without a ramp.
    void setParams(float mixAmount, float targetPosition, float focusLoHz, float focusHiHz)
    {
        mix.setTarget(mixAmount), target.setTarget(targetPosition), focus.setEdges(focusLoHz, focusHiHz);
    }

    void begin(juce::AudioBuffer<float>& io, int n)
    {
        mask.process(io, n, transient.data());
        focus.split(io, n);
    }
    void end(juce::AudioBuffer<float>& io, int n)
    {
        for (size_t i = 0; i < (size_t) n; ++i)
            weight[i] = mix.next() * TransientMask::weight(transient[i], target.next());
        focus.merge(io, weight.data(), n);
    }

private:
    TransientMask mask;
    FocusSplitter focus = [] {
        FocusSplitter f;
        f.setEdges(constants::focusMinHz, constants::focusMaxHz);
        return f;
    }();
    Smoother mix, target;
    std::vector<float> transient, weight;
};
} // namespace snap
