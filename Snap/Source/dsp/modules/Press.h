#pragma once
#include "ModuleBase.h"
#include "PressConstants.h"
#include "../common/EnvelopeFollower.h"
#include "../common/Smoother.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace snap
{
class Press final : public ModuleBase
{
public:
    struct Params
    {
        float amount = 0;
        int mode = 0;
        float downUp = -1, mix = 1, bands = 0;
    };

    Press() { setParams(Params {}); }

    void prepare(double fs, int channels, int, bool) override
    {
        using namespace pressConstants;
        const float maxCutoff = std::nextafter(static_cast<float>(fs * constants::crossoverMaxFraction), 0.0f);
        for (size_t k = 0; k < filters.size(); ++k)
        {
            auto& filter = filters[k];
            filter.prepare({ fs, 1, static_cast<juce::uint32>(channels) });
            filter.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
            filter.setCutoffFrequency(std::min(crossoverHz[k], maxCutoff));
        }
        for (auto& detector : detectors)
        {
            detector.peak.prepare(fs, peakAttackMs, peakReleaseMs);
            detector.reference.prepare(fs, referenceAttackMs, referenceReleaseMs);
        }
        for (auto* smoother : { &amount, &downUp, &mix, &bands, &modeRamp })
            smoother->prepare(fs);
        for (size_t k = 0; k < modes.size(); ++k)
            preparedModes[k] = coefficients(fs, modes[k]);
        dcPole = 1.0f - EnvelopeFollower::coef(fs, 1000.0 / (juce::MathConstants<double>::twoPi * evenDcBlockHz));
        reset();
    }

    void reset() override
    {
        for (auto& filter : filters)
            filter.reset();
        for (auto& detector : detectors)
        {
            detector.peak.reset(), detector.reference.reset();
            detector.fast = detector.slow = detector.reduction = 0;
        }
        for (auto* smoother : { &amount, &downUp, &mix, &bands })
            smoother->reset(smoother->targetValue());
        evenDc = {};
        modeFrom = modeTo = preparedModes[mode];
        modeRamp.reset(1);
        downMeter.store(0, std::memory_order_relaxed), upMeter.store(0, std::memory_order_relaxed);
    }

    void setParams(const Params& p)
    {
        amount.setTarget(p.amount), downUp.setTarget(p.downUp), mix.setTarget(p.mix), bands.setTarget(p.bands);
        const auto nextMode = static_cast<size_t>(std::clamp(p.mode, 0, static_cast<int>(preparedModes.size()) - 1));
        if (nextMode != mode)
        {
            modeFrom = currentMode(modeRamp.value());
            mode = nextMode;
            modeTo = preparedModes[mode];
            modeRamp.reset(0), modeRamp.setTarget(1);
        }
    }

    int latency() const override { return 0; }

    // Takes over another instance's level detectors (the slow level reference takes seconds to settle from zero), so a
    // freshly reset copy that starts in another chain position compresses like the one it replaces.
    void seedDetectors(const Press& other) { detectors = other.detectors; }
    float downwardDb() const { return downMeter.load(std::memory_order_relaxed); }
    float upwardDb() const { return upMeter.load(std::memory_order_relaxed); }

    void process(juce::AudioBuffer<float>& io, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const int channels = io.getNumChannels();
        auto* const* audio = io.getArrayOfWritePointers();
        float deepest = 0, highest = 0;
        for (int i = 0; i < n; ++i)
        {
            std::array<float, bandCount + 1> peaks {};
            std::array<std::array<float, bandCount>, 2> split {};
            for (int c = 0; c < channels; ++c)
            {
                float residual = audio[c][i];
                auto& parts = split[static_cast<size_t>(c)];
                peaks[0] = std::max(peaks[0], std::abs(residual));
                for (size_t k = 0; k < filters.size(); ++k)
                {
                    parts[k] = filters[k].processSample(c, residual);
                    residual -= parts[k];
                }
                parts.back() = residual;
                for (size_t k = 0; k < bandCount; ++k)
                    peaks[k + 1] = std::max(peaks[k + 1], std::abs(parts[k]));
            }

            const auto m = currentMode(modeRamp.next());
            const float a = amount.next(), direction = downUp.next(), blend = mix.next(), separation = bands.next();
            const bool multiband = a > 0 && separation > 0;
            auto& fullDetector = detectors[0];
            fullDetector.step(peaks[0], m);
            const auto full = a > 0 ? fullDetector.level(m) : Level {};
            const Curve curve(m, a, direction);
            const auto broad = curve.gain(full, 0);
            fullDetector.reduction = broad.down;
            std::array<float, bandCount> delta {};
            for (size_t k = 0; k < bandCount; ++k)
            {
                auto& detector = detectors[k + 1];
                detector.step(peaks[k + 1], m);
                auto gain = broad;
                if (multiband)
                {
                    const auto band = detector.level(m);
                    const Level control { std::lerp(full.down, band.down, separation), std::lerp(full.up, band.up, separation),
                                          std::lerp(full.absolute, band.absolute, separation) };
                    gain = curve.gain(control, separation * m[bandMakeup] * pressConstants::stackMakeupDb[k]);
                }
                detector.reduction = gain.down;
                delta[k] = gain.linear - broad.linear;
                deepest = std::min(deepest, gain.down), highest = std::max(highest, gain.up);
            }

            if (a <= 0 || blend <= 0)
                continue;
            const float pre = a * m[preDrive], even = a * a * m[evenDrive];
            for (int c = 0; c < channels; ++c)
            {
                const float dry = audio[c][i];
                // Gain-weighted bands, with their common part reconstructed directly from the dry input.
                float wet = dry * broad.linear;
                if (multiband)
                    for (size_t k = 0; k < bandCount; ++k)
                        wet += delta[k] * split[static_cast<size_t>(c)][k];
                // A common pre-saturation gain commutes with the band gain sum.
                if (pre > 0)
                    wet /= 1.0f + pre * std::abs(dry);
                if (even > 0)
                {
                    // The even term is even x r; r's DC is blocked, and the result still fades to exactly 0 with the drive.
                    const float driven = even * wet, denominator = 1.0f + driven * driven;
                    const float r = pressConstants::evenAsymmetry * driven * wet / denominator;
                    float& dc = evenDc[static_cast<size_t>(c)];
                    dc += dcPole * (r - dc);
                    wet = wet / std::sqrt(denominator) + even * (r - dc);
                }
                audio[c][i] = std::lerp(dry, wet, blend);
            }
        }
        downMeter.store(deepest, std::memory_order_relaxed), upMeter.store(highest, std::memory_order_relaxed);
    }

private:
    enum Coefficient
    {
        attack, release, slowAttack, slowRelease, slowShare, ratioStart, ratioEnd, upRatio, knee,
        downStart, downEnd, upStart, upEnd, makeup, feedback, preDrive, evenDrive, bandMakeup, coefficientCount
    };
    using Controls = std::array<float, coefficientCount>;
    static constexpr size_t bandCount = pressConstants::crossoverHz.size() + 1;

    static Controls coefficients(double fs, const pressConstants::Mode& m)
    {
        return { EnvelopeFollower::coef(fs, m.attackMs), EnvelopeFollower::coef(fs, m.releaseMs),
                 EnvelopeFollower::coef(fs, m.slowAttackMs), EnvelopeFollower::coef(fs, m.slowReleaseMs),
                 m.slowShare, m.ratioStart, m.ratioEnd, m.upRatio, m.kneeDb, m.downStartDb, m.downEndDb,
                 m.upStartDb, m.upEndDb, m.makeupDb, m.feedback, m.preDrive, m.evenDrive, m.bandMakeup };
    }

    Controls currentMode(float t) const
    {
        if (t >= 1)
            return modeTo;
        Controls m;
        for (size_t k = 0; k < m.size(); ++k)
            m[k] = std::lerp(modeFrom[k], modeTo[k], t);
        return m;
    }

    static float db(float level) { return 20.0f * std::log10(std::max(pressConstants::minLevel, level)); }
    static float linear(float gainDb) { return std::pow(10.0f, gainDb * 0.05f); }

    struct Level { float down, up, absolute; };

    struct Detector
    {
        void step(float input, const Controls& m)
        {
            const float peakLevel = peak.next(input);
            reference.next(peakLevel);
            const float key = m[feedback] <= 0 ? peakLevel : peakLevel * linear(m[feedback] * reduction);
            fast = follow(fast, key, m[attack], m[release]);
            slow = follow(slow, key, m[slowAttack], m[slowRelease]);
        }

        Level level(const Controls& m) const
        {
            const float levelDb = db(peak.value()), referenceDb = db(reference.value());
            const float releaseLevel = fast + m[slowShare] * std::max(0.0f, slow - fast);
            return { db(releaseLevel) - referenceDb, levelDb - referenceDb, levelDb };
        }

        static float follow(float state, float input, float a, float r)
        {
            const float v = input + (input > state ? a : r) * (state - input);
            return v < constants::denormalFloor ? 0.0f : v;
        }

        EnvelopeFollower peak, reference;
        float fast = 0, slow = 0, reduction = 0;
    };

    struct Gain { float linear = 1, down = 0, up = 0; };

    struct Curve
    {
        Curve(const Controls& m, float a, float direction)
            : downThreshold(std::lerp(m[downStart], m[downEnd], a)), upThreshold(std::lerp(m[upStart], m[upEnd], a)),
              kneeDb(m[knee]), downAmount(a * std::min(1.0f, 1.0f - direction)),
              upAmount(a * std::min(1.0f, 1.0f + direction)), baseMakeup(m[makeup]),
              downSlope(1.0f - 1.0f / std::lerp(m[ratioStart], m[ratioEnd], a)), upSlope(1.0f - 1.0f / m[upRatio])
        {
            // Compensate the feedback detector for the portion of the gain reduction it already sees.
            downSlope /= 1.0f - m[feedback] * downAmount * downSlope;
        }

        Gain gain(const Level& level, float bandTrim) const
        {
            using namespace pressConstants;
            if (downAmount <= 0 && upAmount <= 0)
                return {};
            const float down = -downAmount * std::min(maxDownDb, downSlope * softKnee(level.down - downThreshold));
            const float gate = std::clamp((level.absolute - gateLowDb) / (gateHighDb - gateLowDb), 0.0f, 1.0f);
            const float up = upAmount * gate * std::min(maxUpDb, upSlope * softKnee(upThreshold - level.up));
            return { linear(down + up + downAmount * (baseMakeup + bandTrim)), down, up };
        }

        float softKnee(float over) const
        {
            const float half = kneeDb * 0.5f;
            const float inside = std::clamp(over + half, 0.0f, kneeDb);
            return std::max(0.0f, over - half) + inside * inside / (2.0f * kneeDb);
        }

        float downThreshold, upThreshold, kneeDb, downAmount, upAmount, baseMakeup, downSlope, upSlope;
    };

    std::array<juce::dsp::LinkwitzRileyFilter<float>, bandCount - 1> filters;
    std::array<Detector, bandCount + 1> detectors;
    Smoother amount, downUp, mix, bands, modeRamp;
    std::array<Controls, pressConstants::modes.size()> preparedModes {};
    Controls modeFrom {}, modeTo {};
    size_t mode = 0;
    std::array<float, 2> evenDc {};
    float dcPole = 0;
    static_assert(std::atomic<float>::is_always_lock_free);
    std::atomic<float> downMeter { 0 }, upMeter { 0 };
};
} // namespace snap
