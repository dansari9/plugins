#pragma once
#include "FocusSplitter.h"
#include "TransientMask.h"
#include <vector>

namespace snap
{
// The shared wrapper for modules with Mix, Target and Focus (drive, push). begin() measures the mask on the full input,
// delays the audio by the lookahead (that delay is the module's latency) and leaves only the Focus band in the buffer.
// The module then processes the band in place with no further latency, and end() adds the band's change, weighted by
// Mix x Target, to the delayed input. An unprocessed band is an exact pass-through for any Focus.
class TargetFocusMix
{
public:
    TargetFocusMix() { mix.reset(1); }
    void prepare(double fs, int channels, int maxBlock, bool lowLatency)
    {
        sampleRate = fs;
        mask.prepare(fs, channels, std::max(lookaheadFor(fs, false), lookaheadFor(fs, true)));
        mask.setLookahead(lookaheadFor(fs, lowLatency));
        focus.prepare(fs, channels, maxBlock, 0); // starts at the last edges set, without a glide
        mix.prepare(fs), target.prepare(fs);
        transient.assign((size_t) maxBlock, 0), weight.assign((size_t) maxBlock, 0);
        reset();
    }
    void reset() { mask.reset(), focus.reset(), mix.reset(mix.targetValue()), target.reset(target.targetValue()); }
    int latency() const { return mask.getLookahead(); }
    void setLowLatency(bool low) { mask.setLookahead(lookaheadFor(sampleRate, low)), reset(); } // audio thread, no allocation
    static int lookaheadFor(double fs, bool lowLatency)
    {
        return lowLatency ? constants::lowLatencyMaskLookahead : (int) std::lround(fs * constants::maskLookaheadMs * 0.001);
    }

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
    double sampleRate = 48000;
};
} // namespace snap
