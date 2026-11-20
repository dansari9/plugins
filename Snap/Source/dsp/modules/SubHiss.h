#pragma once

#include "../common/Delay.h"
#include "../common/Ducker.h"
#include "../common/Smoother.h"
#include "SubHissConstants.h"
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace snap
{
class SubHiss
{
public:
    struct LayerSettings
    {
        float amount = 0, sensitivity = 50;
        float detectLoHz, detectHiHz, decayMs;
        float duck = 0;
        bool solo = false, post = false;
    };

    struct SubSettings : LayerSettings
    {
        SubSettings() : LayerSettings { 0, 50, 30, 150, 300 } {}
        float pitch = 31, stereo = 0, color = 20;
        bool acid = false;
    };

    struct HissSettings : LayerSettings
    {
        HissSettings() : LayerSettings { 0, 50, 1500, 8000, 180 } {}
        float pitch = 0, tone = 0, env = 30, res = 10;
        int type = 0;
    };

    struct Settings
    {
        bool enabled = true;
        SubSettings sub;
        HissSettings hiss;
    };

    // Prepare and load the embedded noise on a non-audio thread.
    void prepare(const juce::dsp::ProcessSpec&, const Settings&, bool lowLatency = false);
    void reset() noexcept;
    int getLatencySamples() const noexcept { return dryDelay.getDelay(); }
    static int latencyFor(double fs, bool lowLatency)
    {
        return lowLatency ? constants::lowLatencyMaskLookahead : (int) std::lround(fs * constants::maskLookaheadMs * 0.001);
    }
    void setLowLatency(bool lowLatency) noexcept; // audio thread: prepare() sized the delays for both modes

    // io becomes delayed dry plus Pre layers. post is overwritten with Post layers in
    // the same timeline. Both buffers must have identical dimensions and separate storage.
    void process(juce::AudioBuffer<float>& io, juce::AudioBuffer<float>& post, const Settings&) noexcept;

private:
    struct Detector
    {
        void prepare(const juce::dsp::ProcessSpec&, int lookaheadSamples);
        void reset() noexcept;
        void setRange(float loHz, float hiHz) noexcept;
        float next(const std::array<float, 2>& input, int channels, float threshold) noexcept;

        std::array<juce::dsp::StateVariableTPTFilter<float>, 2> highpass, lowpass;
        EnvelopeFollower envelope;
        float lowHz = 0, highHz = 0, highestHz = 0, peak = 0, pendingThreshold = 1, hysteresis = 0;
        int retrigger = 1, lookahead = 0, sinceHit = 0, sinceQuiet = 0, pending = -1;
        bool armed = true;
    };

    struct Layer
    {
        void prepare(const juce::dsp::ProcessSpec&, int lookaheadSamples);
        void reset() noexcept;
        void set(const LayerSettings&, bool listen, bool audible, float defaultThresholdDb, bool immediate) noexcept;
        float detect(const std::array<float, 2>& input, int channels, bool updateFilters) noexcept;
        float gain(float key) noexcept;

        Detector detector;
        Ducker ducker;
        Smoother amount, decayRate, duck, post, lowHz, highHz, threshold;
        float decayScale = 0;
        bool listening = false;
    };

    struct Voice
    {
        double phase = 0;
        float level = 0, attack = 0, sweep = 0;
        int type = 0;
    };

    struct Voices
    {
        void prepare(double fs, double attackMs) noexcept;
        void trigger(float velocity, int type = 0) noexcept;
        void reset() noexcept;
        float advance(Voice&, float decay) const noexcept;

        Voice current, previous;
        float attackStep = 1, fadeStep = 1, previousGain = 0;
    };

    void set(const Settings&, bool immediate) noexcept;
    void loadNoise();
    std::array<float, 3> osc(Voice&, double pitchHz, float decay, float color, float weight) noexcept;
    std::array<float, 2> renderSub(float velocity) noexcept;
    float playNoise(Voice&, double increment, float decay) const noexcept;
    float renderHiss(float velocity, bool updateFilters) noexcept;

    Layer sub, hiss;
    Voices subVoices, hissVoices;
    Delay dryDelay, stereoDelay;
    juce::dsp::LinkwitzRileyFilter<float> stereoHighpass;
    juce::dsp::StateVariableTPTFilter<float> acidLowpass, hissLowpass;
    std::array<std::vector<float>, subHissConstants::noiseNames.size()> noise;
    Smoother dryGain, subPitch, subColor, subStereo, subAcid, hissPitch, hissTone, hissEnv, hissRes;
    double sampleRate = 48000;
    float pitchFall = 0, filterFall = 0, dcAlpha = 0, toneAlpha = 0, highestHz = 0;
    float subDc = 0, hissTiltLow = 0;
    int hissType = 0, filterInterval = 1, untilFilterUpdate = 0;
};
} // namespace snap
