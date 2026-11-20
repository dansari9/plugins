#include "Check.h"
#include "dsp/modules/SubHiss.h"
#include "loops/Loops.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace
{
using snap::SubHiss;
using Buffer = juce::AudioBuffer<float>;
constexpr int rate = 48000;
constexpr int blockSize = 257;
constexpr float audible = 1.0e-4f;
constexpr float tolerance = 2.0e-6f;

struct Outputs
{
    Buffer pre, post;
    int latency;
};

SubHiss::Settings activeLayers()
{
    SubHiss::Settings settings;
    settings.sub.amount = settings.hiss.amount = 100;
    return settings;
}

void addHit(Buffer& input, int onset, double sampleRate, bool kick, bool snare)
{
    const int end = std::min(input.getNumSamples(), onset + (int) (0.10 * sampleRate));
    for (int i = onset; i < end; ++i)
    {
        const double t = (i - onset) / sampleRate;
        const double ramp = std::sin(juce::MathConstants<double>::halfPi * std::min(1.0, t / 0.0015));
        const double phase = juce::MathConstants<double>::twoPi * t;
        // Give the kick body and snare crack distinct bands so bleed cannot supply the test trigger.
        const double low = kick ? 0.78 * std::sin(phase * 78.0) * std::exp(-t / 0.020) : 0.0;
        const double high = snare ? (0.36 * std::sin(phase * 3300.0) + 0.32 * std::sin(phase * 4700.0)
                                    + 0.18 * std::sin(phase * 5900.0)) * std::exp(-t / 0.012) : 0.0;
        for (int c = 0; c < input.getNumChannels(); ++c)
            input.addSample(c, i, (float) (ramp * ramp * (low + high)));
    }
}

Buffer isolatedHit(bool kick, bool snare)
{
    Buffer input(2, rate / 5);
    input.clear();
    addHit(input, rate / 100, rate, kick, snare);
    return input;
}

void process(SubHiss& module, Buffer& pre, Buffer& post, const SubHiss::Settings& settings,
             int begin, int end, int block, bool probe = false)
{
    for (int start = begin; start < end; start += block)
    {
        const int size = std::min(block, end - start);
        Buffer ioView(pre.getArrayOfWritePointers(), pre.getNumChannels(), start, size);
        Buffer postView(post.getArrayOfWritePointers(), post.getNumChannels(), start, size);
        for (int c = 0; c < postView.getNumChannels(); ++c)
            std::fill_n(postView.getWritePointer(c), size, 0.375f);
        setAudioThread(probe);
        module.process(ioView, postView, settings);
        setAudioThread(false);
    }
}

Outputs run(const Buffer& input, const SubHiss::Settings& settings, int fs = rate,
            int block = blockSize, bool lowLatency = false, int padding = 0)
{
    SubHiss module;
    module.prepare({ (double) fs, (juce::uint32) block, (juce::uint32) input.getNumChannels() }, settings, lowLatency);
    const int latency = module.getLatencySamples();
    const int length = input.getNumSamples() + padding + latency;
    Outputs out { Buffer(input.getNumChannels(), length), Buffer(input.getNumChannels(), length), latency };
    out.pre.clear();
    for (int c = 0; c < input.getNumChannels(); ++c)
        out.pre.copyFrom(c, 0, input, c, 0, input.getNumSamples());
    process(module, out.pre, out.post, settings, 0, length, block);
    return out;
}

Buffer sum(const Buffer& a, const Buffer& b)
{
    Buffer output(a);
    for (int c = 0; c < a.getNumChannels(); ++c)
        output.addFrom(c, 0, b, c, 0, a.getNumSamples());
    return output;
}

float peak(const Buffer& buffer, int start, int count)
{
    return buffer.getMagnitude(start, count);
}

float peak(const Buffer& buffer)
{
    return peak(buffer, 0, buffer.getNumSamples());
}

float difference(const Buffer& a, const Buffer& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return std::numeric_limits<float>::infinity();

    float error = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const float delta = std::abs(a.getSample(c, i) - b.getSample(c, i));
            if (!std::isfinite(delta))
                return std::numeric_limits<float>::infinity();
            error = std::max(error, delta);
        }
    return error;
}

