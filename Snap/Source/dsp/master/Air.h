#pragma once
#include "MasterConstants.h"
#include "../common/EnvelopeFollower.h"
#include "../common/Smoother.h"
#include <juce_dsp/juce_dsp.h>

namespace snap
{
class Air
{
public:
    void prepare(double fs, int channels)
    {
        using namespace masterConstants;
        highPass.prepare({ fs, 1, static_cast<juce::uint32>(channels) });
        highPass.setType(juce::dsp::StateVariableTPTFilterType::highpass);
        highPass.setResonance(static_cast<float>(butterworthQ));
        highPass.setCutoffFrequency(std::min(airHz, static_cast<float>(fs * maxCutoffFraction)));
        for (auto* follower : { &fullLevel, &highLevel })
            follower->prepare(fs, airAttackMs, airReleaseMs);
        gap = juce::Decibels::decibelsToGain(-airGapDb);
        boostLimit = juce::Decibels::decibelsToGain(airBoostMaxDb);
        gateFloor = juce::Decibels::decibelsToGain(airGateDb);
        gateScale = 1 / (juce::Decibels::decibelsToGain(airGateOpenDb) - gateFloor);
        amount.prepare(fs);
        reset();
    }
    void reset()
    {
        highPass.reset(), fullLevel.reset(), highLevel.reset();
        amount.reset(amount.targetValue());
    }
    void setAmount(float a) { amount.setTarget(a); }

    void process(float& left, float& right, bool stereo)
    {
        const float hiLeft = highPass.processSample(0, left);
        const float hiRight = stereo ? highPass.processSample(1, right) : hiLeft;
        const float full = fullLevel.next(stereo ? std::max(std::abs(left), std::abs(right)) : std::abs(left));
        const float high = highLevel.next(std::max(std::abs(hiLeft), std::abs(hiRight)));
        const float a = amount.next();
        if (a == 0 || full <= gateFloor)
            return;

        using namespace masterConstants;
        const float threshold = gap * full;
        const float boost = high >= threshold ? 1.0f
                            : high == 0 ? boostLimit
                            : std::min(boostLimit, std::pow(threshold / high, airRatioSlope));
        const float gate = std::min(1.0f, (full - gateFloor) * gateScale);
        const float boostDelta = gate * (boost - 1);
        const float side = (hiLeft - hiRight) * (0.5f * (1 + boostDelta));
        const float sideDelta = side != 0 ? gate * (std::tanh(airSideDrive * side) / airSideDrive - side) : 0;

        // The low band is input minus high, so only the change to the high band needs adding.
        left += a * (hiLeft * boostDelta + sideDelta);
        if (stereo)
            right += a * (hiRight * boostDelta - sideDelta);
    }

private:
    juce::dsp::StateVariableTPTFilter<float> highPass;
    EnvelopeFollower fullLevel, highLevel;
    Smoother amount;
    float gap = 0, boostLimit = 1, gateFloor = 0, gateScale = 0;
};
} // namespace snap
