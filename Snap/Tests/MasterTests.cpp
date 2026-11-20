#include "Check.h"
#include "dsp/master/Master.h"
#include "loops/Loops.h"
#include <cstring>

namespace
{
using snap::Master;
using Buffer = juce::AudioBuffer<float>;
constexpr double fs = 48000, twoPi = juce::MathConstants<double>::twoPi;
constexpr int block = 64;

float sine(int i, double hz, double rate = fs)
{
    return static_cast<float>(std::sin(twoPi * hz * i / rate));
}

Buffer makeSine(double hz, float level = 0.2f, int channels = 1, float rightGain = 1, int samples = 9600)
{
    Buffer audio(channels, samples);
    for (int i = 0; i < samples; ++i)
    {
        const float x = level * sine(i, hz);
        audio.setSample(0, i, x);
        if (channels == 2)
            audio.setSample(1, i, rightGain * x);
    }
    return audio;
}

Buffer makeNoise(int channels, int samples)
{
    Buffer audio(channels, samples);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < samples; ++i)
            audio.setSample(c, i, i == 0 ? -0.0f : loops::noise(static_cast<juce::uint32>(i + samples * c)));
    return audio;
}

struct Meters { float shaperDb = 0; bool clipped = false; };

Meters process(Master& master, Buffer& audio, int blockSize = block, const Buffer* post = nullptr)
{
    Meters meters;
    for (int start = 0; start < audio.getNumSamples(); start += blockSize)
    {
        const int n = std::min(blockSize, audio.getNumSamples() - start);
        Buffer view(audio.getArrayOfWritePointers(), audio.getNumChannels(), start, n);
        std::array<const float*, 2> postData {};
        if (post != nullptr)
            for (int c = 0; c < post->getNumChannels(); ++c)
                postData[static_cast<size_t>(c)] = post->getReadPointer(c, start);
        master.process(view, n, post != nullptr ? postData.data() : nullptr);
        const float gain = master.shaperGainDb();
        if (std::abs(gain) > std::abs(meters.shaperDb))
            meters.shaperDb = gain;
        meters.clipped |= master.clipping();
    }
    return meters;
}

Buffer render(const Buffer& input, const Master::Params& params, int blockSize = block, double rate = fs)
{
    Master master;
    master.setParams(params);
    master.prepare(rate, input.getNumChannels(), blockSize);
    Buffer output(input);
    process(master, output, blockSize);
    return output;
}

