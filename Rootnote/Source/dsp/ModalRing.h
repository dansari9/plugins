#pragma once

#include "Series.h"
#include "Smoothers.h"
#include "TransientGate.h"
#include "Weighting.h"

namespace rootnote
{
/**
    The Ring engine: a bank of modal resonators that only the start
    of each hit can excite, so the tail length is set in milliseconds and does
    not depend on the bell Q.

    Each mode is one complex pole z[n] = p z[n-1] + x[n], p = r e^(j theta),
    run as a 2x2 rotation. It has the same poles as the two-pole recursion
    y = 2 r cos(theta) y1 - r^2 y2 + x, but the state is a rotating vector whose
    length is scaled by exactly r each sample whatever theta does. That keeps
    the amplitude steady while the pitch glides (Drop, MIDI glide), where the
    direct recursion jumps.

    Mode m sits on slot m of the boost series and takes the boost weights
    (range, blend, tilt). Audio-thread safe after prepare().
*/
class ModalRing
{
public:
    /** Harmonic and drum series ring on their first 16 bands, the octave series on every octave the EQ has. */
    static constexpr int kSeriesModes = 16;
    static constexpr int kMaxModes = kOctaveSlots;
    static constexpr int modeCount(SeriesType series) noexcept
    {
        return series == SeriesType::octaves ? kOctaveSlots : kSeriesModes;
    }
    /** Higher modes decay faster: T60_k = T60 / ratio^kDamping. */
    static constexpr double kDamping = 0.7;

    struct Settings
    {
        BoostShape shape;
        double decaySeconds = 0.6; // T60 of the root
        double dropSemitones = 0.0;

        bool operator==(const Settings& o) const noexcept
        {
            return shape == o.shape && decaySeconds == o.decaySeconds && dropSemitones == o.dropSemitones;
        }
    };

    void prepare(double sampleRate) noexcept;

    /** Silences everything and forgets the input seen so far. */
    void reset() noexcept;

    /** Silences the resonators but keeps listening for hits. */
    void silence() noexcept;
    void setSettings(const Settings& newSettings) noexcept;
    void setTargetFrequency(double hz, double rampSeconds) noexcept;

    /**
        Reads the dry input, writes the ring signal. inRight and outRight may
        be null for mono. The outputs must not alias the inputs.
    */
    void process(const float* inLeft, const float* inRight, double* outLeft, double* outRight, int numSamples) noexcept;

    /**
        Listens to the input without ringing, while Ring is at zero. The hit
        detector then knows what came before when Ring is turned up in the
        middle of a sound, and does not take that sound for a new hit.
    */
    void listen(const float* inLeft, const float* inRight, int numSamples) noexcept;

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