bool isSilent(const Buffer& buffer, int count)
{
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < count; ++i)
            if (std::fpclassify(buffer.getSample(c, i)) != FP_ZERO)
                return false;
    return true;
}

bool isSilent(const Buffer& buffer)
{
    return isSilent(buffer, buffer.getNumSamples());
}

bool isFinite(const Buffer& buffer)
{
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (!std::isfinite(buffer.getSample(c, i)))
                return false;
    return true;
}

void checkDelayedDry(const Buffer& input, const Outputs& output, int delay)
{
    CHECK(output.latency == delay);
    CHECK(isSilent(output.post));
    if (output.latency != delay)
        return;

    CHECK(isSilent(output.pre, delay));
    for (int c = 0; c < input.getNumChannels(); ++c)
        CHECK(std::memcmp(input.getReadPointer(c), output.pre.getReadPointer(c, delay),
                          (size_t) input.getNumSamples() * sizeof(float)) == 0);
}

void testDry()
{
    Buffer input(2, 777);
    for (int c = 0; c < input.getNumChannels(); ++c)
        for (int i = 0; i < input.getNumSamples(); ++i)
            input.setSample(c, i, 0.6f * loops::noise((juce::uint32) (i + 137 * c)));

    for (const bool lowLatency : { false, true })
    {
        const int delay = lowLatency ? snap::constants::lowLatencyMaskLookahead : 144;
        auto settings = activeLayers();
        settings.enabled = false;
        settings.sub.solo = settings.hiss.solo = true;
        settings.hiss.post = true;
        checkDelayedDry(input, run(input, settings, rate, blockSize, lowLatency), delay);

        settings.enabled = true;
        settings.sub.amount = settings.hiss.amount = 0;
        checkDelayedDry(input, run(input, settings, rate, blockSize, lowLatency), delay);
    }
}

void testSensitivity()
{
    const auto hit = isolatedHit(true, true);
    auto settings = activeLayers();
    settings.sub.post = settings.hiss.post = true;
    CHECK(peak(run(hit, settings).post) > audible);
    settings.sub.sensitivity = settings.hiss.sensitivity = 0;
    checkDelayedDry(hit, run(hit, settings), 144);

    auto split = settings;
    split.sub.solo = split.hiss.solo = true;
    split.sub.post = false;
    split.hiss.sensitivity = 50;
    const auto hissOnly = run(hit, split);
    CHECK(isSilent(hissOnly.pre) && peak(hissOnly.post) > audible);
    split.sub.sensitivity = 50;
    split.hiss.sensitivity = 0;
    const auto subOnly = run(hit, split);
    CHECK(peak(subOnly.pre) > audible && isSilent(subOnly.post));

    // Disable sensitivity after a short input burst, before its lookahead-aligned wet output.
    settings.sub.sensitivity = settings.hiss.sensitivity = 100;
    constexpr int onset = 480, disableAt = onset + 112, restoreAt = onset + 2400;
    Buffer burst(2, hit.getNumSamples());
    burst.clear();
    addHit(burst, onset, rate, true, true);
    burst.clear(disableAt, burst.getNumSamples() - disableAt);
    const auto reference = run(burst, settings);
    CHECK(isSilent(reference.post, disableAt));
    CHECK(peak(reference.post, disableAt, restoreAt - disableAt) > audible);

    SubHiss module;
    module.prepare({ rate, blockSize, 2 }, settings);
    Buffer pre(burst), post(2, burst.getNumSamples());
    process(module, pre, post, settings, 0, disableAt, blockSize);
    settings.sub.sensitivity = settings.hiss.sensitivity = 0;
    process(module, pre, post, settings, disableAt, restoreAt, blockSize);
    settings.sub.sensitivity = settings.hiss.sensitivity = 100;
    process(module, pre, post, settings, restoreAt, burst.getNumSamples(), blockSize);
    CHECK(isSilent(post));
}

