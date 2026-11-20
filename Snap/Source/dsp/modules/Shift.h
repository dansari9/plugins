#pragma once
#include "ModuleBase.h"
#include "ShiftConstants.h"
#include "../common/Smoother.h"
#include "../common/TransientMask.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <complex>
#include <vector>

namespace snap
{
class Shift final : public ModuleBase
{
public:
    struct Params
    {
        float amount = 0, balance = 0;
        bool invert = false;
        float stereo = 0, stretch = 0, formantMix = 1, target = 0;
        float focusLo = constants::focusMinHz, focusHi = constants::focusMaxHz;
    };

    Shift()
    {
        formantMix.reset(1), direction.reset(1);
        focusLo.reset(std::log2(constants::focusMinHz)), focusHi.reset(std::log2(constants::focusMaxHz));
    }

    void setParams(const Params& p)
    {
        amount.setTarget(p.amount), balance.setTarget(p.balance), direction.setTarget(p.invert ? -1.0f : 1.0f);
        stereo.setTarget(p.stereo), stretch.setTarget(p.stretch), formantMix.setTarget(p.formantMix);
        target.setTarget(p.target);
        const float lo = std::min(p.focusLo, p.focusHi);
        focusLo.setTarget(std::log2(lo)), focusHi.setTarget(std::log2(p.focusHi));
        focusLoMix.setTarget(lo > constants::focusMinHz ? 1.0f : 0.0f);
        focusHiMix.setTarget(p.focusHi < constants::focusMaxHz ? 1.0f : 0.0f);
    }

    static int latencyFor(double fs, bool lowLatency) { return lowLatency ? 0 : 1 << fftOrder(fs); }
    int latency() const override { return disabled ? 0 : size; }
    void setLowLatency(bool low) override
    {
        if (low != disabled)
            disabled = low, reset();
    }

    void prepare(double fs, int channels, int maxBlock, bool lowLatency) override
    {
        using namespace shiftConstants;
        size = latencyFor(fs, false), disabled = lowLatency;

        channelCount = static_cast<size_t>(channels), hop = size / overlap, half = size / 2;
        const auto lookahead = static_cast<int>(std::lround(fs * constants::maskLookaheadMs * 0.001));
        detector.prepare(fs, channels, lookahead), detector.setLookahead(lookahead);
        maskDelay.prepare(1, size - lookahead), maskDelay.setDelay(size - lookahead);
        dryDelay.prepare(channels, size), dryDelay.setDelay(size);
        scratch.setSize(channels, maxBlock), mask.resize(static_cast<size_t>(maxBlock));
        for (auto* smoother : { &amount, &balance, &direction, &stereo, &stretch, &formantMix, &target,
                                &focusLo, &focusHi, &focusLoMix, &focusHiMix })
            smoother->prepare(fs);
        binsPerHz = static_cast<float>(size / fs), attackRecovery = EnvelopeFollower::coef(fs, attackRecoveryMs);

        const auto length = static_cast<size_t>(size), bins = static_cast<size_t>(half + 1);
        window.resize(length), synthesis.resize(length), spectrum.resize(bins), bin.resize(bins);
        fftIn.resize(length), fftOut.resize(length);
        prepareFFT(fftOrder(fs));
        for (size_t i = 0; i < length; ++i)
            window[i] = static_cast<float>(0.5 - 0.5 * std::cos(twoPi * static_cast<double>(i) / size));
        for (int i = 0; i < size; ++i)
        {
            float sum = 0;
            for (int h = 0; h < overlap; ++h)
            {
                const float w = window[static_cast<size_t>((i + h * hop) & (size - 1))];
                sum += w * w;
            }
            synthesis[static_cast<size_t>(i)] = window[static_cast<size_t>(i)] / sum;
        }

        const int order = static_cast<int>(std::lround(lifterSeconds * fs));
        lifter.resize(static_cast<size_t>(order + 1));
        for (int q = 0; q <= order; ++q)
            lifter[static_cast<size_t>(q)] = static_cast<float>(0.5 + 0.5 * std::cos(0.5 * twoPi * q / order));
        for (int k = 1; k <= half; ++k)
            bin[static_cast<size_t>(k)].logHz = static_cast<float>(std::log2(k * fs / size));
        for (size_t c = 0; c < channelCount; ++c)
        {
            auto& s = state[c];
            s.input.resize(length), s.output.resize(length), s.phase.resize(bins);
            s.peak.prepare(fs, 0, peakReleaseMs);
        }
        reset();
    }

