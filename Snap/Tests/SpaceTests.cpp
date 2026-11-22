#include "Check.h"
#include "Parameters.h"
#include "SpaceIRData.h"
#include "dsp/modules/Space.h"
#include "loops/Loops.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <chrono>
#include <cstring>

namespace
{
using snap::Space;
using namespace snap::spaceConstants;
using Buffer = juce::AudioBuffer<float>;
constexpr int rate = 48000;

Space::Settings fullyWet()
{
    Space::Settings s;
    s.amount = 100;
    s.focusLoHz = 20;
    s.focusHiHz = 20000;
    return s;
}

void process(Space& space, Buffer& buffer, const Space::Settings& s, int blockSize, bool probe = false)
{
    for (int i = 0; i < buffer.getNumSamples(); i += blockSize)
    {
        Buffer block(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), i,
                     std::min(blockSize, buffer.getNumSamples() - i));
        setAudioThread(probe);
        space.process(block, s);
        setAudioThread(false);
    }
}

Buffer impulse(Space::Settings s, int length, int channels = 2, int fs = rate, int block = 509)
{
    Space space;
    space.prepare({ (double) fs, (juce::uint32) block, (juce::uint32) channels }, s);
    Buffer out(channels, length);
    out.clear();
    for (int c = 0; c < channels; ++c)
        out.setSample(c, 0, 1.0f);
    process(space, out, s, block, true);
    for (int c = 0; c < channels; ++c)
        out.addSample(c, 0, -1.0f);
    return out;
}

double peakDifference(const Buffer& a, const Buffer& b)
{
    double peak = 0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            peak = std::max(peak, (double) std::abs(a.getSample(c, i) - b.getSample(c, i)));
    return peak;
}

void testDry()
{
    Space space;
    for (const auto fs : { 44100, 48000, 96000, 192000, 384000 })
        for (const auto channels : { 1, 2 })
        {
            auto s = fullyWet();
            s.enabled = false;
            space.prepare({ (double) fs, 4096, (juce::uint32) channels }, s);
            Buffer dry(channels, 8192);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < dry.getNumSamples(); ++i)
                    dry.setSample(c, i, loops::noise((juce::uint32) (i + c * 19)) * 0.5f);
            dry.setSample(0, 0, -0.0f);
            auto output = dry;
            process(space, output, s, 4096, true);
            s.amount = 0;
            s.enabled = true;
            s.type = 3;
            process(space, output, s, 1, true);
            for (int c = 0; c < channels; ++c)
                CHECK(std::memcmp(output.getReadPointer(c), dry.getReadPointer(c),
                                  (size_t) dry.getNumSamples() * sizeof(float)) == 0);
        }
    CHECK(Space::getLatencySamples() == 0);
}

void testResponses()
{
    const auto choices = snap::spec("space_type").choices;
    CHECK(choices.size() == responses.size());
    for (size_t index = 0; index < responses.size(); ++index)
    {
        const auto& type = responses[index];
        const double trim = std::pow(10.0, levelTrimDb[index] / 20.0); // each response's level trim (0.5)
        if (index < choices.size())
            CHECK(type.name == choices[index]);
        auto s = fullyWet();
        s.type = (int) index;
        const auto wet = impulse(s, rate * 4);
        int size = 0;
        const auto key = std::string(type.name) + "_wav";
        const auto* data = SpaceIRData::getNamedResource(key.c_str(), size);
        CHECK(data != nullptr);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(
            wav.createReaderFor(new juce::MemoryInputStream(data, (size_t) size, false), true));
        CHECK(reader != nullptr && reader->numChannels == 2 && std::abs(reader->sampleRate - rate) < 0.01);
        if (reader == nullptr)
            continue;
        const int frames = (int) reader->lengthInSamples;
        Buffer source(2, frames);
        CHECK(reader->read(&source, 0, frames, 0, true, true));

        const int length = (int) std::ceil(std::min((double) frames,
            (type.decayLeadInSeconds + type.rt60Seconds * irFloorDb / -60.0) * rate));
        const int fade = (int) std::lround(endFadeSeconds * rate);
        double error = 0, energy = 0, stereoDifference = 0;
        for (int i = 0; i < wet.getNumSamples(); ++i)
        {
            const float envelope = i >= length ? 0.0f
                : (float) (std::exp(std::log(0.001) * std::max(0.0, (double) i / rate - type.decayLeadInSeconds) / type.rt60Seconds)
                * std::min(1.0, (double) (length - 1 - i) / fade));
            for (int c = 0; c < 2; ++c)
            {
                const float actual = wet.getSample(c, i);
                const float expected = i < length ? (float) (trim * source.getSample(c, i) * envelope) : 0.0f;
                error = std::max(error, (double) std::abs(actual - expected) / trim);
                energy += (double) actual * actual / (trim * trim);
            }
            stereoDifference += std::abs(wet.getSample(0, i) - wet.getSample(1, i));
        }
        CHECK(error < 2.0e-6);
        CHECK(energy > 0.16 && energy < 0.6);
        CHECK(stereoDifference > 1.0);
    }
}