void testDetectorSplit()
{
    const auto kick = isolatedHit(true, false);
    const auto snare = isolatedHit(false, true);
    auto settings = activeLayers();
    settings.sub.post = true;
    settings.hiss.amount = 0;
    const auto sub = run(kick, settings);
    CHECK(peak(sub.post) > audible);
    CHECK(isSilent(run(snare, settings).post));

    settings.sub.amount = 0;
    settings.hiss.amount = 100;
    settings.hiss.post = true;
    const auto hiss = run(snare, settings);
    CHECK(peak(hiss.post) > audible);
    CHECK(isSilent(run(kick, settings).post));

    for (const auto* out : { &sub, &hiss })
    {
        const int alignedOnset = rate / 100 + out->latency;
        CHECK(peak(out->post, 0, alignedOnset - rate / 1000) < audible);
        CHECK(peak(out->post, alignedOnset - rate / 1000, rate / 100) > audible);
    }
}

void testRouting()
{
    const auto input = isolatedHit(true, true);
    for (const bool lowLatency : { false, true })
    {
        const auto render = [&](const SubHiss::Settings& settings)
        {
            return run(input, settings, rate, blockSize, lowLatency);
        };
        const auto dry = render({});
        auto settings = activeLayers();
        const auto bothPre = render(settings);
        CHECK(isSilent(bothPre.post));

        settings.sub.post = settings.hiss.post = true;
        const auto bothPost = render(settings);
        CHECK(difference(bothPost.pre, dry.pre) <= 0.0f);
        CHECK(difference(sum(bothPost.pre, bothPost.post), bothPre.pre) < tolerance);

        settings.sub.post = false;
        const auto split = render(settings);
        CHECK(difference(sum(split.pre, split.post), bothPre.pre) < tolerance);

        settings.sub.solo = true;
        const auto subPre = render(settings);
        CHECK(peak(subPre.pre) > audible && isSilent(subPre.post));
        settings.sub.post = true;
        const auto subPost = render(settings);
        CHECK(isSilent(subPost.pre));
        CHECK(difference(subPost.post, subPre.pre) < tolerance);

        settings.sub.solo = false;
        settings.hiss.solo = true;
        const auto hissPost = render(settings);
        CHECK(isSilent(hissPost.pre) && peak(hissPost.post) > audible);
        settings.hiss.post = false;
        const auto hissPre = render(settings);
        CHECK(isSilent(hissPre.post));
        CHECK(difference(hissPre.pre, hissPost.post) < tolerance);

        settings.hiss.post = true;
        settings.sub.solo = true;
        settings.sub.post = false;
        const auto dualSolo = render(settings);
        CHECK(difference(dualSolo.pre, subPre.pre) < tolerance);
        CHECK(difference(dualSolo.post, hissPost.post) < tolerance);
        CHECK(difference(split.pre, sum(dry.pre, dualSolo.pre)) < tolerance);
        CHECK(difference(split.post, dualSolo.post) < tolerance);
        CHECK(difference(bothPost.post, sum(subPost.post, hissPost.post)) < tolerance);

        // A zero-amount solo leaves the other audible layer and the delayed dry intact.
        settings.sub.amount = 0;
        settings.hiss.solo = false;
        const auto ignoredSolo = render(settings);
        CHECK(difference(ignoredSolo.pre, dry.pre) <= 0.0f);
        CHECK(difference(ignoredSolo.post, hissPost.post) < tolerance);
    }
}

