#include "Check.h"
#include "dsp/modules/Press.h"
#include "loops/Loops.h"
#include <cstring>

namespace
{
using snap::Press;
using Buffer = juce::AudioBuffer<float>;
constexpr double fs = 48000, twoPi = juce::MathConstants<double>::twoPi;
constexpr int block = 64;

float sine(int i, double hz) { return static_cast<float>(std::sin(twoPi * hz * i / fs)); }
double energyDb(double ratio) { return 10.0 * std::log10(ratio); }

struct Meters { float down = 0, up = 0; };

Meters render(Press& press, Buffer& audio, int blockSize = block)
{
    Meters meters;
    for (int start = 0; start < audio.getNumSamples(); start += blockSize)
    {
        Buffer view(audio.getArrayOfWritePointers(), audio.getNumChannels(), start,
                    std::min(blockSize, audio.getNumSamples() - start));
        press.process(view, view.getNumSamples());
        meters.down = std::min(meters.down, press.downwardDb());
        meters.up = std::max(meters.up, press.upwardDb());
    }
    return meters;
}

Buffer render(const Buffer& dry, const Press::Params& params)
{
    Press press;
    press.setParams(params);
    press.prepare(fs, dry.getNumChannels(), block, false);
    Buffer wet(dry);
    render(press, wet);
    return wet;
}

bool same(const Buffer& a, const Buffer& b, int start = 0)
{
    const size_t bytes = static_cast<size_t>(a.getNumSamples() - start) * sizeof(float);
    for (int c = 0; c < a.getNumChannels(); ++c)
        if (std::memcmp(a.getReadPointer(c, start), b.getReadPointer(c, start), bytes) != 0)
            return false;
    return true;
}

double energy(const Buffer& audio, int start, int end)
{
    double sum = 0;
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = start; i < end; ++i)
        {
            const double x = audio.getSample(c, i);
            sum += x * x;
        }
    return sum;
}

void testTransparency()
{
    for (const int channels : { 1, 2 })
    {
        Buffer dry(channels, 4096 + 29);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < dry.getNumSamples(); ++i)
                dry.setSample(c, i, i == 0 ? -0.0f : loops::noise(static_cast<juce::uint32>(i + c * dry.getNumSamples())));
        for (int mode = 0; mode < 6; ++mode)
            for (const int blockSize : { 1, 64, 4096 })
            {
                Press press;
                press.setParams({ .mode = mode, .downUp = 0, .bands = 1 });
                press.prepare(fs, channels, blockSize, false);
                CHECK(press.latency() == 0);
                Buffer out(dry);
                const auto meters = render(press, out, blockSize);
                CHECK(same(out, dry));
                CHECK(meters.down == 0 && meters.up == 0);
            }
    }
}

void testBroadband()
{
    const auto dry = loops::make(2, fs, 0.7);
    Buffer out(dry);
    Press press;
    press.setParams({ .amount = 1 });
    press.prepare(fs, 2, 1, false);
    double residual = 0;
    float deepest = 0;
    // At one sample per block the meter gives the instantaneous reduction shared by both channels.
    for (int i = 0; i < out.getNumSamples(); ++i)
    {
        Buffer sample(out.getArrayOfWritePointers(), 2, i, 1);
        press.process(sample, 1);
        deepest = std::min(deepest, press.downwardDb());
        const double gain = std::pow(10.0, (press.downwardDb() + snap::pressConstants::modes[0].makeupDb) / 20.0);
        for (int c = 0; c < out.getNumChannels(); ++c)
        {
            const double error = out.getSample(c, i) - dry.getSample(c, i) * gain;
            residual += error * error;
        }
    }
    CHECK(deepest < -5);
    CHECK(energyDb(residual / energy(dry, 0, dry.getNumSamples())) <= -60);
}

double toneEnergy(const Buffer& audio, int start, int end)
{
    double inPhase = 0, quadrature = 0;
    for (int i = start; i < end; ++i)
    {
        const double phase = twoPi * 8000 * i / fs;
        inPhase += audio.getSample(1, i) * std::cos(phase);
        quadrature += audio.getSample(1, i) * std::sin(phase);
    }
    return (inPhase * inPhase + quadrature * quadrature) / ((end - start) * static_cast<double>(end - start));
}

