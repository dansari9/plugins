#pragma once
#include "../dsp/common/TargetFocusMix.h"
#include "../dsp/modules/Shift.h"

namespace snap
{
// The plug-in's one reported latency. Every module is padded to its own maximum, so turning modules on or off or
// reordering them never changes it; Low Latency Mode is the exception and reports exactly 64 samples.
// Sums the modules that have latency: shift (STFT) and the Target lookahead of drive and push; press has none.
static_assert(2 * constants::lowLatencyMaskLookahead <= constants::lowLatencySamples, "the Low Latency Mode budget holds drive and push");

inline int chainLatency(double fs, bool lowLatency)
{
    if (lowLatency)
        return constants::lowLatencySamples;
    return Shift::latencyFor(fs, false) + 2 * TargetFocusMix::lookaheadFor(fs, false);
}
} // namespace snap