bool sameDelayed(const Buffer& out, const Buffer& in, int delay, int from = 0)
{
    const int start = std::max(delay, from);
    const size_t bytes = static_cast<size_t>(out.getNumSamples() - start) * sizeof(float);
    for (int c = 0; c < in.getNumChannels(); ++c)
    {
        for (int i = from; i < start; ++i)
            if (out.getSample(c, i) != 0)
                return false;
        if (std::memcmp(out.getReadPointer(c, start), in.getReadPointer(c, start - delay), bytes) != 0)
            return false;
    }
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

double gainDb(double hz, const Master::Params& params)
{
    const auto dry = makeSine(hz);
    const auto wet = render(dry, params);
    constexpr int start = 4800, end = 9600;
    return 10 * std::log10(energy(wet, start, end) / energy(dry, start, end));
}

void testTransparency()
{
    for (const double rate : { 44100.0, fs, 96000.0, 192000.0 })
        for (const int channels : { 1, 2 })
        {
            const auto dry = makeNoise(channels, 4096 + 569);
            for (const int blockSize : { 1, block, 4096 })
                for (const bool lowLatency : { false, true })
                    for (const bool eqOn : { true, false })
                    {
                        Master master;
                        master.setParams({ .eqOn = eqOn, .lowLatency = lowLatency });
                        master.prepare(rate, channels, blockSize);
                        Buffer wet(dry);
                        const auto meters = process(master, wet, blockSize);
                        CHECK(sameDelayed(wet, dry, master.latency()));
                        CHECK(meters.shaperDb == 0 && !meters.clipped);
                    }
        }

    const auto dry = makeNoise(2, 4900);
    const int delay = Master::latencyFor(fs, false);
    for (int shape = 0; shape < 4; ++shape)
        CHECK(sameDelayed(render(dry, { .lcShape = 1, .hcShape = 1, .toneShape = shape }), dry, delay));
    CHECK(sameDelayed(render(dry, { .eqOn = false, .cutLo = 1900, .cutHi = 1300, .tone = -1, .toneShape = 2 }),
                      dry, delay));
}

void testCut()
{
    CHECK(gainDb(50, { .cutLo = 200 }) < -10);
    CHECK(gainDb(50, { .cutLo = 200, .lcShape = 1 }) < -20);
    for (int shape : { 0, 1 })
        CHECK(std::abs(gainDb(2000, { .cutLo = 200, .lcShape = shape })) < 1);
    CHECK(gainDb(240, { .cutLo = 200, .lcShape = 1 }) > 1);
    CHECK(gainDb(10000, { .cutHi = 2000 }) < -6);
    CHECK(gainDb(10000, { .cutHi = 2000, .hcShape = 1 }) < -30);

    for (int shape : { 0, 1 })
    {
        const double almostOpen = gainDb(50, { .cutLo = 25, .lcShape = shape });
        CHECK(std::abs(almostOpen) < 0.6);
        CHECK(std::abs(gainDb(50, { .cutLo = 20.5f, .lcShape = shape })) < 0.02);
    }
}

void testTone()
{
    for (const float t : { -1.0f, 1.0f })
    {
        const double low = -t * gainDb(100, { .tone = t });
        const double high = t * gainDb(8000, { .tone = t });
        CHECK(low > 4 && low < 7 && high > 4 && high < 7);
        CHECK(t * gainDb(70, { .tone = t, .toneShape = 1 }) > 2);
        CHECK(t * gainDb(300, { .tone = t, .toneShape = 1 }) < -2);
    }

    const Master::Params smile { .tone = -1, .toneShape = 2 };
    const double mids = gainDb(1000, smile);
    CHECK(mids < gainDb(100, smile) - 7 && mids < gainDb(8000, smile) - 7);

    const Master::Params widen { .tone = 1, .toneShape = 3 }, tighten { .tone = -1, .toneShape = 3 };
    const auto side = makeSine(8000, 0.2f, 2, -1), wide = render(side, widen), narrow = render(side, tighten);
    CHECK(energy(wide, 4800, 9600) > 2 * energy(side, 4800, 9600));
    CHECK(energy(narrow, 4800, 9600) < 0.7 * energy(side, 4800, 9600));
    CHECK(gainDb(2500, tighten) > 3);
    for (const auto& params : { widen, tighten })
    {
        const auto dualMono = render(makeSine(8000, 0.2f, 2), params);
        const auto mono = render(makeSine(8000), params);
        const auto bytes = static_cast<size_t>(mono.getNumSamples()) * sizeof(float);
        CHECK(std::memcmp(dualMono.getReadPointer(0), dualMono.getReadPointer(1), bytes) == 0);
        CHECK(std::memcmp(dualMono.getReadPointer(0), mono.getReadPointer(0), bytes) == 0);
    }
}

std::array<double, 2> bandEnergy(const Buffer& audio, int offset)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft(order);
    juce::dsp::WindowingFunction<float> window(size, juce::dsp::WindowingFunction<float>::hann);
    std::array<float, 2 * size> data {};
    std::array<double, 2> bands {};
    const int frames = (audio.getNumSamples() - Master::latencyFor(fs, false)) / size;
    for (int frame = 0; frame < frames; ++frame)
        for (int c = 0; c < audio.getNumChannels(); ++c)
        {
            std::copy_n(audio.getReadPointer(c, offset + frame * size), size, data.begin());
            window.multiplyWithWindowingTable(data.data(), size);
            fft.performFrequencyOnlyForwardTransform(data.data(), true);
            for (int bin = 0; bin <= size / 2; ++bin)
            {
                const double hz = fs * bin / size, magnitude = data[static_cast<size_t>(bin)];
                if (hz < 2000)
                    bands[0] += magnitude * magnitude;
                if (hz > 7000)
                    bands[1] += magnitude * magnitude;
            }
        }
    return bands;
}

