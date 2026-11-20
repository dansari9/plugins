#include "Check.h"
#include "dsp/modules/Drive.h"
#include "loops/Loops.h"
#include <bit>
#include <cstdint>

namespace
{
using snap::Drive;
using Buffer = juce::AudioBuffer<float>;
constexpr double fs = 48000, twoPi = juce::MathConstants<double>::twoPi;
constexpr int delay = 144, modes = static_cast<int>(snap::driveConstants::flavorLabels.size());

float sine(int i, double hz, double rate = fs) { return static_cast<float>(std::sin(twoPi * hz * i / rate)); }

Buffer render(const Buffer& in, Drive::Params p, int block = 64, bool lowLatency = false, double rate = fs)
{
    Drive drive;
    drive.setParams(p);
    drive.prepare(rate, in.getNumChannels(), block, lowLatency);
    CHECK(drive.latency() == (lowLatency ? snap::constants::lowLatencyMaskLookahead : static_cast<int>(std::lround(rate * 0.003))));
    Buffer out(in);
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int n = std::min(block, out.getNumSamples() - start);
        Buffer view(out.getArrayOfWritePointers(), out.getNumChannels(), start, n);
        drive.process(view, n);
    }
    return out;
}

double energy(const Buffer& a, int start, int end, const Buffer* dry = nullptr)
{
    double sum = 0;
    for (int i = start; i < end; ++i)
    {
        const double x = a.getSample(0, i) - (dry != nullptr ? dry->getSample(0, i) : 0.0f);
        sum += x * x;
    }
    return sum;
}

double bin(const Buffer& a, double hz, int n, const Buffer* dry = nullptr, double rate = fs)
{
    double re = 0, im = 0;
    for (int i = a.getNumSamples() - n; i < a.getNumSamples(); ++i)
    {
        const double x = a.getSample(0, i) - (dry != nullptr ? dry->getSample(0, i) : 0.0f);
        const double phase = twoPi * hz * i / rate;
        re += x * std::cos(phase), im += x * std::sin(phase);
    }
    return 2 * std::hypot(re, im) / n;
}

void testDelayAndMix()
{
    for (int channels : { 1, 2 })
    {
        Buffer in(channels, 4251);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < in.getNumSamples(); ++i)
                in.setSample(c, i, loops::noise(static_cast<juce::uint32>(i + c * in.getNumSamples())));

        for (int mode = 0; mode < modes; ++mode)
        {
            auto checkDelayed = [&](const Buffer& out, int latency) {
                bool exact = true;
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < in.getNumSamples(); ++i)
                    {
                        const float expected = i < latency ? 0.0f : in.getSample(c, i - latency);
                        exact &= std::bit_cast<std::uint32_t>(out.getSample(c, i)) == std::bit_cast<std::uint32_t>(expected);
                    }
                CHECK(exact);
            };
            Drive::Params p;
            p.mode = mode;
            for (bool lowLatency : { false, true })
                for (int block : { 1, 64, 4096 })
                    checkDelayed(render(in, p, block, lowLatency), lowLatency ? snap::constants::lowLatencyMaskLookahead : delay);

            p.amount = 1, p.mix = 0;
            const auto dry = render(in, p);
            checkDelayed(dry, delay);
            p.mix = 0.5f;
            const auto half = render(in, p);
            p.mix = 1;
            const auto wet = render(in, p);
            float error = 0;
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < in.getNumSamples(); ++i)
                    error = std::max(error, std::abs(half.getSample(c, i) - 0.5f * (dry.getSample(c, i) + wet.getSample(c, i))));
            CHECK(error < 2.0e-7f);
        }
    }
}

void testFocusAndTarget()
{
    Buffer in(1, 19200);
    for (int i = 0; i < in.getNumSamples(); ++i)
        in.setSample(0, i, 0.32f * sine(i, 60) + 0.55f * sine(i, 5000));
    Drive::Params p;
    p.focusLo = 2000;
    const auto focusedDry = render(in, p);
    p.amount = 1;
    const auto focused = render(in, p);
    const double bass = bin(focused, 60, 9600);
    CHECK(std::abs(20 * std::log10(bass / 0.32)) < 0.2);
    for (double hz : { 0.0, 60.0, 120.0, 180.0 })
        CHECK(bin(focused, hz, 9600, &focusedDry) < bass * 0.001);
    CHECK(bin(focused, 15000, 9600, &focusedDry) > 0.01);

    const int hit = 4800;
    for (int i = 0; i < in.getNumSamples(); ++i)
    {
        const float amp = i < hit ? 0.01f : 0.12f + 0.74f * std::exp(-static_cast<float>(i - hit) / 3400.0f);
        in.setSample(0, i, amp * sine(i - hit, 200));
    }
    p = {};
    const auto dry = render(in, p);
    p.amount = 1;
    const auto wet = render(in, p);
    p.target = -1;
    const auto transient = render(in, p);
    p.target = 1;
    const auto sustain = render(in, p);
    const int onset = hit + delay, headEnd = onset + 320, tailStart = onset + 6700, tailEnd = onset + 11500;
    const double head = energy(wet, onset, headEnd, &dry), tail = energy(wet, tailStart, tailEnd, &dry);
    CHECK(head > 0.01 && tail > 0.001);
    CHECK(energy(transient, onset, headEnd, &dry) > head * 0.50);
    CHECK(energy(sustain, onset, headEnd, &dry) < head * 0.10);
    CHECK(energy(transient, tailStart, tailEnd, &dry) < tail * 0.001);
    CHECK(energy(sustain, tailStart, tailEnd, &dry) > tail * 0.90);
}