void testBandIndependence()
{
    Buffer dry(2, 62400);
    for (int i = 0; i < dry.getNumSamples(); ++i)
    {
        const double t = (i - 48000) / fs;
        const float burst = t < 0 ? 0.0f : static_cast<float>((1 - std::exp(-t / 0.02)) * std::exp(-t / 0.20));
        dry.setSample(0, i, burst * sine(i, 60));
        dry.setSample(1, i, 0.035f * sine(i, 8000));
    }
    const auto wide = render(dry, { .amount = 1 });
    const auto split = render(dry, { .amount = 1, .bands = 1 });
    const auto duckDb = [](const Buffer& audio) { return energyDb(toneEnergy(audio, 51600, 55200) / toneEnergy(audio, 43200, 46800)); };
    const double wideChange = duckDb(wide), bandChange = duckDb(split);
    CHECK(wideChange < -5);
    CHECK(bandChange > -1 && bandChange < 2);
}

void testDirections()
{
    constexpr int hitsEnd = 72000, floorStart = 86400, tailStart = 69600, hitStart = 48000;
    Buffer dry(1, 91200);
    for (int i = 0; i < dry.getNumSamples(); ++i)
    {
        const double t = (i % 24000) / fs;
        const float hit = static_cast<float>(0.006 + 0.85 * (1 - std::exp(-t / 0.018)) * std::exp(-t / 0.075));
        const float floor = 0.0000316228f * loops::noise(static_cast<juce::uint32>(i));
        dry.setSample(0, i, i < hitsEnd ? hit * sine(i, 180) : (i < floorStart ? 0.0f : floor));
    }
    Press down;
    down.setParams({ .amount = 1, .mode = 2 });
    down.prepare(fs, 1, block, false);
    Buffer reduced(dry);
    const auto downMeters = render(down, reduced);
    const float dryPeak = dry.getMagnitude(0, hitStart, hitsEnd - hitStart);
    const float downPeak = reduced.getMagnitude(0, hitStart, hitsEnd - hitStart);
    const double peakRatio = downPeak / dryPeak;
    const double energyRatio = energy(reduced, hitStart, hitsEnd) / energy(dry, hitStart, hitsEnd);
    CHECK(peakRatio < 0.5);
    CHECK(energyDb(peakRatio * peakRatio / energyRatio) < -1);
    CHECK(downMeters.down < -3 && downMeters.up == 0);

    Press up;
    up.setParams({ .amount = 1, .downUp = 1 });
    up.prepare(fs, 1, block, false);
    Buffer lifted(dry);
    const auto upMeters = render(up, lifted);
    CHECK(energyDb(energy(lifted, tailStart, hitsEnd) / energy(dry, tailStart, hitsEnd)) > 3);
    CHECK(same(lifted, dry, hitsEnd));
    CHECK(upMeters.down == 0 && upMeters.up > 3 && upMeters.up <= 12);
    CHECK(up.upwardDb() == 0);
}

void testMix()
{
    const auto dry = loops::make(1, fs, 0.35);
    const auto muted = render(dry, { .amount = 1, .mode = 2, .downUp = 0, .mix = 0, .bands = 1 });
    const auto half = render(dry, { .amount = 1, .mode = 2, .downUp = 0, .mix = 0.5f, .bands = 1 });
    const auto wet = render(dry, { .amount = 1, .mode = 2, .downUp = 0, .bands = 1 });
    CHECK(same(muted, dry));
    CHECK(!same(wet, dry));
    float error = 0;
    for (int c = 0; c < dry.getNumChannels(); ++c)
        for (int i = 0; i < dry.getNumSamples(); ++i)
            error = std::max(error, std::abs(half.getSample(c, i) - 0.5f * (dry.getSample(c, i) + wet.getSample(c, i))));
    CHECK(error < 2.0e-7f);
}

