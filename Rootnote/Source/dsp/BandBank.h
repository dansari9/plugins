#pragma once

#include "Series.h"
#include "Smoothers.h"
#include "SvfBell.h"
#include "Weighting.h"

namespace rootnote
{
/**
    A series cascade of up to 32 stereo bells at rootHz * series ratio.

    Boost and cut use the same class; only the per-band gain rule differs.
    Everything expensive (tan, exp, the weighting) runs at control rate, once
    per control interval and only while something is moving. Between two
    control points the four filter coefficients are ramped linearly, so the
    response moves without steps. A band at exactly 0 dB is not processed at
    all, which makes a neutral bank bit-transparent.

    Audio-thread safe after prepare(): no allocation, no locks.
*/
class BandBank
{
public:
    enum class Kind
    {
        boost,
        cut
    };

    struct Settings
    {
        SeriesType series = SeriesType::harmonic;
        QMode qMode = QMode::constantQ;
        int bands = 8;
        double gainDb = 0.0; // boost: 0..+24, cut: -30..0
        double q = 12.0;

        // Boost only.
        double blend = 0.5;
        bool focusLink = true;
        double focusHz = 65.0;
        double rangeOct = 1.5;
        double tiltDbPerOct = -3.0;

        // Cut only.
        double overtones = 0.5;

        bool operator==(const Settings& o) const noexcept
        {
            return series == o.series && qMode == o.qMode && bands == o.bands && gainDb == o.gainDb
                && q == o.q && blend == o.blend && focusLink == o.focusLink && focusHz == o.focusHz
                && rangeOct == o.rangeOct && tiltDbPerOct == o.tiltDbPerOct && overtones == o.overtones;
        }
        bool operator!=(const Settings& o) const noexcept { return !(*this == o); }

        /** The boost weighting these settings describe. */
        BoostShape shape() const noexcept
        {
            return { series, bands, blend, focusLink, focusHz, rangeOct, tiltDbPerOct };
        }
    };

    void prepare(double sampleRate, Kind bankKind) noexcept;

    /** Clears the filter state. The next settings and frequency are taken without a ramp. */
    void reset() noexcept;

    void setSettings(const Settings& newSettings) noexcept;

    /** Starts a ramp, linear in pitch, from the current root to hz. */
    void setTargetFrequency(double hz, double rampSeconds) noexcept;

    /** In place. right may be null for mono. */
    void process(double* left, double* right, int numSamples) noexcept;

    /** True when no band is switched on, so process() leaves the signal untouched. */
    bool isNeutral() const noexcept { return numActive == 0 && !moving; }

    // State read-outs for tests, the ring engine and the editor. Audio thread only.
    double currentRootHz() const noexcept;
    double targetRootHz() const noexcept;
    bool isRootMoving() const noexcept { return pitch.isActive(); }
    int controlIntervalSamples() const noexcept { return controlInterval; }
    int activeBandCount() const noexcept { return numActive; }
    double slotGainDb(int slot) const noexcept { return slots[slot].gain.value; }
    double slotRatio(int slot) const noexcept { return slots[slot].ratio; }
    bool slotActive(int slot) const noexcept { return slots[slot].active; }

private:
    struct Slot
    {
        BellCoeffs coeffs;        // running values
        BellCoeffs target;        // values at the end of the control period
        BellCoeffs delta;         // per-sample increments
        BellState state[2];
        LinearRamp gain;          // dB
        double ratio = 1.0;
        double g = 0.0;           // cached tan() prewarp
        double gHz = -1.0;        // frequency g was computed for
        bool active = false;
        bool ramping = false;
        bool retire = false;      // reached 0 dB; switch off at the next control point
    };

    void controlTick() noexcept;
    void markDirty() noexcept;
    double targetGainFor(int slot, double rootHz) const noexcept;
    static void runSlot(Slot& slot, double* left, double* right, int numSamples) noexcept;

    Slot slots[kMaxSlots];
    int activeSlots[kMaxSlots] {};
    int numActive = 0;

    Settings settings;
    BoostShape shape;
    Kind kind = Kind::boost;
    double fs = 48000.0;
    int controlInterval = 16;
    int gainRampSamples = 960;
    int qRampSamples = 1440;

    LinearRamp pitch; // log2(Hz)
    LinearRamp logQ;  // log2(Q)

    int samplesToTick = 0;
    bool moving = true;
    int elapsedInPeriod = 0;
    bool midPeriod = false;   // a setter cut the running control period short
    bool snapPending = true;
};
} // namespace rootnote