void testVoices()
{
    Buffer in(1, 9600);
    for (int i = 0; i < in.getNumSamples(); ++i)
        in.setSample(0, i, 0.69f * sine(i, 1100));
    const auto loop = loops::make(1, fs, 0.25);
    const auto clean = render(loop, {});
    for (int mode = 0; mode < modes; ++mode)
    {
        Drive::Params p;
        p.mode = mode, p.amount = 0.08f;
        const auto light = render(in, p);
        p.amount = 1;
        const auto heavy = render(in, p);
        const auto thd = [](const Buffer& b) {
            double harmonics = 0;
            for (int h = 2; h <= 16; ++h)
            {
                const double harmonic = bin(b, h * 1100, 4800);
                harmonics += harmonic * harmonic;
            }
            return std::sqrt(harmonics) / bin(b, 1100, 4800);
        };
        const double lightThd = thd(light), heavyThd = thd(heavy);
        if (heavyThd <= std::max(0.018, 1.8 * lightThd))
            std::cerr << "Drive mode " << mode << " light THD " << lightThd << ", full THD " << heavyThd << '\n';
        CHECK(heavyThd > std::max(0.018, 1.8 * lightThd));

        p.flavor = -1;
        const auto left = render(in, p);
        p.flavor = 1;
        const auto right = render(in, p);
        double difference = 0;
        for (int h = 1; h <= 8; ++h)
            difference += std::abs(bin(left, h * 1100, 4800) - bin(right, h * 1100, 4800));
        CHECK(difference > 0.015);
        for (float flavor : { -1.0f, 0.0f, 1.0f })
        {
            p.flavor = flavor;
            const auto saturated = render(loop, p);
            const int end = loop.getNumSamples();
            const double change = 10 * std::log10(energy(saturated, delay, end) / energy(clean, delay, end));
            CHECK(std::abs(change) <= 3.0);
        }
    }

    for (double rate : { fs, 192000.0 })
    {
        const int n = static_cast<int>(rate / 10);
        Buffer tone(1, 2 * n);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            tone.setSample(0, i, 0.65f * sine(i, 7000, rate));
        Drive::Params p;
        p.mode = 4, p.flavor = -1, p.amount = 1;
        CHECK(bin(render(tone, p, 64, false, rate), 4000, n, nullptr, rate) > 0.12);
    }
}

void testAutomationAndBounds()
{
    Drive drive;
    Drive::Params p;
    constexpr int block = 64;
    Buffer buf(2, block);
    drive.prepare(fs, 2, block, false);
    float lastIn = 0, lastOut = 0, inputStep = 0, outputStep = 0;
    bool returnedToDry = true;
    takeAudioAllocations();
    for (int b = 0; b < 144; ++b)
    {
        for (int i = 0; i < block; ++i)
        {
            const float x = 0.42f * sine(b * block + i, 730);
            inputStep = std::max(inputStep, std::abs(x - lastIn)), lastIn = x;
            buf.setSample(0, i, x), buf.setSample(1, i, -x);
        }
        p.amount = b < 10 || b >= 120 ? 0.0f : static_cast<float>(b % 23) / 22;
        p.flavor = b % 3 == 0 ? -1.0f : 1.0f;
        p.mode = (b / 9) % modes;
        setAudioThread(true);
        drive.setParams(p), drive.process(buf, block);
        setAudioThread(false);
        for (int i = 0; i < block; ++i)
        {
            const float y = buf.getSample(0, i);
            outputStep = std::max(outputStep, std::abs(y - lastOut)), lastOut = y;
            if (b >= 135)
                returnedToDry &= y == 0.42f * sine(b * block + i - delay, 730);
        }
    }
    setAudioThread(true);
    drive.reset();
    setAudioThread(false);
    CHECK(takeAudioAllocations() == 0);
    CHECK(returnedToDry && outputStep < 2.1f * inputStep);

    Buffer noise(1, 8192);
    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample(0, i, loops::noise(static_cast<juce::uint32>(i)));
    for (double rate : { 44100.0, 192000.0, 768000.0 })
        for (int mode = 0; mode < modes; ++mode)
        {
            p = {};
            p.mode = mode, p.amount = 1;
            const auto out = render(noise, p, 4096, false, rate);
            CHECK(std::all_of(out.getReadPointer(0), out.getReadPointer(0) + out.getNumSamples(),
                              [](float v) { return std::isfinite(v) && std::abs(v) < 2.0f; }));
        }
}
} // namespace

void testDrive()
{
    testDelayAndMix();
    testFocusAndTarget();
    testVoices();
    testAutomationAndBounds();
}
