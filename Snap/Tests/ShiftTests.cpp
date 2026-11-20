#include "Check.h"
#include "dsp/modules/Shift.h"
#include "loops/Loops.h"
#include <bit>
#include <cstdint>

namespace
{
using snap::Shift;
using Buffer = juce::AudioBuffer<float>;
constexpr double fs = 48000, twoPi = juce::MathConstants<double>::twoPi;
constexpr int delay = 2048, analysisSamples = 9600;

float sine(int i, double hz, double rate = fs) { return static_cast<float>(std::sin(twoPi * hz * i / rate)); }

Buffer tone(int channels, double hz, double rate = fs)
{
    Buffer in(channels, static_cast<int>(rate * 0.35));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < in.getNumSamples(); ++i)
            in.setSample(c, i, 0.4f * sine(i, hz, rate));
    return in;
}

Buffer render(const Buffer& in, const Shift::Params& p, int block = 64, double rate = fs, bool lowLatency = false)
{
    Shift shift;
    shift.setParams(p);
    shift.prepare(rate, in.getNumChannels(), block, lowLatency);
    CHECK(shift.latency() == Shift::latencyFor(rate, lowLatency));
    Buffer out(in);
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int n = std::min(block, out.getNumSamples() - start);
        Buffer view(out.getArrayOfWritePointers(), out.getNumChannels(), start, n);
        shift.process(view, n);
    }
    return out;
}

bool exactDelay(const Buffer& in, const Buffer& out, int latency)
{
    for (int c = 0; c < in.getNumChannels(); ++c)
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            const float expected = i < latency ? 0.0f : in.getSample(c, i - latency);
            if (std::bit_cast<std::uint32_t>(out.getSample(c, i)) != std::bit_cast<std::uint32_t>(expected))
                return false;
        }
    return true;
}

float peak(const Buffer& in, int start, int end) { return in.getMagnitude(0, start, end - start); }

double energy(const Buffer& in, int start, int end, const Buffer* dry = nullptr)
{
    double sum = 0;
    for (int i = start; i < end; ++i)
    {
        const double x = static_cast<double>(in.getSample(0, i)) - (dry != nullptr ? dry->getSample(0, i) : 0.0f);
        sum += x * x;
    }
    return sum;
}

double bin(const Buffer& in, double hz, int channel = 0, const Buffer* dry = nullptr)
{
    double re = 0, im = 0;
    for (int i = 0; i < analysisSamples; ++i)
    {
        const int sample = in.getNumSamples() - analysisSamples + i;
        const double x = static_cast<double>(in.getSample(channel, sample)) - (dry != nullptr ? dry->getSample(channel, sample) : 0.0f);
        const double phase = twoPi * hz * i / fs;
        re += x * std::cos(phase), im += x * std::sin(phase);
    }
    return 2 * std::hypot(re, im) / analysisSamples;
}

void checkPitch(const Buffer& in, double expected, int channel = 0)
{
    const int first = in.getNumSamples() - analysisSamples;
    double firstCrossing = 0, lastCrossing = 0;
    int crossings = 0;
    for (int i = first + 1; i < in.getNumSamples(); ++i)
    {
        const double previous = in.getSample(channel, i - 1), current = in.getSample(channel, i);
        if (previous < 0 && current >= 0)
        {
            lastCrossing = i - 1 - previous / (current - previous);
            if (crossings++ == 0)
                firstCrossing = lastCrossing;
        }
    }
    CHECK(crossings > 1 && std::abs(fs * (crossings - 1) / (lastCrossing - firstCrossing) / expected - 1) < 0.01);
    CHECK(bin(in, expected, channel) > 0.035);
}

void testDelayAndReconstruction()
{
    for (int channels : { 1, 2 })
    {
        Buffer in(channels, 8513);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < in.getNumSamples(); ++i)
                in.setSample(c, i, i % 79 == 0 ? -0.0f : loops::noise(static_cast<juce::uint32>(i + c * in.getNumSamples())));

        Shift::Params moved;
        moved.balance = -0.72f, moved.stretch = 1, moved.stereo = -0.86f, moved.invert = true;
        moved.target = -1, moved.formantMix = 0.41f, moved.focusLo = 700, moved.focusHi = 5000;
        for (int block : { 1, 64, 4096 })
        {
            CHECK(exactDelay(in, render(in, {}, block), delay));
            CHECK(exactDelay(in, render(in, moved, block), delay));
        }
        moved.amount = 1;
        CHECK(Shift::latencyFor(fs, true) == 0);
        CHECK(exactDelay(in, render(in, moved, 4096, fs, true), 0));
    }

    Buffer noise(1, 14000);
    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample(0, i, loops::noise(static_cast<juce::uint32>(i)));
    const auto sin = tone(1, 220);
    for (const auto* in : { static_cast<const Buffer*>(&noise), &sin })
    {
        const auto dry = render(*in, {});
        Shift::Params p;
        p.amount = 1, p.balance = 1, p.formantMix = 0;
        const auto identity = render(*in, p, 4096);
        CHECK(energy(identity, 0, in->getNumSamples(), &dry) <= energy(dry, 0, in->getNumSamples()) * 1.0e-8);
        p.amount = 0.02f, p.balance = 0, p.formantMix = 1;
        const auto small = render(*in, p);
        CHECK(energy(small, 0, in->getNumSamples(), &dry) <= energy(dry, 0, in->getNumSamples()) * 1.0e-4);
    }
}