void testControls()
{
    constexpr int length = 32768;
    auto s = fullyWet();
    const auto wet = impulse(s, length, 2, rate, 1);
    CHECK(peakDifference(wet, impulse(s, length, 2, rate, 4096)) < 1.0e-7);
    s.width = -100;
    const auto swapped = impulse(s, length);
    s.width = 100;
    s.pan = 100;
    const auto panned = impulse(s, length);
    s.pan = 0;
    const auto mono = impulse(s, length, 1);
    s.predelayMs = 500;
    const auto delayed = impulse(s, length);
    s.predelayMs = 0;
    s.duck = 100;
    const auto ducked = impulse(s, length);
    s.duck = 0;
    s.focusLoHz = 250;
    s.focusHiHz = 1800;
    const auto focused = impulse(s, length);
    double wetEnergy = 0, duckEnergy = 0, focusedEnergy = 0;
    float error = 0;
    for (int i = 0; i < length; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            const float v = wet.getSample(c, i);
            error = std::max(error, std::abs(v - swapped.getSample(1 - c, i)));
            error = std::max(error, std::abs(delayed.getSample(c, i) - (i >= rate / 2 ? wet.getSample(c, i - rate / 2) : 0.0f)));
            wetEnergy += (double) v * v;
            duckEnergy += std::pow(ducked.getSample(c, i), 2);
            focusedEnergy += std::pow(focused.getSample(c, i), 2);
        }
        error = std::max(error, std::abs(mono.getSample(0, i) - 0.5f * (wet.getSample(0, i) + wet.getSample(1, i))));
        error = std::max(error, std::abs(wet.getSample(1, i) - panned.getSample(1, i)));
    }
    CHECK(error < 2.0e-6f);
    CHECK(panned.getMagnitude(0, 0, length) < 1.0e-7f);
    CHECK(duckEnergy < wetEnergy * 0.90);
    CHECK(focusedEnergy < wetEnergy * 0.55);

    s = fullyWet();
    s.type = 5;
    s.decay = 0;
    const auto shortTail = impulse(s, rate);
    s.decay = 100;
    const auto longTail = impulse(s, rate);
    CHECK(shortTail.getRMSLevel(0, rate / 2, rate / 2) < 1.0e-7f * (float) std::pow(10.0, levelTrimDb[5] / 20.0));
    CHECK(longTail.getRMSLevel(0, rate / 2, rate / 2) > 3.0e-5f);

    s = fullyWet();
    s.stretch = -100;
    const auto compact = impulse(s, length);
    s.stretch = 100;
    const auto extended = impulse(s, length);
    CHECK(compact.getMagnitude(0, rate / 4, rate / 4) < 1.0e-7f);
    CHECK(extended.getMagnitude(0, rate / 4, rate / 4) > 1.0e-5f);

    // Exercise convolution at both operating-rate extremes, including 2x oversampling.
    CHECK(impulse(fullyWet(), rate / 4, 1, 44100).getMagnitude(0, rate / 4) > 0.01f);
    CHECK(impulse(fullyWet(), rate / 4, 2, 384000, 4096).getMagnitude(0, rate / 4) > 0.001f);
}

void testRebuild()
{
    auto initial = fullyWet();
    auto changed = initial;
    changed.type = 1;
    changed.decay = 72;
    changed.stretch = -34;
    Space space;
    space.prepare({ rate, 128, 2 }, initial);
    Buffer block(2, 128);
    const auto fill = [&]
    {
        for (int c = 0; c < 2; ++c)
            std::fill_n(block.getWritePointer(c), block.getNumSamples(), 0.375f);
    };
    const auto run = [&](const Space::Settings& s)
    {
        fill();
        setAudioThread(true);
        space.process(block, s);
        setAudioThread(false);
    };

    for (int i = 0; i < 400; ++i)
        run(initial);
    const float before = block.getSample(0, 127);
    float previous = before, maximumStep = 0.0f;
    for (int i = 0; i < 440; ++i)
    {
        // Supersede two quick requests while processing continues.
        auto request = changed;
        if (i < 6)
            request.type = 4 + i % 2;
        run(request);
        for (auto value : std::span(block.getReadPointer(0), 128))
        {
            maximumStep = std::max(maximumStep, std::abs(value - previous));
            previous = value;
        }
        if (i < 200)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(maximumStep < 0.002f * (float) std::pow(10.0, levelTrimDb[4] / 20.0)); // the louder of the two rooms (0.5 trims)
    CHECK(std::abs(previous - before) > 0.0005f);

    Space reference;
    reference.prepare({ rate, 128, 2 }, changed);
    for (int i = 0; i < 400; ++i)
    {
        fill();
        reference.process(block, changed);
    }
    CHECK(std::abs(previous - block.getSample(0, 127)) < 2.0e-6f);

    setAudioThread(true);
    space.reset();
    setAudioThread(false);
    block.clear();
    process(space, block, changed, 128, true);
    CHECK(block.getMagnitude(0, 128) < 1.0e-7f);
    CHECK(takeAudioAllocations() == 0);
}
} // namespace

void testSpace()
{
    testDry();
    testResponses();
    testControls();
    testRebuild();
}

int renderSpace(const juce::File& directory)
{
    if (directory.createDirectory().failed())
        return 1;

    for (int i = 0; i < 4; ++i)
    {
        auto dry = loops::make(i, rate, 9);
        dry.clear(rate * 6, rate * 3);
        dry.applyGain(0.5f);
        for (const int type : { -1, 0, 13, 17 })
        {
            auto output = dry;
            auto s = Space::Settings {};
            s.type = std::max(0, type);
            s.amount = type < 0 ? 0.0f : 45.0f;
            Space space;
            space.prepare({ rate, 512, 2 }, s);
            process(space, output, s, 512);
            const auto name = juce::String(loops::names[i]) + "-"
                + (type < 0 ? "dry" : juce::String(responses[(size_t) type].name.data()));
            auto file = directory.getChildFile(name + ".wav");
            if (file.existsAsFile() && !file.deleteFile())
                return 1;
            std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
            const auto options = juce::AudioFormatWriterOptions {}
                .withSampleRate(rate).withNumChannels(2).withBitsPerSample(24);
            const auto writer = juce::WavAudioFormat().createWriterFor(stream, options);
            if (!writer || !writer->writeFromAudioSampleBuffer(output, 0, output.getNumSamples()))
                return 1;
        }
    }
    return 0;
}