void testAir()
{
    const int delay = Master::latencyFor(fs, false);
    const auto checkBands = [delay](const Buffer& dry, const Buffer& wet)
    {
        const auto dryBands = bandEnergy(dry, 0), wetBands = bandEnergy(wet, delay);
        const double lowDb = 10 * std::log10(wetBands[0] / dryBands[0]);
        const double highDb = 10 * std::log10(wetBands[1] / dryBands[1]);
        CHECK(std::abs(lowDb) < 0.5);
        CHECK(highDb > 1.5);
    };

    const auto kick = loops::make(0, fs, 1.0);
    checkBands(kick, render(kick, { .air = 1 }));

    // Darken the kit so its broadband snare leaves headroom for upward compression.
    auto dry = loops::make(2, fs, 1.0);
    for (int c = 0; c < dry.getNumChannels(); ++c)
    {
        float state = 0;
        for (int i = 0; i < dry.getNumSamples(); ++i)
        {
            state += 0.15f * (dry.getSample(c, i) - state);
            dry.setSample(c, i, state);
        }
    }
    const auto wet = render(dry, { .air = 1 });
    checkBands(dry, wet);

    Buffer silent(2, 4096);
    silent.clear();
    CHECK(sameDelayed(render(silent, { .air = 1 }), silent, delay));
    const auto quiet = makeSine(1000, 0.0009f, 2, -1);
    CHECK(sameDelayed(render(quiet, { .air = 1 }), quiet, delay));

    const auto small = render(dry, { .air = 0.01f });
    float error = 0, change = 0;
    for (int c = 0; c < dry.getNumChannels(); ++c)
        for (int i = delay; i < dry.getNumSamples(); ++i)
        {
            const float x = dry.getSample(c, i - delay), delta = small.getSample(c, i) - x;
            error = std::max(error, std::abs(delta - 0.01f * (wet.getSample(c, i) - x)));
            change = std::max(change, std::abs(delta));
        }
    CHECK(error < 2.0e-7f && change > 1.0e-5f);
}

void testShaper()
{
    constexpr int kick = 2400, snare = 36000, head = 480, tailStart = 2880, tailEnd = 9600;
    Buffer dry(2, snare + tailEnd + 1024);
    dry.clear();
    for (int i = 0; i < tailEnd; ++i)
    {
        const float k = loops::kick(i / fs);
        const float s = loops::snare(i / fs, static_cast<juce::uint32>(i));
        dry.setSample(0, kick + i, k), dry.setSample(1, kick + i, k);
        dry.setSample(0, snare + i, s), dry.setSample(1, snare + i, 0.8f * s);
    }

    const int delay = Master::latencyFor(fs, false);
    const auto attackUp = render(dry, { .attack = 1 }), attackDown = render(dry, { .attack = -1 });
    const auto sustainUp = render(dry, { .sustain = 1 }), sustainDown = render(dry, { .sustain = -1 });
    for (const int hit : { kick, snare })
    {
        const float peak = dry.getMagnitude(0, hit, head);
        CHECK(attackUp.getMagnitude(0, hit + delay, head) > 1.5f * peak);
        CHECK(attackDown.getMagnitude(0, hit + delay, head) < 0.7f * peak);
        const double tail = energy(dry, hit + tailStart, hit + tailEnd);
        CHECK(energy(sustainUp, hit + tailStart + delay, hit + tailEnd + delay) > 2 * tail);
        CHECK(energy(sustainDown, hit + tailStart + delay, hit + tailEnd + delay) < 0.5 * tail);
    }

    Master master;
    master.setParams({ .attack = -1, .sustain = -1 });
    master.prepare(fs, 2, block);
    Buffer wet(dry);
    CHECK(std::abs(process(master, wet).shaperDb + snap::masterConstants::shaperRangeDb) < 1.0e-5f);
    master.setParams({});
    wet.makeCopyOf(dry);
    process(master, wet);
    CHECK(master.shaperGainDb() == 0 && sameDelayed(wet, dry, delay, 2 * kick));
}

void testClip()
{
    const auto hot = makeSine(997, juce::Decibels::decibelsToGain(6.0f), 2, -1);
    const auto quiet = makeSine(997, juce::Decibels::decibelsToGain(-12.0f), 2, -1);
    const float peakLimit = juce::Decibels::decibelsToGain(snap::masterConstants::clipCeilingDb + 0.05f);
    constexpr int start = 2400, end = 9600;
    for (const bool lowLatency : { false, true })
    {
        Master master;
        master.setParams({ .clipOn = true, .lowLatency = lowLatency });
        master.prepare(fs, 2, block);
        const int delay = master.latency();
        Buffer wet(hot);
        CHECK(process(master, wet).clipped);
        CHECK(wet.getMagnitude(start, end - start) <= peakLimit);

        master.reset();
        wet.makeCopyOf(quiet);
        CHECK(!process(master, wet).clipped);
        double residual = 0;
        for (int i = start; i < end; ++i)
        {
            const double error = wet.getSample(0, i) - quiet.getSample(0, i - delay);
            residual += error * error;
        }
        CHECK(10 * std::log10(2 * residual / energy(quiet, start - delay, end - delay)) < -50);

        master.setParams({ .lowLatency = lowLatency });
        master.reset();
        wet.makeCopyOf(hot);
        CHECK(!process(master, wet).clipped && sameDelayed(wet, hot, delay));
    }
}