    void reset() override
    {
        if (size == 0)
            return;
        detector.reset(), maskDelay.reset(), dryDelay.reset();
        for (auto* smoother : { &amount, &balance, &direction, &stereo, &stretch, &formantMix, &target,
                                &focusLo, &focusHi, &focusLoMix, &focusHiMix })
            smoother->reset(smoother->targetValue());
        cursor = 0, untilFrame = hop;
        for (size_t c = 0; c < channelCount; ++c)
        {
            auto& s = state[c];
            std::fill(s.input.begin(), s.input.end(), 0.0f);
            std::fill(s.output.begin(), s.output.end(), 0.0f);
            std::fill(s.phase.begin(), s.phase.end(), Phase {});
            s.peak.reset(), s.hopPower = 0, s.previousPower = 0, s.attackDry = 0, s.resetFrames = 0;
        }
    }

    void process(juce::AudioBuffer<float>& io, int n) override
    {
        if (disabled)
            return;
        const juce::ScopedNoDenormals noDenormals;
        for (size_t c = 0; c < channelCount; ++c)
            scratch.copyFrom(static_cast<int>(c), 0, io, static_cast<int>(c), 0, n);
        detector.process(scratch, n, mask.data());
        for (int i = 0; i < n; ++i)
        {
            using namespace shiftConstants;
            const float a = amount.next(), b = balance.next(), pan = stereo.next(), sign = direction.next();
            const float semis = maxSemitones * a * std::pow(std::abs(a), amountExponent - 1.0f);
            // Balance and stereo scale the tapered semitones.
            const float pitch = semis * sign * (1.0f - std::max(0.0f, b)), form = semis * (1.0f + std::min(0.0f, b));
            const float pivot = stretch.next(), mix = formantMix.next();
            const float weight = TransientMask::weight(maskDelay.next(0, mask[static_cast<size_t>(i)]), target.next());
            focusLo.next(), focusHi.next(), focusLoMix.next(), focusHiMix.next();
            const auto index = static_cast<size_t>(cursor);
            const bool frameReady = --untilFrame == 0;
            cursor = (cursor + 1) & (size - 1);
            for (size_t c = 0; c < channelCount; ++c)
            {
                const auto ch = static_cast<int>(c);
                auto& s = state[c];
                const float freq = pitch * (channelCount == 1 ? 1.0f : 1.0f - std::max(0.0f, c == 0 ? pan : -pan));
                const float x = io.getSample(ch, i), dry = dryDelay.next(ch, x), level = std::abs(dry);
                // Keep abrupt attacks in place and bound ringing by the causal dry peak.
                s.attackDry = level > attackPeakRatio * s.peak.value() + attackPeakFloor ? 1.0f : s.attackDry * attackRecovery;
                const float wet = s.output[index], ceiling = peakHeadroom * s.peak.next(level);
                s.input[index] = x, s.output[index] = 0, s.hopPower += x * x;
                const float fade = std::min(1.0f, std::max(std::abs(freq), std::abs(form)) / engagementSemitones);
                const float engagement = fade * fade * (3.0f - 2.0f * fade);
                const float delta = std::clamp(wet, -ceiling, ceiling) - dry;
                io.setSample(ch, i, engagement > 0 && weight > 0 ? dry + (weight * engagement * (1.0f - s.attackDry)) * delta : dry);
                if (frameReady)
                    renderFrame(s, freq, form, pivot, mix);
            }
            if (frameReady)
                untilFrame = hop;
        }
    }

private:
    using Complex = std::complex<float>;
    static constexpr double twoPi = juce::MathConstants<double>::twoPi;
    struct Bin { Complex outside; float magnitude = 0, angle = 0, logHz = 0, envelope = 0, warped = 0; };
    struct Phase { float previous = 0, offset = 0; };
    struct Channel
    {
        std::vector<float> input, output;
        std::vector<Phase> phase;
        EnvelopeFollower peak;
        float hopPower = 0, previousPower = 0, attackDry = 0;
        int resetFrames = 0;
    };

    static int fftOrder(double fs)
    {
        return shiftConstants::referenceOrder + static_cast<int>(std::lround(std::log2(fs / shiftConstants::referenceRate)));
    }

    static float fourthPower(float x) { const float squared = x * x; return squared * squared; }

    // A plain radix-2 FFT on private buffers: JUCE's portable backend takes a spinlock, and one code path on every platform
    // keeps the tested behaviour. The twiddles come from a library transform in prepare().
    void prepareFFT(int order)
    {
        const juce::dsp::FFT setup(order);
        std::fill(fftIn.begin(), fftIn.end(), Complex {});
        fftIn[1] = 1.0f;
        setup.perform(fftIn.data(), fftOut.data(), false);
        twiddle.assign(fftOut.begin(), fftOut.begin() + half);
        reverse.resize(static_cast<size_t>(size));
        reverse[0] = 0;
        for (size_t i = 1; i < reverse.size(); ++i)
            reverse[i] = (reverse[i / 2] / 2) + (i % 2) * static_cast<size_t>(half);
    }

