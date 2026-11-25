#include "Vibrate.h"
#include <cstring>
#include <iostream>
#include <random>

// Real processor, interleaved stereo f32, 128 samples per block as on the site.
static int renderFile(const juce::StringArray& args)
{
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(args[1]);
    juce::MemoryBlock input;
    if (!file.loadFileAsData(input) || input.getSize() == 0 || input.getSize() % (2 * sizeof(float)) != 0)
        return 1;
    juce::MemoryInputStream in(input, false);
    juce::MemoryOutputStream out;
    VibrateProcessor processor;
    double rate = 48000.0;
    for (int i = 3; i < args.size(); ++i)
    {
        const auto id = args[i].upToFirstOccurrenceOf("=", false, false);
        const auto value = args[i].fromFirstOccurrenceOf("=", false, false);
        if (id == "rate_hz")
            rate = value.getDoubleValue();
        else if (id == "preset")
            processor.setCurrentProgram(value.getIntValue());
        else if (auto* param = processor.apvts.getParameter(id))
            param->setValueNotifyingHost(param->convertTo0to1(value.getFloatValue()));
        else
        {
            std::cerr << "Unknown render option " << id << '\n';
            return 1;
        }
    }
    if (!std::isfinite(rate) || rate < 44100.0 || rate > 192000.0)
        return 1;
    processor.prepareToPlay(rate, 128);
    juce::AudioBuffer<float> buffer(2, 128);
    juce::MidiBuffer midi;
    int remaining = static_cast<int>(input.getSize() / (2 * sizeof(float)));
    while (remaining > 0)
    {
        const int count = std::min(128, remaining);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, count);
        for (int i = 0; i < count; ++i)
            for (int channel = 0; channel < 2; ++channel)
                block.setSample(channel, i, in.readFloat());
        processor.processBlock(block, midi);
        for (int i = 0; i < count; ++i)
            for (int channel = 0; channel < 2; ++channel)
                out.writeFloat(block.getSample(channel, i));
        remaining -= count;
    }
    return juce::File::getCurrentWorkingDirectory().getChildFile(args[2])
        .replaceWithData(out.getData(), out.getDataSize()) ? 0 : 1;
}

namespace
{
using vibrate::Controls;
using vibrate::Engine;
using vibrate::Stereo;

constexpr std::array<double, 5> sampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
constexpr Controls full { 1, 1, 1, 1, 1, 1 };
int failures = 0;

void check(bool pass, const char* name)
{
    std::cout << (pass ? "PASS " : "FAIL ") << name << '\n';
    failures += pass ? 0 : 1;
}

bool same(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
bool same(Stereo a, Stereo b) { return same(a.left, b.left) && same(a.right, b.right); }

void set(VibrateProcessor& processor, std::size_t index, double normal)
{
    auto* param = processor.apvts.getParameter(vibrate::ids[index]);
    param->setValueNotifyingHost(static_cast<float>(normal));
}

std::vector<Stereo> noise(int count)
{
    std::mt19937 rng(0x91de7u);
    auto sample = [&] { return static_cast<float>(static_cast<double>(rng()) * (2.0 / 4294967296.0) - 1.0); };
    std::vector<Stereo> audio(static_cast<std::size_t>(count));
    for (auto& x : audio) x = { sample(), sample() };
    audio[0] = { -0.0f, -0.0f };
    audio[1] = { 0.0f, -0.0f };
    return audio;
}

std::vector<Stereo> runProcessor(const std::vector<Stereo>& input, const Controls& controls,
                                 double rate, int channels, int blockSize, bool reset = false)
{
    VibrateProcessor p;
    p.setChannelLayoutOfBus(true, 0, channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
    for (std::size_t i = 0; i < controls.size(); ++i) set(p, i, controls[i]);
    p.prepareToPlay(rate, 4096);
    juce::AudioBuffer<float> storage(2, 4096);
    juce::MidiBuffer midi;
    std::vector<Stereo> output;
    output.reserve(input.size());
    std::mt19937 rng(325u);
    auto render = [&] {
        output.clear();
        for (std::size_t offset = 0; offset < input.size();)
        {
            const int size = blockSize == 0 ? 1 + static_cast<int>(rng() % 4096u) : blockSize;
            const auto count = std::min(static_cast<std::size_t>(size), input.size() - offset);
            juce::AudioBuffer<float> block(storage.getArrayOfWritePointers(), 2, static_cast<int>(count));
            for (std::size_t i = 0; i < count; ++i)
            {
                block.setSample(0, static_cast<int>(i), input[offset + i].left);
                block.setSample(1, static_cast<int>(i), input[offset + i].right);
            }
            p.processBlock(block, midi);
            for (std::size_t i = 0; i < count; ++i)
                output.push_back({ block.getSample(0, static_cast<int>(i)), block.getSample(1, static_cast<int>(i)) });
            offset += count;
        }
    };
    render();
    if (reset)
    {
        p.reset();
        render();
    }
    return output;
}

void testNull()
{
    const auto in = noise(12517);
    bool ok = true;
    for (double rate : sampleRates)
        for (int channels : { 1, 2 })
            for (auto controls : { Controls { 0, 0, 0, 0, 0, 0.73 }, Controls { 1, 1, 1, 1, 1, 0 } })
            {
                const auto out = runProcessor(in, controls, rate, channels, 0);
                for (std::size_t i = 0; i < in.size(); ++i)
                    ok &= same(out[i], { in[i].left, channels == 1 ? in[i].left : in[i].right });
            }
    check(ok, "all-zero depths and zero mix null bit-exactly including signed zero, mono and stereo, five rates");
}

template <unsigned lane>
bool testMask(const std::vector<Stereo>& input)
{
    Engine zero, masked;
    std::mt19937 rng(7515u);
    bool ok = true;
    for (int setting = 0; setting < 50; ++setting)
    {
        Controls controls;
        for (auto& c : controls) c = static_cast<double>(rng() % 1001u) / 1000.0;
        masked.prepare(48000, controls);
        controls[lane] = 0.0;
        zero.prepare(48000, controls);
        for (const auto& x : input)
            ok &= same(zero.process(x.left, x.right), masked.process<Engine::allLanes ^ (1u << lane)>(x.left, x.right));
    }
    return ok;
}

void testMonoAndDeterminism()
{
    const auto in = noise(16384);
    const auto ref = runProcessor(in, full, 48000, 2, 512);
    bool exact = true;
    for (int size : { 1, 64, 512, 4096, 0 })
    {
        const auto out = runProcessor(in, full, 48000, 2, size, size == 0);
        for (std::size_t i = 0; i < in.size(); ++i)
            exact &= same(out[i], ref[i]);
    }
    double energy = 0, residual = 0;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const double dry = static_cast<double>(in[i].left) + in[i].right;
        const double difference = static_cast<double>(ref[i].left) + ref[i].right - dry;
        energy += dry * dry;
        residual += difference * difference;
    }
    check(exact, "deterministic across processor instances, reset and block partition");
    check(residual < energy * 1e-13, "mono sum null below -130 dB");
}
} // namespace

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juce;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(argv[i]);
    if (args.size() >= 3 && args[0] == "--render")
        return renderFile(args);
    testNull();
    const auto input = noise(1536);
    check(testMask<vibrate::pitch>(input) && testMask<vibrate::pan>(input) && testMask<vibrate::delay>(input),
          "pitch, pan and delay null identically to the compile-time lane mask");
    testMonoAndDeterminism();
    return failures == 0 ? 0 : 1;
}