void testPostBus()
{
    Master master;
    master.prepare(fs, 2, block);
    const int maskDelay = snap::TransientShaper::lookaheadFor(fs, false);
    const int clipDelay = master.latency() - maskDelay;
    const auto dry = makeNoise(2, 4900), post = makeSine(997, 2.0f, 2, -1, dry.getNumSamples());
    Buffer wet(dry);
    process(master, wet, block, &post);
    bool exact = true;
    for (int c = 0; c < dry.getNumChannels(); ++c)
        for (int i = 0; i < dry.getNumSamples(); ++i)
        {
            const float main = i >= master.latency() ? dry.getSample(c, i - master.latency()) : 0;
            const float bus = i >= clipDelay ? post.getSample(c, i - clipDelay) : 0;
            exact &= wet.getSample(c, i) == main + bus;
        }
    CHECK(exact);

    master.setParams({ .clipOn = true });
    master.reset();
    wet.clear();
    CHECK(process(master, wet, block, &post).clipped);
}

void testOutputAndMeter()
{
    constexpr int samples = static_cast<int>(3.2 * fs);
    const auto input = makeSine(997, 0.1f, 2, 1, samples);
    snap::LoudnessMeter reference;
    reference.prepare(fs, 2);
    reference.process(input, samples);

    Master master;
    master.prepare(fs, 2, block);
    const auto neutral = render(input, {});
    Buffer wet(input);
    master.setParams({ .outDb = 6 });
    process(master, wet);
    const float scale = std::pow(10.0f, 6.0f / 20.0f);
    bool correctGain = true;
    const int settled = static_cast<int>(fs * snap::constants::smoothingSeconds);
    for (int c = 0; c < input.getNumChannels(); ++c)
        for (int i = settled; i < samples; ++i)
            correctGain &= wet.getSample(c, i) == neutral.getSample(c, i) * scale;
    CHECK(correctGain);
    CHECK(std::abs(reference.lufs() - (-20.0f - 0.691f)) < 1);
    CHECK(std::abs(master.preOutputLufs() - reference.lufs()) < 0.5);
    const float withGain = master.preOutputLufs();
    master.setParams({});
    master.reset();
    wet.makeCopyOf(input);
    process(master, wet);
    CHECK(master.preOutputLufs() == withGain);
}

void testLatencyAndPartialBlocks()
{
    for (const double rate : { 44100.0, fs, 96000.0, 192000.0 })
    {
        Master master;
        master.prepare(rate, 1, block);
        for (const bool lowLatency : { false, true, false })
        {
            master.setParams({ .eqOn = false, .air = 0.6f, .attack = 0.7f, .sustain = -0.2f,
                               .clipOn = true, .lowLatency = lowLatency });
            const int latency = Master::latencyFor(rate, lowLatency);
            CHECK(master.latency() == latency);
            CHECK(!lowLatency || latency <= 24);
            master.setParams({ .lowLatency = lowLatency });
            CHECK(master.latency() == latency);
        }
    }

    Master master;
    master.prepare(fs, 2, block);
    const auto dry = makeNoise(2, block);
    Buffer partial(dry);
    master.process(partial, 13, nullptr);
    bool correct = true;
    for (int c = 0; c < dry.getNumChannels(); ++c)
        for (int i = 0; i < block; ++i)
            correct &= partial.getSample(c, i) == (i < 13 ? 0.0f : dry.getSample(c, i));
    CHECK(correct);
}