void testFrequency()
{
    const auto in = tone(1, 220);
    Shift::Params p;
    p.balance = -1, p.amount = 1;
    const auto up = render(in, p);
    checkPitch(up, 440);
    CHECK(bin(up, 440) > 10 * bin(up, 220));
    p.amount = -1;
    checkPitch(render(in, p), 110);
    p.amount = std::sqrt(7.0f / 12.0f);
    checkPitch(render(in, p), 220 * std::exp2(7.0 / 12));
    p.amount = 1, p.invert = true;
    checkPitch(render(in, p), 110);
    p.invert = false, p.stereo = -1;
    const auto stereo = render(tone(2, 220), p, 4096);
    checkPitch(stereo, 440, 0), checkPitch(stereo, 220, 1);
    checkPitch(render(in, p, 1), 440);
}

void testFormants()
{
    constexpr int fundamental = 150, harmonics = 40;
    Buffer in(1, 16800);
    in.clear();
    for (int h = 1; h <= harmonics; ++h)
    {
        const double distance = std::log2(h * fundamental / 900.0);
        const auto gain = static_cast<float>((h == 1 ? 0.3 : 0.12) * std::exp(-0.5 * distance * distance) / std::sqrt(h));
        for (int i = 0; i < in.getNumSamples(); ++i)
            in.addSample(0, i, (h == 1 ? 0.22f : gain) * sine(i, h * fundamental));
    }
    const auto centroid = [](const Buffer& out) {
        double total = 0, weighted = 0;
        for (int h = 1; h <= harmonics; ++h)
        {
            const double a = bin(out, h * fundamental), power = a * a;
            total += power, weighted += power * h * fundamental;
        }
        return weighted / total;
    };
    const auto dry = render(in, {});
    const double centre = centroid(dry);
    Shift::Params p;
    p.balance = 1, p.stretch = -1, p.amount = 1;
    const auto up = render(in, p);
    CHECK(bin(up, fundamental) > 0.05 && bin(up, fundamental + 10) < 0.01);
    CHECK(centroid(up) > centre * 1.08);
    p.amount = -1;
    CHECK(centroid(render(in, p)) < centre * 0.95);
    p.amount = 1, p.stretch = 1;
    const auto highPivot = render(in, p);
    CHECK(std::abs(bin(highPivot, fundamental, 0, &dry)) < 0.25 * bin(up, fundamental, 0, &dry));
    p.stretch = -1, p.formantMix = 0;
    CHECK(std::abs(centroid(render(in, p)) / centre - 1) < 0.001);
}

void testAttacks()
{
    Buffer hits(1, 19200);
    hits.clear();
    for (int hit : { 2700, 7950, 13750 })
    {
        hits.setSample(0, hit, 0.8f);
        for (int i = 1; i < 2300; ++i)
            hits.setSample(0, hit + i, 0.2f * sine(i, 170) * std::exp(-static_cast<float>(i) / 420.0f));
    }
    Shift::Params p;
    p.balance = -1, p.amount = std::sqrt(5.0f / 12.0f);
    const auto shifted = render(hits, p);
    const auto dry = render(hits, {});
    for (int hit : { 2700, 7950, 13750 })
    {
        const int onset = hit + delay, end = onset + 240;
        const double crest = peak(shifted, onset, end) / std::sqrt(energy(shifted, onset, end));
        const double dryCrest = peak(dry, onset, end) / std::sqrt(energy(dry, onset, end));
        CHECK(crest > dryCrest * 0.5); // the attack stays at least as sharp (sharper is fine)
        CHECK(peak(shifted, onset - 480, onset) < 0.1f * peak(dry, onset, end));
    }
}