    void transform(bool inverse)
    {
        const auto length = fftIn.size();
        for (size_t i = 0; i < length; ++i)
            fftOut[i] = inverse ? std::conj(fftIn[reverse[i]]) : fftIn[reverse[i]];
        for (size_t span = 1; span < length; span *= 2)
        {
            const auto stride = length / (2 * span);
            for (size_t start = 0; start < length; start += 2 * span)
                for (size_t j = 0; j < span; ++j)
                {
                    const auto even = fftOut[start + j], odd = twiddle[j * stride] * fftOut[start + j + span];
                    fftOut[start + j] = even + odd, fftOut[start + j + span] = even - odd;
                }
        }
        if (inverse)
        {
            const float scale = 1.0f / static_cast<float>(size);
            for (auto& value : fftOut)
                value = std::conj(value) * scale;
        }
    }
    std::vector<size_t> reverse;
    std::vector<Complex> twiddle;

    void renderFrame(Channel& s, float pitch, float form, float stretchValue, float mix)
    {
        using namespace shiftConstants;
        if (s.hopPower > s.previousPower * onsetPowerRatio + onsetPowerFloor * static_cast<float>(hop))
            s.resetFrames = onsetResetFrames;
        s.previousPower = s.hopPower, s.hopPower = 0;
        const bool onset = s.resetFrames > 0;
        s.resetFrames -= static_cast<int>(onset);

        for (int i = 0; i < size; ++i)
            fftIn[static_cast<size_t>(i)] = s.input[static_cast<size_t>((cursor + i) & (size - 1))] * window[static_cast<size_t>(i)];
        transform(false);
        const float lowMix = focusLoMix.value(), highMix = focusHiMix.value();
        const float lo = fourthPower(std::exp2(focusLo.value()) * binsPerHz);
        const float hi = fourthPower(std::exp2(focusHi.value()) * binsPerHz);
        for (size_t k = 0; k < bin.size(); ++k)
        {
            // Refer phases to the window centre so neighbouring bins interpolate coherently.
            const auto x = k % 2 == 0 ? fftOut[k] : -fftOut[k];
            // A complementary zero-phase split keeps the outside band aligned with the full dry delay.
            const float f = fourthPower(static_cast<float>(k));
            const float band = std::lerp(1.0f, f / (f + lo), lowMix) * std::lerp(1.0f, hi / (f + hi), highMix);
            spectrum[k] = x * band, bin[k].outside = x - spectrum[k];
            bin[k].magnitude = std::abs(spectrum[k]), bin[k].angle = std::arg(spectrum[k]);
        }

        const bool warp = std::abs(form) > 0 && mix > 0;
        if (std::abs(pitch) > 0 || warp)
            spectralEnvelope(form, stretchValue, mix);

        if (std::abs(pitch) > 0)
            remap(s, std::exp2(pitch / semitonesPerOctave), onset);
        else
        {
            for (size_t k = 0; k < bin.size(); ++k)
            {
                s.phase[k].offset = 0;
                fftIn[k] = warp ? spectrum[k] * envelopeGain(k, k) : spectrum[k];
            }
        }

        for (size_t k = 0; k < bin.size(); ++k)
        {
            s.phase[k].previous = bin[k].angle;
            fftIn[k] += bin[k].outside;
            if (k % 2 != 0)
                fftIn[k] = -fftIn[k];
        }
        for (int k = 1; k < half; ++k)
            fftIn[static_cast<size_t>(size - k)] = std::conj(fftIn[static_cast<size_t>(k)]);
        fftIn[0].imag(0), fftIn[static_cast<size_t>(half)].imag(0);
        transform(true);
        // The oldest input in the frame was N samples ago. Its output starts at the next ring slot.
        for (int i = 0; i < size; ++i)
            s.output[static_cast<size_t>((cursor + i) & (size - 1))] += fftOut[static_cast<size_t>(i)].real() * synthesis[static_cast<size_t>(i)];
    }