void testNoAllocationOnHits()
{
    constexpr int first = 480, changeAt = 9600, second = 14400;
    Buffer pre(2, 22000), post(2, pre.getNumSamples());
    pre.clear();
    addHit(pre, first, rate, true, true);
    addHit(pre, second, rate, true, true);
    auto settings = activeLayers();
    settings.sub.solo = settings.hiss.solo = true;
    settings.hiss.post = true;
    settings.sub.decayMs = 400;
    settings.hiss.decayMs = 340;
    SubHiss module;
    module.prepare({ rate, blockSize, 2 }, settings);

    takeAudioAllocations();
    process(module, pre, post, settings, 0, changeAt, blockSize, true);
    settings.sub.post = true;
    settings.hiss.post = false;
    settings.sub.pitch = 38;
    settings.sub.stereo = 70;
    settings.sub.acid = true;
    settings.sub.detectHiHz = 180;
    settings.hiss.type = 11;
    settings.hiss.pitch = 5;
    settings.hiss.tone = 30;
    settings.hiss.detectLoHz = 2000;
    process(module, pre, post, settings, changeAt, pre.getNumSamples(), blockSize, true);
    CHECK(takeAudioAllocations() == 0);

    for (const auto* output : { &pre, &post })
    {
        CHECK(peak(*output, first, rate / 30) > audible);
        const float tail = peak(*output, second - rate / 50, rate / 50);
        const float retrigger = peak(*output, second, rate / 30);
        CHECK(tail > tolerance);
        CHECK(retrigger > std::max(audible, 8.0f * tail));
        CHECK(isFinite(*output));
    }
}

void testLivePostAndRetrigger()
{
    constexpr int first = 480, routeChange = 3600, second = 9600, length = 16000;
    Buffer input(2, length);
    input.clear();
    addHit(input, first, rate, true, false);
    auto settings = activeLayers();
    settings.hiss.amount = 0;
    settings.sub.solo = true;
    settings.sub.color = 0;
    settings.sub.decayMs = 2000;
    const auto singleHit = run(input, settings);
    addHit(input, second, rate, true, false);
    auto bothHits = run(input, settings, rate, 4096);
    bothHits.pre.setSize(2, length, true);

    SubHiss module;
    module.prepare({ rate, blockSize, 2 }, settings);
    const int processedLength = bothHits.pre.getNumSamples();
    Buffer pre(input), post(2, processedLength);
    pre.setSize(2, processedLength, true, true);
    takeAudioAllocations();
    process(module, pre, post, settings, 0, routeChange, blockSize, true);
    settings.sub.post = true;
    process(module, pre, post, settings, routeChange, processedLength, blockSize, true);
    CHECK(takeAudioAllocations() == 0);

    const auto routed = sum(pre, post);
    const int alignedSecond = second + bothHits.latency;
    CHECK(peak(post, 0, routeChange) <= 0.0f);
    const int settled = routeChange + rate / 50 + 1;
    CHECK(peak(pre, settled, processedLength - settled) <= 0.0f);
    CHECK(difference(routed, bothHits.pre) < tolerance);
    CHECK(peak(singleHit.pre, alignedSecond, rate / 100) > 0.01f);
    float changeFromSingle = 0.0f, maximumStep = 0.0f;
    for (int i = alignedSecond; i < alignedSecond + rate / 100; ++i)
    {
        const float sample = routed.getSample(0, i);
        changeFromSingle = std::max(changeFromSingle, std::abs(sample - singleHit.pre.getSample(0, i)));
        maximumStep = std::max(maximumStep, std::abs(sample - routed.getSample(0, i - 1)));
    }
    CHECK(changeFromSingle > 0.1f);
    CHECK(maximumStep < 0.025f);
}

void testDuck()
{
    const auto hit = isolatedHit(true, true);
    auto settings = activeLayers();
    settings.sub.post = true;
    settings.sub.solo = settings.hiss.solo = true;
    const auto open = run(hit, settings);
    settings.sub.duck = settings.hiss.duck = 100;
    const auto ducked = run(hit, settings);
    const int start = rate / 100 + open.latency;
    CHECK(peak(ducked.post, start, rate / 25) < 0.3f * peak(open.post, start, rate / 25));
    CHECK(peak(ducked.pre, start, rate / 25) < 0.3f * peak(open.pre, start, rate / 25));
}