void testFocusAndTarget()
{
    Buffer in(1, 16800);
    for (int i = 0; i < in.getNumSamples(); ++i)
        in.setSample(0, i, 0.35f * sine(i, 100) + 0.4f * sine(i, 3000));
    Shift::Params p;
    p.focusLo = 2000;
    const auto dry = render(in, p);
    p.amount = 1, p.balance = -1;
    const auto shifted = render(in, p);
    CHECK(bin(shifted, 100, 0, &dry) < 0.00035);
    const double shiftedTone = bin(shifted, 6000), originalTone = bin(shifted, 3000);
    CHECK(shiftedTone > 0.05 && originalTone < 0.1);

    constexpr int hit = 2900, onset = hit + delay;
    for (int i = 0; i < in.getNumSamples(); ++i)
    {
        const float amp = i < hit ? 0.01f : 0.12f + 0.74f * std::exp(-static_cast<float>(i - hit) / 3400.0f);
        in.setSample(0, i, amp * sine(i - hit, 280));
    }
    p.focusLo = snap::constants::focusMinHz;
    const auto base = render(in, {}), full = render(in, p);
    p.target = -1;
    const auto transient = render(in, p);
    p.target = 1;
    const auto sustain = render(in, p);
    CHECK(energy(transient, onset, onset + 320, &base) > 2 * energy(sustain, onset, onset + 320, &base));
    CHECK(energy(sustain, onset + 6700, onset + 11500, &base) > 100 * energy(transient, onset + 6700, onset + 11500, &base));

    const int lookahead = static_cast<int>(std::lround(fs * snap::constants::maskLookaheadMs * 0.001));
    snap::TransientMask detector;
    detector.prepare(fs, 1, lookahead), detector.setLookahead(lookahead);
    std::vector<float> mask(static_cast<size_t>(in.getNumSamples()));
    detector.process(in, in.getNumSamples(), mask.data());
    float error = 0;
    for (int i = delay - lookahead; i < full.getNumSamples(); ++i)
    {
        const float expected = base.getSample(0, i) + (full.getSample(0, i) - base.getSample(0, i)) * mask[static_cast<size_t>(i - delay + lookahead)];
        error = std::max(error, std::abs(expected - transient.getSample(0, i)));
    }
    CHECK(error < 2.0e-7f);
}

void testAutomationAndRates()
{
    Shift shift;
    Shift::Params p;
    constexpr int block = 64;
    Buffer buf(2, block);
    shift.prepare(fs, 2, block, false);
    float last = 0, step = 0;
    bool finiteAndBounded = true, returnedToDry = true;
    takeAudioAllocations();
    for (int b = 0; b < 390; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const float x = 0.4f * sine(b * block + i, 730);
            buf.setSample(0, i, x), buf.setSample(1, i, -x);
        }
        p.amount = b < 35 || b >= 350 ? 0.0f : static_cast<float>(b % 41) / 20.0f - 1.0f;
        p.balance = -1, p.invert = b % 67 >= 34, p.formantMix = (b % 3 == 0) ? 0.0f : 1.0f;
        p.stereo = b % 2 == 0 ? -1.0f : 1.0f;
        setAudioThread(true);
        shift.setParams(p), shift.process(buf, block);
        setAudioThread(false);
        CHECK(shift.latency() == delay);
        for (int i = 0; i < block; ++i)
        {
            const float y = buf.getSample(0, i);
            step = std::max(step, std::abs(y - last)), last = y;
            finiteAndBounded &= std::isfinite(y) && std::abs(y) < 1.6f
                               && std::isfinite(buf.getSample(1, i)) && std::abs(buf.getSample(1, i)) < 1.6f;
            if (b >= 365)
                returnedToDry &= y == 0.4f * sine(b * block + i - delay, 730);
        }
    }
    setAudioThread(true);
    shift.reset();
    setAudioThread(false);
    CHECK(takeAudioAllocations() == 0);
    CHECK(finiteAndBounded && returnedToDry && step < 0.22f);

    const auto checkRate = [](double rate, int latency) {
        CHECK(Shift::latencyFor(rate, false) == latency && Shift::latencyFor(rate, true) == 0);
        Shift::Params params;
        params.amount = 1, params.balance = -0.35f;
        const auto out = render(tone(1, 292, rate), params, 4096, rate);
        const auto* samples = out.getReadPointer(0);
        CHECK(std::all_of(samples, samples + out.getNumSamples(), [](float x) { return std::isfinite(x) && std::abs(x) < 1.6f; }));
        CHECK(peak(out, latency + latency, out.getNumSamples()) > 0.03f);
    };
    checkRate(44100, 2048), checkRate(96000, 4096), checkRate(192000, 8192);
}
} // namespace

void testShift()
{
    testDelayAndReconstruction();
    testFrequency();
    testFormants();
    testAttacks();
    testFocusAndTarget();
    testAutomationAndRates();
}
