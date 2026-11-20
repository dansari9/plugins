#pragma once
#include "MasterConstants.h"
#include "../common/Smoother.h"
#include <array>

namespace snap
{
class CutFilter
{
public:
    void prepare(double fs) { lo.prepare(fs), hi.prepare(fs); }
    void reset() { lo.reset(), hi.reset(); }
    void setParams(float loHz, float hiHz, int loShape, int hiShape)
    {
        lo.setParams(loHz, loShape), hi.setParams(hiHz, hiShape);
    }
    void step() { lo.step(), hi.step(); }
    float processSample(size_t ch, float x) { return hi.processSample(ch, lo.processSample(ch, x)); }

private:
    // TPT state-variable section. Both shapes at each edge share the same prewarped cutoff.
    struct Section
    {
        void tune(double g, double newQ)
        {
            k = 1 / newQ, a = 1 / (1 + g * (g + k)), b = g * a, c = g * b;
        }
        double process(size_t ch, double x, bool highPass)
        {
            auto& [s1, s2] = state[ch];
            const double v = x - s2, band = a * s1 + b * v, low = s2 + b * s1 + c * v;
            s1 = 2 * band - s1, s2 = 2 * low - s2;
            return highPass ? x - k * band - low : low;
        }

        std::array<std::array<double, 2>, 2> state {};
        double k = 0, a = 0, b = 0, c = 0;
    };

    class Edge
    {
    public:
        explicit Edge(bool highPass) : hp(highPass) {}

        void prepare(double fs)
        {
            maxHz = fs * masterConstants::maxCutoffFraction;
            piOverFs = std::numbers::pi / fs;
            logHz.prepare(fs), shape.prepare(fs);
            reset();
        }
        void reset()
        {
            logHz.reset(logHz.targetValue()), shape.reset(shape.targetValue());
            gentle.state = {}, steepFirst.state = {}, steepSecond.state = {}, pole = {};
            tunedHz = 0;
            tune(logHz.value());
            steepMix = shape.value();
        }
        void setParams(float hz, int choice)
        {
            using namespace masterConstants;
            hz = hp ? std::clamp(hz, cutLoOpenHz, cutLoMaxHz) : std::clamp(hz, cutHiMinHz, cutHiOpenHz);
            if (hz != targetHz)
            {
                targetHz = hz;
                logHz.setTarget(std::log2(hz));
            }
            shape.setTarget(choice == 1 ? 1.0f : 0.0f);
        }
        void step()
        {
            if (logHz.isSmoothing())
                tune(logHz.next());
            steepMix = shape.next();
        }
        float processSample(size_t ch, float x)
        {
            double soft;
            if (hp)
                soft = gentle.process(ch, x, true);
            else
            {
                const double v = (x - pole[ch]) * poleGain;
                soft = pole[ch] + v;
                pole[ch] = soft + v;
            }
            const double steep = steepSecond.process(ch, steepFirst.process(ch, x, hp), hp);
            if (presence == 0)
                return x;

            const double filtered = soft + steepMix * (steep - soft);
            return static_cast<float>(x + presence * (filtered - x));
        }

    private:
        void tune(float logCutoff)
        {
            using namespace masterConstants;
            const float inside = hp ? (logCutoff - openLogHz) / cutLoPresenceOctaves
                                    : (openLogHz - logCutoff) / cutHiPresenceOctaves;
            const float weight = std::clamp(inside, 0.0f, 1.0f);
            presence = hp ? weight * weight : weight;
            const double hz = std::min(maxHz, std::exp2(static_cast<double>(logCutoff)));
            if (hz == tunedHz)
                return;

            tunedHz = hz;
            const double g = std::tan(piOverFs * hz);
            if (hp)
                gentle.tune(g, butterworthQ);
            else
                poleGain = g / (1 + g);
            steepFirst.tune(g, hp ? cutLoSteepFirstQ : cutHiSteepFirstQ);
            steepSecond.tune(g, hp ? cutLoSteepSecondQ : cutHiSteepSecondQ);
        }

        const bool hp;
        const float openLogHz = std::log2(hp ? masterConstants::cutLoOpenHz : masterConstants::cutHiOpenHz);
        Section gentle, steepFirst, steepSecond;
        std::array<double, 2> pole {};
        Smoother logHz, shape;
        float targetHz = 0, steepMix = 0, presence = 0;
        double maxHz = 0, piOverFs = 0, tunedHz = 0, poleGain = 0;
    };

    Edge lo { true }, hi { false };
};
} // namespace snap