void testAutomation()
{
    constexpr int activeBlocks = 180, blocks = 220;
    const auto dry = makeSine(173, 0.12f, 2, -0.7f, blocks * block);
    Master master;
    master.prepare(fs, 2, block);
    Buffer wet(dry);
    takeAudioAllocations();
    for (int b = 0; b < blocks; ++b)
    {
        Master::Params p;
        if (b < activeBlocks)
        {
            const float sweep = static_cast<float>(b % 43) / 42.0f;
            p.eqOn = b % 2 != 0, p.clipOn = b % 2 == 0;
            p.lcShape = b % 2, p.hcShape = 1 - b % 2, p.toneShape = b % 4;
            p.cutLo = 20.0f * std::pow(100.0f, sweep), p.cutHi = 20000.0f * std::pow(0.05f, sweep);
            p.tone = 2 * sweep - 1, p.air = 1 - sweep;
            p.attack = sweep - 0.5f, p.sustain = 0.5f - sweep, p.outDb = 12 * sweep - 6;
        }

        Buffer view(wet.getArrayOfWritePointers(), 2, b * block, block);
        setAudioThread(true);
        master.setParams(p);
        master.process(view, block, nullptr);
        setAudioThread(false);
    }
    CHECK(takeAudioAllocations() == 0);

    bool finite = true;
    float inputStep = 0, outputStep = 0;
    for (int c = 0; c < wet.getNumChannels(); ++c)
        for (int i = 1; i < wet.getNumSamples(); ++i)
        {
            const float y = wet.getSample(c, i);
            finite &= std::isfinite(y);
            inputStep = std::max(inputStep, std::abs(dry.getSample(c, i) - dry.getSample(c, i - 1)));
            outputStep = std::max(outputStep, std::abs(y - wet.getSample(c, i - 1)));
        }
    CHECK(finite && outputStep < 3.0f * inputStep);
    const int settled = activeBlocks * block + static_cast<int>(fs * snap::constants::smoothingSeconds) + master.latency();
    CHECK(sameDelayed(wet, dry, master.latency(), settled));
}

void testModeSwitchAndBounds()
{
    for (const double rate : { 44100.0, 192000.0 })
    {
        Master master;
        master.prepare(rate, 2, 4096);
        const auto dry = makeNoise(2, 4096);
        Buffer wet(dry);
        for (const float t : { -1.0f, 1.0f })
            for (int shape = 0; shape < 4; ++shape)
            {
                const Master::Params p { .cutLo = t > 0 ? 2000.0f : 20.0f,
                                         .cutHi = t > 0 ? 1000.0f : 20000.0f, .lcShape = 1, .hcShape = 1,
                                         .tone = t, .toneShape = shape, .air = 1, .attack = t, .sustain = t,
                                         .clipOn = shape % 2 == 0, .outDb = 24, .lowLatency = false };
                wet.makeCopyOf(dry);
                setAudioThread(true);
                master.setParams(p);
                master.reset();
                master.process(wet, wet.getNumSamples(), nullptr);
                setAudioThread(false);
                for (int c = 0; c < wet.getNumChannels(); ++c)
                    CHECK(std::all_of(wet.getReadPointer(c), wet.getReadPointer(c) + wet.getNumSamples(),
                                      [](float x) { return std::isfinite(x) && std::abs(x) < 256; }));
            }

        wet.makeCopyOf(dry);
        for (int i = 0; i < wet.getNumSamples(); i += block)
        {
            const bool lowLatency = i % (2 * block) == 0;
            const float sweep = static_cast<float>(i) / static_cast<float>(wet.getNumSamples());
            Buffer view(wet.getArrayOfWritePointers(), 2, i, block);
            setAudioThread(true);
            master.setParams({ .eqOn = lowLatency, .cutLo = 20 + 1980 * sweep, .cutHi = 1000 + 19000 * sweep,
                               .lcShape = lowLatency ? 1 : 0, .hcShape = lowLatency ? 0 : 1,
                               .tone = 2 * sweep - 1, .toneShape = (i / block) % 4, .air = sweep, .attack = sweep,
                               .sustain = -sweep, .clipOn = lowLatency, .outDb = 24 * sweep, .lowLatency = lowLatency });
            master.process(view, block, nullptr);
            setAudioThread(false);
            CHECK(master.latency() == Master::latencyFor(rate, lowLatency));
        }
        for (int c = 0; c < wet.getNumChannels(); ++c)
            CHECK(std::all_of(wet.getReadPointer(c), wet.getReadPointer(c) + wet.getNumSamples(),
                              [](float x) { return std::isfinite(x); }));
    }
    CHECK(takeAudioAllocations() == 0);
}
} // namespace

void testMaster()
{
    testTransparency();
    testCut();
    testTone();
    testAir();
    testShaper();
    testClip();
    testPostBus();
    testOutputAndMeter();
    testLatencyAndPartialBlocks();
    testAutomation();
    testModeSwitchAndBounds();
}
