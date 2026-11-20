#pragma once
#include "../dsp/common/TargetFocusMix.h"
#include "../dsp/master/Master.h"
#include "../dsp/modules/Shift.h"
#include "../dsp/modules/SubHiss.h"

namespace snap
{
// The plug-in's one reported latency. Every module is padded to its own maximum, so turning modules on or off or
// reordering them never changes it; Low Latency Mode is the exception and reports exactly 64 samples.
// Sums the stages that have latency: the sub/hiss detector lookahead, shift (STFT), the Target lookahead of drive and push
// (space and press have none) and the master (shaper lookahead plus the Clip oversampler). SnapTests checks that the Low
// Latency Mode stages fit in the 64 samples; the processor pads the rest.
inline int chainLatency(double fs, bool lowLatency)
{
    if (lowLatency)
        return constants::lowLatencySamples;
    return SubHiss::latencyFor(fs, false) + Shift::latencyFor(fs, false) + 2 * TargetFocusMix::lookaheadFor(fs, false)
           + Master::latencyFor(fs, false);
}
} // namespace snap