void testProcessingFormats()
{
    struct Format { int sampleRate, channels, block; bool lowLatency; };
    const Format formats[] = {
        { 44100, 1, 1, false }, { 48000, 2, 64, true }, { 96000, 1, 509, true },
        { 192000, 2, 1024, false }, { 384000, 2, 4096, false }
    };
    auto settings = activeLayers();
    settings.sub.solo = settings.hiss.solo = true;
    settings.sub.stereo = settings.sub.color = 100;
    settings.sub.acid = true;
    settings.hiss.post = true;
    settings.hiss.pitch = 12;
    settings.hiss.tone = settings.hiss.env = settings.hiss.res = 100;
    settings.hiss.type = 17;
    for (const auto& format : formats)
    {
        Buffer input(format.channels, format.sampleRate / 10);
        input.clear();
        addHit(input, format.sampleRate / 100, format.sampleRate, true, true);
        const auto output = run(input, settings, format.sampleRate, format.block, format.lowLatency);
        CHECK(output.latency == (format.lowLatency ? snap::constants::lowLatencyMaskLookahead : (int) std::lround(0.003 * format.sampleRate)));
        CHECK(isFinite(output.pre) && peak(output.pre) > audible);
        CHECK(isFinite(output.post) && peak(output.post) > audible);
    }
}

void testHissTypesAndReset()
{
    const auto input = isolatedHit(false, true);
    auto settings = activeLayers();
    settings.sub.amount = 0;
    settings.hiss.solo = true;
    settings.hiss.post = true;
    std::vector<Buffer> types;
    types.reserve(18);
    for (int type = 0; type < 18; ++type)
    {
        settings.hiss.type = type;
        auto output = run(input, settings);
        CHECK(isSilent(output.pre));
        CHECK(isFinite(output.post) && peak(output.post) > audible);
        for (const auto& other : types)
            CHECK(difference(output.post, other) > audible);
        types.push_back(std::move(output.post));
    }

    const auto combinedHit = isolatedHit(true, true);
    settings.sub.amount = 100;
    settings.sub.solo = true;
    SubHiss module;
    module.prepare({ rate, blockSize, 2 }, settings);
    Buffer firstPre(combinedHit), firstPost(2, combinedHit.getNumSamples());
    process(module, firstPre, firstPost, settings, 0, combinedHit.getNumSamples(), blockSize);
    module.reset();
    Buffer nextPre(combinedHit), nextPost(2, combinedHit.getNumSamples());
    process(module, nextPre, nextPost, settings, 0, combinedHit.getNumSamples(), blockSize);
    CHECK(peak(firstPre) > audible && peak(firstPost) > audible);
    CHECK(difference(firstPre, nextPre) <= 0.0f);
    CHECK(difference(firstPost, nextPost) <= 0.0f);
}

