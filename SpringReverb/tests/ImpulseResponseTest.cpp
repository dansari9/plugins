#include "DSP/SpringReverb.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct Render
{
    double sampleRate = 48000.0;
    juce::AudioBuffer<float> buffer;
};

Render render(double sampleRate, double seconds, int channels, const SpringReverb::Params& params,
              int blockSize, bool impulseOnBoth = true)
{
    SpringReverb reverb;
    reverb.prepare(sampleRate, blockSize, channels);
    reverb.setParameters(params, true);

    const int total = static_cast<int>(std::llround(seconds * sampleRate));
    juce::AudioBuffer<float> whole(channels, total);
    whole.clear();
    if (impulseOnBoth || channels == 1)
    {
        for (int ch = 0; ch < channels; ++ch)
            whole.setSample(ch, 0, 1.0f);
    }
    else
    {
        whole.setSample(0, 0, 1.0f);
    }

    juce::AudioBuffer<float> block(channels, blockSize);
    int rendered = 0;
    while (rendered < total)
    {
        const int n = std::min(blockSize, total - rendered);
        block.clear();
        for (int ch = 0; ch < channels; ++ch)
            block.copyFrom(ch, 0, whole, ch, rendered, n);
        juce::AudioBuffer<float> slice(block.getArrayOfWritePointers(), channels, 0, n);
        reverb.process(slice);
        for (int ch = 0; ch < channels; ++ch)
            whole.copyFrom(ch, rendered, slice, ch, 0, n);
        rendered += n;
    }

    return { sampleRate, std::move(whole) };
}

bool allFinite(const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const float* data = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (!std::isfinite(data[i]))
                return false;
    }
    return true;
}

float peakOf(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        peak = std::max(peak, buffer.getMagnitude(ch, 0, buffer.getNumSamples()));
    return peak;
}

double rmsRange(const juce::AudioBuffer<float>& buffer, int channel, int start, int count)
{
    const float* data = buffer.getReadPointer(channel);
    const int n = std::min(count, buffer.getNumSamples() - start);
    if (n <= 0)
        return 0.0;
    double energy = 0.0;
    for (int i = 0; i < n; ++i)
        energy += static_cast<double>(data[start + i]) * data[start + i];
    return std::sqrt(energy / static_cast<double>(n));
}

int peakIndex(const float* data, int n)
{
    int index = 0;
    float best = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs(data[i]);
        if (a > best)
        {
            best = a;
            index = i;
        }
    }
    return index;
}

double correlation(const juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const float* l = buffer.getReadPointer(0);
    const float* r = buffer.getReadPointer(1);
    double sumL = 0, sumR = 0, sumLL = 0, sumRR = 0, sumLR = 0;
    for (int i = 0; i < n; ++i)
    {
        sumL += l[i];
        sumR += r[i];
        sumLL += static_cast<double>(l[i]) * l[i];
        sumRR += static_cast<double>(r[i]) * r[i];
        sumLR += static_cast<double>(l[i]) * r[i];
    }
    const double inv = 1.0 / static_cast<double>(n);
    const double cov = sumLR - sumL * sumR * inv;
    const double varL = sumLL - sumL * sumL * inv;
    const double varR = sumRR - sumR * sumR * inv;
    return cov / std::sqrt(std::max(1.0e-18, varL * varR));
}

int samplesAbove(const float* data, int n, float threshold)
{
    int count = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs(data[i]) >= threshold)
            ++count;
    return count;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.getParentDirectory().createDirectory();
    if (file.existsAsFile())
        file.deleteFile();

    auto fileStream = std::make_unique<juce::FileOutputStream>(file);
    if (fileStream == nullptr || fileStream->failedToOpen())
        return false;

    std::unique_ptr<juce::OutputStream> stream(fileStream.release());
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sampleRate)
                             .withNumChannels(buffer.getNumChannels())
                             .withBitsPerSample(24);
    auto writer = format.createWriterFor(stream, options);
    if (writer == nullptr)
        return false;
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

