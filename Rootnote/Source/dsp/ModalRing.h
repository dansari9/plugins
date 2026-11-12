#pragma once

#include "Series.h"
#include "Smoothers.h"
#include "TransientGate.h"
#include "Weighting.h"

namespace rootnote
{
/**
    The Ring engine: a bank of up to 16 modal resonators that only the start
    of each hit can excite, so the tail length is set in milliseconds and does
    not depend on the bell Q.

    Each mode is one complex pole z[n] = p z[n-1] + x[n], p = r e^(j theta),
    run as a 2x2 rotation. It has the same poles as the two-pole recursion
    y = 2 r cos(theta) y1 - r^2 y2 + x, but the state is a rotating vector whose
    length is scaled by exactly r each sample whatever theta does. That keeps
    the amplitude steady while the pitch glides (Drop, MIDI glide), where the
    direct recursion jumps.

    Modes sit on the boost series and take the boost weights (range, blend,
    tilt). Audio-thread safe after prepare().
*/
class ModalRing
{
public:
    static constexpr int kMaxModes = 16;
    /** Higher modes decay faster: T60_k = T60 / ratio^kDamping. */
    static constexpr double kDamping = 0.7;

    struct Settings
    {
        BoostShape shape;
        double decaySeconds = 0.6; // T60 of the root
        double dropSemitones = 0.0;

        bool operator==(const Settings& o) const noexcept
        {
            return shape.series == o.shape.series && shape.bands == o.shape.bands && shape.blend == o.shape.blend
                && shape.focusLink == o.shape.focusLink && shape.focusHz == o.shape.focusHz
                && shape.rangeOct == o.shape.rangeOct && shape.tiltDbPerOct == o.shape.tiltDbPerOct
                && decaySeconds == o.decaySeconds && dropSemitones == o.dropSemitones;
        }
    };

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setSettings(const Settings& newSettings) noexcept;
    void setTargetFrequency(double hz, double rampSeconds) noexcept;

    /**
        Reads the dry input, writes the ring signal. inRight and outRight may
        be null for mono. The outputs must not alias the inputs.
    */
    void process(const float* inLeft, const float* inRight, double* outLeft, double* outRight, int numSamples) noexcept;

    /** Slot of the boost series that mode m follows. */
    static int slotForMode(SeriesType series, int mode) noexcept
    {
        // Octaves: modes cover 2^-4 .. 2^11, which reaches below 18 Hz and
        // above the audible range for every root.
        return series == SeriesType::octaves ? mode + kOctaveRootSlot - 4 : mode;
    }

    /** Time the pitch drop takes to settle, tied to the decay: 20 ms .. 300 ms. */
    static double dropSeconds(double decaySeconds) noexcept
    {
        return std::clamp(0.15 * decaySeconds, 0.020, 0.300);
    }

    // Read-outs for tests.
    double currentRootHz() const noexcept { return std::exp2(pitch.value); }
    double currentDropSemitones() const noexcept { return dropNow; }
    int hitCount() const noexcept { return hits; }
    double gateLevel() const noexcept { return lastGate; }

private:
    struct Mode
    {
        double c = 0.0, s = 0.0;          // r cos(theta), r sin(theta), running
        double dc = 0.0, ds = 0.0;        // per-sample increments
        double tc = 0.0, ts = 0.0;        // values at the end of the control period
        double re[2] = { 0.0, 0.0 };
        double im[2] = { 0.0, 0.0 };
        LinearRamp weight;
        bool active = false;
    };

    void controlTick() noexcept;

    Mode modes[kMaxModes];
    TransientGate gate;
    Settings settings;
    LinearRamp pitch; // log2(Hz)
    double fs = 48000.0;
    double inputGain = 0.0;
    double dropNow = 0.0;
    double lastGate = 0.0;
    int controlInterval = 16;
    int weightRampSamples = 960;
    int samplesToTick = 0;
    int hits = 0;
    bool pendingHit = false;
    bool snapPending = true;
    bool dirty = true;
    bool moving = true;
};
} // namespace rootnote