    void spectralEnvelope(float form, float stretchValue, float mix)
    {
        using namespace shiftConstants;
        for (size_t k = 0; k < bin.size(); ++k)
            fftIn[k] = std::log(std::max(magnitudeFloor, bin[k].magnitude));
        for (int k = 1; k < half; ++k)
            fftIn[static_cast<size_t>(size - k)] = fftIn[static_cast<size_t>(k)];
        transform(true);
        std::fill(fftIn.begin(), fftIn.end(), Complex {});
        fftIn[0] = fftOut[0].real();
        for (size_t q = 1; q < lifter.size(); ++q)
            fftIn[q] = fftIn[fftIn.size() - q] = fftOut[q].real() * lifter[q];
        transform(false);
        for (size_t k = 0; k < bin.size(); ++k)
            bin[k].envelope = fftOut[k].real();

        const float pivot = std::lerp(std::log2(pivotCentreHz), std::log2(stretchValue < 0 ? pivotLowHz : pivotHighHz), std::abs(stretchValue));
        const float octaves = form / semitonesPerOctave;
        bin[0].warped = bin[0].envelope;
        for (size_t k = 1; k < bin.size(); ++k)
        {
            const float taper = std::clamp(1.0f + (bin[k].logHz - pivot) / pivotTaperOctaves, 0.0f, 1.0f);
            const float source = std::min(static_cast<float>(half), static_cast<float>(k) * std::exp2(-octaves * taper));
            const auto lo = static_cast<size_t>(source), hi = std::min(lo + 1, static_cast<size_t>(half));
            bin[k].warped = std::lerp(bin[k].envelope, std::lerp(bin[lo].envelope, bin[hi].envelope, source - static_cast<float>(lo)), mix);
        }
    }

    float envelopeGain(size_t source, size_t dest) const
    {
        constexpr float limit = shiftConstants::maxEnvelopeGainLog;
        return std::exp(std::clamp(bin[dest].warped - bin[source].envelope, -limit, limit));
    }

    void remap(Channel& s, float ratio, bool onset)
    {
        std::fill_n(fftIn.begin(), bin.size(), Complex {});
        size_t peak = bin[0].magnitude >= bin[1].magnitude ? 0 : 1;
        while (peak < static_cast<size_t>(half) && bin[peak].magnitude < bin[peak + 1].magnitude)
            ++peak;
        size_t first = 0;
        const auto lastBin = static_cast<size_t>(half);
        while (first <= lastBin)
        {
            size_t next = peak + 1;
            while (next <= lastBin && (bin[next].magnitude <= bin[next - 1].magnitude
                                       || (next < lastBin && bin[next].magnitude < bin[next + 1].magnitude)))
                ++next;
            const size_t last = next <= lastBin ? (peak + next) / 2 : lastBin;
            // Accumulate at the peak's measured frequency and lock the other bins to its phase offset.
            constexpr double radiansPerBin = twoPi / shiftConstants::overlap;
            const double expected = radiansPerBin * static_cast<double>(peak);
            const double error = std::remainder(bin[peak].angle - s.phase[peak].previous - expected, twoPi);
            const double location = std::clamp((expected + error) / radiansPerBin, 0.0, static_cast<double>(half));
            const auto offset = onset ? 0.0f : static_cast<float>(std::remainder(s.phase[peak].offset + (ratio - 1.0) * location * radiansPerBin, twoPi));
            const auto rotation = std::polar(1.0f, offset);
            const auto translation = static_cast<float>((ratio - 1.0) * (onset ? static_cast<double>(peak) : location));
            for (size_t k = first; k <= last; ++k)
            {
                s.phase[k].offset = offset;
                const float dest = static_cast<float>(k) + translation;
                if (dest <= -1.0f || dest >= static_cast<float>(half + 1))
                    continue;
                const int lo = static_cast<int>(std::floor(dest)), hi = lo + 1;
                const float fraction = dest - static_cast<float>(lo);
                const auto rotated = spectrum[k] * rotation;
                if (lo >= 0)
                    fftIn[static_cast<size_t>(lo)] += rotated * ((1.0f - fraction) * envelopeGain(k, static_cast<size_t>(lo)));
                if (hi <= half && fraction > 0)
                    fftIn[static_cast<size_t>(hi)] += rotated * (fraction * envelopeGain(k, static_cast<size_t>(hi)));
            }
            first = last + 1, peak = next;
        }
    }

    TransientMask detector;
    Delay dryDelay, maskDelay;
    Smoother amount, balance, direction, stereo, stretch, formantMix, target;
    Smoother focusLo, focusHi, focusLoMix, focusHiMix;
    juce::AudioBuffer<float> scratch;
    std::array<Channel, 2> state;
    std::vector<float> window, synthesis, lifter, mask;
    std::vector<Bin> bin;
    std::vector<Complex> spectrum, fftIn, fftOut;
    float binsPerHz = 0, attackRecovery = 0;
    size_t channelCount = 0;
    int size = 0, half = 0, hop = 0, cursor = 0, untilFrame = 0;
    bool disabled = false; // Low Latency Mode: shift is off and adds no latency
};
} // namespace snap
