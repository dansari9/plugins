#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

/**
    Real-time guitar-amp spring tank.

    The topology follows the dispersive-allpass spring models of Abel, Berners,
    Costello and Smith (AES 121, 2006) and the parametric spring of Välimäki,
    Parker and Abel (JAES 2010):

      - Three springs in parallel, with incommensurate one-way delays, like a
        short/medium/long amp tank.
      - Each spring has two feedback loops. The low loop carries the body of
        the tank; the high loop is shorter and carries the bright drip.
      - Dispersion is a cascade of identical one-sample-at-48 kHz allpasses
        (a spectral delay filter). The stretch scales with sample rate, so the
        chirp keeps the same length from 44.1 kHz to 192 kHz. Negative
        coefficients hold the lows back: highs arrive first and the note falls,
        which is the surf-tank drip. Tension sets how strong that coefficient
        is. The sections are exact allpasses, so they disperse without eating
        the decay.
      - A short cascade sits inside each feedback loop, so later repeats pick
        up a little more smear. The long drip cascade is on the high-loop
        pickup only. Putting that long chain inside the loop would stretch
        every repeat until the tail was too quiet to hear.
      - Tone is a gentle per-trip damping lowpass plus a mild transducer peak.
        Decay is the RT60, mapped to a negative reflection gain. Drip is the
        balance between the falling high loop and the body.

    Process() allocates nothing. Time constants are in seconds, so 44.1 kHz
    through 192 kHz keep the same tank.
*/
class SpringReverb
{
public:
    struct Params
    {
        float decaySeconds = 2.2f;
        float tension = 0.28f;     // 0 loose / chirpy, 1 tight / ping
        float tone = 0.58f;        // 0 dark, 1 bright
        float drip = 0.70f;        // emphasis of the falling transient
        float preDelayMs = 0.0f;
        float mix = 0.42f;         // 0 dry, 1 wet
        float outputDb = 0.0f;
    };

    SpringReverb();
    ~SpringReverb();

    SpringReverb(const SpringReverb&) = delete;
    SpringReverb& operator=(const SpringReverb&) = delete;

    void prepare(double sampleRate, int samplesPerBlock, int numChannels);
    void reset();
    /** Snap skips the smoothing ramp. Use it from prepare() and offline renders. */
    void setParameters(const Params& params, bool snap);
    void process(juce::AudioBuffer<float>& buffer);
    bool isPrepared() const noexcept;

    static Params defaultParams() { return {}; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Delay-line timing and chirp-direction checks against the real DSP types. */
bool springReverbDspSelfTest(juce::String& failureReason);
