#include "PluginProcessor.h"
#include "chain/Latency.h"
#include "presets/StateIO.h"

using namespace juce;

SnapProcessor::SnapProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, nullptr, "SNAP", snap::createLayout()), magnitude(apvts.getRawParameterValue("magnitude")),
      bypass(apvts.getRawParameterValue("bypass")), lowLatency(apvts.getRawParameterValue("low_latency")),
      master { { { snap::spec("in_gain"), apvts.getRawParameterValue("in_gain") },
                 { snap::spec("out_gain"), apvts.getRawParameterValue("out_gain") },
                 { snap::spec("attack"), apvts.getRawParameterValue("attack") },
                 { snap::spec("sustain"), apvts.getRawParameterValue("sustain") } } }
{
    apvts.addParameterListener("low_latency", this);
}

SnapProcessor::~SnapProcessor() { apvts.removeParameterListener("low_latency", this); }

void SnapProcessor::parameterChanged(const String&, float value) { setLatencySamples(snap::chainLatency(sampleRate, value > 0.5f)); }

bool SnapProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

void SnapProcessor::prepareToPlay(double fs, int)
{
    fs = fs > 0 && fs < 1.0e7 ? fs : 48000.0; // a nonsense rate (NaN, inf, <= 0) must not reach the int casts below
    sampleRate = fs;
    shaper.prepare(fs);
    chainPad.prepare(2, jmax(snap::chainLatency(fs, false), snap::chainLatency(fs, true)));
    setLatencySamples(snap::chainLatency(fs, *lowLatency > 0.5f));
    peakIn = peakOut = peakGain = 0;
    decimation = jmax(1, (int) (fs * 2.5 / historySize)), count = 0;
    for (auto& s : master)
        s.smooth.reset(fs, snap::constants::smoothingSeconds), s.smooth.setCurrentAndTargetValue(s.target(*magnitude * 0.01f));
}

void SnapProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), channels = jmin(2, getTotalNumInputChannels(), buffer.getNumChannels());
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    if (channels == 0)
        return;
    chainPad.setDelay(snap::chainLatency(sampleRate, *lowLatency > 0.5f));
    if (*bypass > 0.5f) // bypassed audio keeps the reported latency
    {
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                buffer.setSample(c, i, chainPad.next(c, buffer.getSample(c, i)));
        return;
    }
    for (auto& s : master)
        s.smooth.setTargetValue(s.target(*magnitude * 0.01f));
    float* x[2] = { buffer.getWritePointer(0), buffer.getWritePointer(channels - 1) };

    for (int i = 0; i < n; ++i)
    {
        const float in = Decibels::decibelsToGain(master[inGain].smooth.getNextValue(), -1000.0f);
        float level = 0, outLevel = 0;
        for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
            level = jmax(level, std::abs(x[c][i] = chainPad.next(c, (std::isfinite(x[c][i]) ? jlimit(-1000.0f, 1000.0f, x[c][i]) : 0.0f) * in)));

        const float applied = shaper.next(level, master[attack].smooth.getNextValue(), master[sustain].smooth.getNextValue());
        const float gain = Decibels::decibelsToGain(applied, -1000.0f) * Decibels::decibelsToGain(master[outGain].smooth.getNextValue(), -1000.0f);
        for (int c = 0; c < channels; ++c)
            outLevel = jmax(outLevel, std::abs(x[c][i] *= gain));

        peakIn = jmax(peakIn, level), peakOut = jmax(peakOut, outLevel);
        peakGain = std::abs(applied) > std::abs(peakGain) ? applied : peakGain;
        if (++count >= decimation)
        {
            const int w = history.write.load(std::memory_order_relaxed);
            history.in[(size_t) w] = peakIn, history.out[(size_t) w] = peakOut, history.gainDb[(size_t) w] = peakGain;
            history.write.store((w + 1) % historySize, std::memory_order_release);
            peakIn = peakOut = peakGain = 0, count = 0;
        }
    }
}

void SnapProcessor::getStateInformation(MemoryBlock& dest) { snap::writeState(apvts, chainOrder, dest); }
void SnapProcessor::setStateInformation(const void* data, int size) { snap::readState(apvts, chainOrder, data, size); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SnapProcessor(); }