void testSubStereo(float pitch)
{
    constexpr int onset = rate / 100, fftOrder = 13, fftSize = 1 << fftOrder;
    Buffer input(2, rate / 2);
    input.clear();
    addHit(input, onset, rate, true, false);
    auto settings = activeLayers();
    settings.hiss.amount = 0;
    settings.sub.post = true;
    settings.sub.pitch = pitch;
    settings.sub.stereo = settings.sub.color = 100;
    settings.sub.decayMs = 800;
    const auto output = run(input, settings);

    std::array<float, 2 * fftSize> mid {}, side {};
    const int start = onset + output.latency + rate / 10;
    for (int i = 0; i < fftSize; ++i)
    {
        const float window = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        const float left = output.post.getSample(0, start + i);
        const float right = output.post.getSample(1, start + i);
        mid[(size_t) i] = window * (left + right) * 0.5f;
        side[(size_t) i] = window * (left - right) * 0.5f;
    }
    const juce::dsp::FFT fft(fftOrder);
    fft.performFrequencyOnlyForwardTransform(mid.data(), true);
    fft.performFrequencyOnlyForwardTransform(side.data(), true);
    const auto energy = [](const auto& spectrum, double lo, double hi)
    {
        double total = 0;
        for (int bin = (int) std::ceil(lo * fftSize / rate); bin * (double) rate / fftSize < hi; ++bin)
            total += (double) spectrum[(size_t) bin] * spectrum[(size_t) bin];
        return total;
    };
    const double lowMid = energy(mid, 25, 120), lowSide = energy(side, 25, 120);
    const double highSide = energy(side, 120, 1000);
    CHECK(highSide > 1.0e-3);
    CHECK(lowSide < lowMid * 0.01);
    CHECK(lowSide < highSide * 0.25);
}

bool writeWav(const juce::File& file, const Buffer& buffer, int start, int count, float gain)
{
    auto output = buffer;
    output.applyGain(gain);
    if (file.existsAsFile() && !file.deleteFile())
        return false;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(rate)
        .withNumChannels(2).withBitsPerSample(24);
    const auto writer = juce::WavAudioFormat().createWriterFor(stream, options);
    return writer != nullptr && writer->writeFromAudioSampleBuffer(output, start, count);
}
} // namespace

void testSubHiss()
{
    testDry();
    testSensitivity();
    testDetectorSplit();
    testRouting();
    testNoAllocationOnHits();
    testLivePostAndRetrigger();
    testDuck();
    testProcessingFormats();
    testHissTypesAndReset();
    testSubStereo(31); // The second harmonic stays below the stereo cutoff.
    testSubStereo(36); // Both synthesized harmonics sit above the cutoff.
}

int renderSubHiss(const juce::File& directory)
{
    if (directory.createDirectory().failed())
        return 1;

    constexpr int renderBlock = 512;
    constexpr int tail = rate;
    for (int which = 0; which < (int) std::size(loops::names); ++which)
    {
        auto input = loops::make(which, rate, 6);
        input.applyGain(0.5f);
        auto settings = SubHiss::Settings {};
        auto dry = run(input, settings, rate, renderBlock, false, tail);
        settings.sub.amount = 45;
        settings.hiss.amount = 40;
        auto both = run(input, settings, rate, renderBlock, false, tail);
        settings.sub.solo = true;
        auto sub = run(input, settings, rate, renderBlock, false, tail);
        settings.sub.solo = false;
        settings.hiss.solo = true;
        auto hiss = run(input, settings, rate, renderBlock, false, tail);
        settings.hiss.solo = false;
        settings.hiss.post = true;
        settings.sub.acid = true;
        settings.sub.stereo = 65;
        settings.hiss.type = 8;
        settings.hiss.tone = -25;
        auto alternative = run(input, settings, rate, renderBlock, false, tail);
        auto postSum = sum(alternative.pre, alternative.post);

        struct Render { const char* name; const Buffer& audio; int latency; };
        const Render renders[] = {
            { "dry", dry.pre, dry.latency }, { "both-pre", both.pre, both.latency },
            { "sub-solo", sub.pre, sub.latency }, { "hiss-solo", hiss.pre, hiss.latency },
            { "alternate-post-sum", postSum, alternative.latency }
        };
        float maxPeak = 0;
        for (const auto& render : renders)
            maxPeak = std::max(maxPeak, peak(render.audio));
        const float gain = maxPeak > 0.8f ? 0.8f / maxPeak : 1.0f;
        for (const auto& render : renders)
        {
            const auto name = juce::String(loops::names[which]) + "-subhiss-" + render.name + ".wav";
            if (!writeWav(directory.getChildFile(name), render.audio, render.latency, input.getNumSamples() + tail, gain))
                return 1;
        }
    }
    return 0;
}