SpringReverb::Params surfWet()
{
    SpringReverb::Params p;
    p.decaySeconds = 2.2f;
    p.tension = 0.22f;
    p.tone = 0.58f;
    p.drip = 0.78f;
    p.preDelayMs = 0.0f;
    p.mix = 1.0f;
    p.outputDb = 0.0f;
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    juce::String selfTest;
    const bool selfOk = springReverbDspSelfTest(selfTest);
    expect(selfOk, selfTest.toStdString());

    const auto wet = surfWet();
    auto ir = render(48000.0, 2.5, 2, wet, 256);
    expect(allFinite(ir.buffer), "48 kHz impulse produced a non-finite sample");

    const float rawPeak = peakOf(ir.buffer);
    std::cout << "raw peak " << rawPeak << "\n";
    expect(rawPeak > 1.0e-3f, "impulse response is silent");
    expect(rawPeak < 4.0f, "impulse response peaked at " + std::to_string(rawPeak));

    const float* left = ir.buffer.getReadPointer(0);
    const int peakAt = peakIndex(left, ir.buffer.getNumSamples());
    const double peakMs = 1000.0 * static_cast<double>(peakAt) / ir.sampleRate;
    std::cout << "peak at " << peakMs << " ms\n";
    expect(peakMs > 8.0 && peakMs < 160.0, "peak should be a spring arrival, not the dry click");

    const int earlyN = static_cast<int>(0.12 * ir.sampleRate);
    const int spread = samplesAbove(left, earlyN, rawPeak * 0.08f);
    std::cout << "hot samples in first 120 ms " << spread << "\n";
    expect(spread > 40, "the onset should be a chirp, not a single click");

    const double earlyRms = rmsRange(ir.buffer, 0, 0, static_cast<int>(0.25 * ir.sampleRate));
    const int win = static_cast<int>(0.2 * ir.sampleRate);
    const double midRms = rmsRange(ir.buffer, 0, static_cast<int>(0.40 * ir.sampleRate), win);
    const double laterRms = rmsRange(ir.buffer, 0, static_cast<int>(1.60 * ir.sampleRate), win);
    std::cout << "rms early " << earlyRms << " mid " << midRms << " later " << laterRms
              << " later/peak " << (laterRms / rawPeak) << "\n";
    // 1.2 s of a 2.2 s RT60 is about -33 dB. Allow extra damping from the
    // tone filters, and reject a tank that is already gone.
    expect(laterRms < midRms * 0.45, "tail should decay");
    expect(laterRms > midRms * 0.008, "2.2 s decay should still be present after 1.6 s");
    expect(laterRms > rawPeak * 8.0e-5, "tail is too quiet to hear under the attack");

    const double corr = correlation(ir.buffer);
    std::cout << "stereo correlation " << corr << "\n";
    expect(corr < 0.995, "left and right springs should not be identical");
    expect(corr > 0.15, "stereo tank should stay related");

    // Block-size invariance: same impulse, same parameters, different blocking.
    auto blocked = render(48000.0, 0.5, 2, wet, 64);
    auto big = render(48000.0, 0.5, 2, wet, 1024);
    float maxDiff = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* a = blocked.buffer.getReadPointer(ch);
        const float* b = big.buffer.getReadPointer(ch);
        for (int i = 0; i < blocked.buffer.getNumSamples(); ++i)
            maxDiff = std::max(maxDiff, std::abs(a[i] - b[i]));
    }
    std::cout << "block split max diff " << maxDiff << "\n";
    expect(maxDiff < 1.0e-4f, "processing must not depend on block size");

    // Fully dry is the input, including output gain.
    SpringReverb::Params dry = wet;
    dry.mix = 0.0f;
    dry.outputDb = -6.0f;
    auto dryRender = render(48000.0, 0.05, 2, dry, 128);
    const float gain = juce::Decibels::decibelsToGain(-6.0f);
    float dryError = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* data = dryRender.buffer.getReadPointer(ch);
        for (int i = 0; i < dryRender.buffer.getNumSamples(); ++i)
        {
            const float expected = (i == 0 ? 1.0f : 0.0f) * gain;
            dryError = std::max(dryError, std::abs(data[i] - expected));
        }
    }
    std::cout << "dry error " << dryError << "\n";
    expect(dryError < 1.0e-5f, "mix at 0 should pass the dry signal through");

    // Silence stays silence.
    SpringReverb reverb;
    reverb.prepare(48000.0, 256, 2);
    reverb.setParameters(wet, true);
    juce::AudioBuffer<float> zeros(2, 2048);
    zeros.clear();
    reverb.process(zeros);
    expect(peakOf(zeros) < 1.0e-6f, "silence in should be silence out");

    // Mono, 44.1 kHz and 192 kHz must stay finite and audible.
    auto mono = render(44100.0, 0.4, 1, wet, 128);
    expect(allFinite(mono.buffer), "44.1 kHz mono was non-finite");
    expect(peakOf(mono.buffer) > 1.0e-3f && peakOf(mono.buffer) < 4.0f, "44.1 kHz mono level");

    auto hi = render(192000.0, 0.25, 2, wet, 512);
    expect(allFinite(hi.buffer), "192 kHz was non-finite");
    expect(peakOf(hi.buffer) > 1.0e-3f && peakOf(hi.buffer) < 4.0f, "192 kHz level");

    // Long decay must not explode.
    SpringReverb::Params soaked = wet;
    soaked.decaySeconds = 5.5f;
    soaked.tension = 0.0f;
    soaked.drip = 1.0f;
    soaked.tone = 1.0f;
    auto hot = render(48000.0, 1.2, 2, soaked, 256);
    expect(allFinite(hot.buffer), "max decay was non-finite");
    expect(peakOf(hot.buffer) < 4.0f, "max decay ran away");
    const double hotEarly = rmsRange(hot.buffer, 0, 0, static_cast<int>(0.2 * 48000.0));
    const double hotLate = rmsRange(hot.buffer, 0, static_cast<int>(0.9 * 48000.0),
                                    static_cast<int>(0.25 * 48000.0));
    expect(hotLate < hotEarly * 1.15, "max decay should not grow over the first second");

    // Parameter motion with no input must not click.
    reverb.reset();
    reverb.setParameters(wet, true);
    SpringReverb::Params moved = wet;
    moved.decaySeconds = 5.0f;
    moved.tension = 1.0f;
    moved.tone = 0.0f;
    moved.drip = 0.0f;
    moved.preDelayMs = 40.0f;
    moved.outputDb = 6.0f;
    reverb.setParameters(moved, false);
    juce::AudioBuffer<float> idle(2, 4000);
    idle.clear();
    reverb.process(idle);
    expect(peakOf(idle) < 1.0e-4f, "smoothing a parameter on silence should not click");

    if (argc >= 2)
    {
        juce::AudioBuffer<float> listening(ir.buffer.getNumChannels(), ir.buffer.getNumSamples());
        listening.makeCopyOf(ir.buffer);
        const float norm = 0.8912509f / std::max(rawPeak, 1.0e-8f); // about -1 dBFS
        listening.applyGain(norm);

        const juce::File outFile(juce::String::fromUTF8(argv[1]));
        expect(writeWav(outFile, listening, ir.sampleRate),
               "could not write " + outFile.getFullPathName().toStdString());
        if (failures == 0)
            std::cout << "wrote " << outFile.getFullPathName() << "\n";
    }

    if (failures > 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "spring tank impulse checks passed\n";
    return 0;
}