void testRelativeThreshold()
{
    const auto input = loops::make(2, fs, 1.35);
    constexpr int settled = 24000;
    const int end = input.getNumSamples();
    const double inputEnergy = energy(input, settled, end);
    for (const int mode : { 0, 2, 5 })
    {
        const auto reduce = [&](float scale) {
            Press press;
            press.setParams({ .amount = 0.8f, .mode = mode, .bands = 0.6f });
            Buffer out(input);
            out.applyGain(scale);
            press.prepare(fs, 2, block, false);
            const auto meters = render(press, out);
            CHECK(meters.down < -4);
            return energyDb(energy(out, settled, end) / (inputEnergy * scale * scale));
        };
        const double hot = reduce(1), quiet = reduce(0.1f);
        CHECK(std::abs(hot - quiet) < 3);
    }
}

float maxStep(const Buffer& audio)
{
    float step = 0;
    for (int i = 1; i < audio.getNumSamples(); ++i)
        step = std::max(step, std::abs(audio.getSample(0, i) - audio.getSample(0, i - 1)));
    return step;
}

void testAutomation()
{
    constexpr int modeSamples = block * 45, amountStart = modeSamples * 9, stop = amountStart + modeSamples * 6;
    Buffer dry(1, stop + modeSamples);
    for (int i = 0; i < dry.getNumSamples(); ++i)
        dry.setSample(0, i, 0.6f * sine(i, 173));
    Buffer out(dry);
    Press press;
    press.prepare(fs, 1, block, false);
    for (int start = 0; start < dry.getNumSamples(); start += block)
    {
        const float amount = start >= amountStart && start < stop ? 1.0f : 0.0f;
        const int mode = std::clamp((start - amountStart) / modeSamples, 0, 5);
        Buffer view(out.getArrayOfWritePointers(), 1, start, block);
        press.setParams({ .amount = amount, .mode = mode });
        press.process(view, block);
    }
    CHECK(!same(out, dry));
    CHECK(maxStep(out) < 1.5f * maxStep(dry));
    CHECK(same(out, dry, stop + static_cast<int>(fs * snap::constants::smoothingSeconds)));
}

void testAllocations()
{
    Press press;
    press.prepare(fs, 2, block, false);
    Buffer audio(2, block);
    setAudioThread(true);
    press.reset();
    for (int b = 0; b < 200; ++b)
    {
        const float amount = static_cast<float>(b % 17) / 16.0f;
        press.setParams({ amount, b % 6, 2 * amount - 1, 1 - amount, amount });
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
                audio.setSample(c, i, loops::noise(static_cast<juce::uint32>(i + block * (2 * b + c))));
        press.process(audio, block);
    }
    setAudioThread(false);
    CHECK(takeAudioAllocations() == 0);
}

void testBounded()
{
    for (const double rate : { 44100.0, 192000.0 })
        for (const int channels : { 1, 2 })
        {
            Buffer dry(channels, static_cast<int>(rate * 0.24));
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < dry.getNumSamples(); ++i)
                    dry.setSample(c, i, loops::noise(static_cast<juce::uint32>(i + c * dry.getNumSamples())));
            for (int mode = 0; mode < 6; ++mode)
            {
                Press press;
                press.setParams({ .amount = 1, .mode = mode, .downUp = 0, .bands = 1 });
                press.prepare(rate, channels, 4096, true);
                Buffer out(dry);
                render(press, out, 4096);
                bool bounded = true;
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < out.getNumSamples(); ++i)
                    {
                        const float x = out.getSample(c, i);
                        bounded = bounded && std::isfinite(x) && std::abs(x) < 15.8489f;
                    }
                CHECK(bounded);
            }
        }
}


void testLullHasNoDc() // the even-harmonic shaper must not leave an offset
{
    Buffer dry(1, 48000);
    for (int i = 0; i < dry.getNumSamples(); ++i)
        dry.setSample(0, i, 0.8f * sine(i, 110));
    const auto wet = render(dry, { .amount = 1, .mode = 4, .downUp = -1 });
    double mean = 0;
    for (int i = 24000; i < 48000; ++i) // an exact number of 110 Hz cycles is not needed: the sine's mean over 0.5 s is tiny
        mean += wet.getSample(0, i);
    CHECK(std::abs(mean / 24000) < 1.0e-3);
}
} // namespace

void testPress()
{
    testTransparency();
    testBroadband();
    testBandIndependence();
    testDirections();
    testMix();
    testRelativeThreshold();
    testAutomation();
    testAllocations();
    testBounded();
    testLullHasNoDc();
}
