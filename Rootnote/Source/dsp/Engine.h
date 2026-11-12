#pragma once

#include "BandBank.h"
#include "Limiter.h"
#include "ModalRing.h"
#include "NoteControl.h"
#include "Smoothers.h"

#include <vector>

namespace rootnote
{
/** Plain values of every parameter, in the units of the parameter table. */
struct EngineParams
{
    double boostNote = 36.0;
    double boostFineCents = 0.0;
    double boostGainDb = 9.0;
    double boostQ = 12.0;
    SeriesType series = SeriesType::harmonic;
    int bands = 8;
    double blend = 0.5;
    bool focusLink = true;
    double focusHz = 65.0;
    double rangeOct = 1.5;
    double tiltDbPerOct = -3.0;
    QMode qMode = QMode::constantQ;

    bool cutOn = false;
    double cutNote = 36.0;
    double cutDepthDb = -12.0;
    double cutQ = 8.0;
    double cutOvertones = 0.5; // 0..1
    int cutBands = 6;

    double ring = 0.0;         // 0..1
    double ringDecaySeconds = 0.6;
    double ringDropSemitones = 0.0;

    MidiMode midiMode = MidiMode::channels12;
    double glideSeconds = 0.0;
    NoteOffMode noteOff = NoteOffMode::hold;
    int key = 0;
    Scale scale = Scale::chromatic;
    bool keyLock = false;
    double a4 = 440.0;

    double mix = 1.0;          // 0..1
    double outputDb = 0.0;
    bool limiter = true;
    bool bypass = false;
};

/** One band of a bank as a display draws it. */
struct BandView
{
    double hz = 0.0;
    double gainDb = 0.0;
};

/**
    The whole signal path, without any JUCE dependency:

        in -> cut bank -> boost bank (+ ring) -> mix -> output gain -> limiter -> out

    with a 20 ms dry/processed crossfade for bypass. Zero latency. After
    prepare() nothing here allocates, locks or blocks.

    Call order on the audio thread, once per host block:
        setParams();  then process() for the samples before each MIDI event,
        handleMidi() for the event, and process() for the rest.
*/
class Engine
{
public:
    /** Pitch ramp for knob and automation moves. */
    static constexpr double kParamPitchRampSeconds = 0.030;
    /** Shortest pitch ramp for a MIDI note change, used when Glide is 0. */
    static constexpr double kMinMidiRampSeconds = 0.005;
    /** Ramp for mix, output, ring amount, limiter switch and bypass. */
    static constexpr double kLevelRampSeconds = 0.020;
    /** Level of the ring signal at Ring = 100 %. */
    static constexpr double kRingMakeup = 2.0;

    void prepare(double sampleRate, int maxBlockSize);
    bool isPrepared() const noexcept { return prepared; }

    /** Clears all audio state. Held MIDI notes are kept. */
    void reset() noexcept;

    void setParams(const EngineParams& newParams) noexcept;
    void handleMidi(const MidiEvent& event) noexcept;

    /** Forgets every MIDI note, so both banks play their knob notes again. */
    void releaseMidi() noexcept { control.reset(); }

    /** In place, one or two channels. */
    void process(float* const* channels, int numChannels, int numSamples) noexcept;

    // Read-outs, audio thread only.
    double boostHz() const noexcept { return boost.currentRootHz(); }
    double cutHz() const noexcept { return cut.currentRootHz(); }
    double boostTargetNote() const noexcept { return control.note(NoteControl::boost); }
    double cutTargetNote() const noexcept { return control.note(NoteControl::cut); }
    bool boostFollowsMidi() const noexcept { return control.midiOwns(NoteControl::boost); }
    bool cutFollowsMidi() const noexcept { return control.midiOwns(NoteControl::cut); }
    unsigned midiNoteOnCount() const noexcept { return control.noteOnCount(); }
    double limiterGain() const noexcept { return limiterGainNow; }
    int ringHitCount() const noexcept { return ring.hitCount(); }
    const BandBank& boostBank() const noexcept { return boost; }
    const BandBank& cutBank() const noexcept { return cut; }

    /**
        Upper bound for the time the processor keeps sounding after the input
        stops: the slowest bell resonance or the ring decay. With MIDI on it
        covers every note MIDI could play. Any thread.
    */
    static double tailSeconds(const EngineParams& params, double sampleRate) noexcept;

    /**
        The bands of a bank at the given root, for a display: every band with a
        gain, lowest slot first. out holds kMaxSlots entries; returns the
        count. Pure functions of their arguments; any thread.
    */
    static int boostBands(const EngineParams& params, double rootHz, double sampleRate, BandView* out) noexcept;
    static int cutBands(const EngineParams& params, double rootHz, double sampleRate, BandView* out) noexcept;

private:
    void applyPitch(unsigned targets, double rampSeconds) noexcept;
    void processChunk(float* left, float* right, int numSamples) noexcept;
    void clearProcessingState() noexcept;

    EngineParams params;
    NoteControl control;
    BandBank boost;
    BandBank cut;
    ModalRing ring;
    Limiter limiter;

    LinearRamp mixRamp;
    LinearRamp outputRamp;  // linear gain
    LinearRamp ringRamp;
    LinearRamp limiterRamp; // 0 = off, 1 = on
    LinearRamp wetRamp;     // 0 = bypassed, 1 = processing

    std::vector<double> work[2];
    std::vector<double> ringOut[2];

    double fs = 48000.0;
    double limiterGainNow = 1.0;
    int maxBlock = 0;
    int levelRampSamples = 960;
    bool prepared = false;
    bool snapLevels = true;
    bool statesCleared = true;
    bool ringCleared = true;
};
} // namespace rootnote
