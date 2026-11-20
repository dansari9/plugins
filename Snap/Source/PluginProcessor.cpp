#include "PluginProcessor.h"
#include "chain/Latency.h"
#include "presets/StateIO.h"

using namespace juce;

SnapProcessor::SnapProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, nullptr, "SNAP", snap::createLayout()), magnitude(apvts.getRawParameterValue("magnitude")),
      bypass(apvts.getRawParameterValue("bypass")), lowLatency(apvts.getRawParameterValue("low_latency"))
{
    constexpr const char* ids[numMaster] = { "in_gain", "out_gain", "eq_on",  "cut_lo", "cut_hi",  "lc_shape", "hc_shape",
                                             "tone",    "tone_shape", "air", "attack", "sustain", "clip_on" };
    for (size_t k = 0; k < controls.size(); ++k)
        controls[k] = { &snap::spec(ids[k]), apvts.getRawParameterValue(ids[k]) };
    apvts.addParameterListener("low_latency", this);
}

SnapProcessor::~SnapProcessor() { apvts.removeParameterListener("low_latency", this); }

void SnapProcessor::parameterChanged(const String&, float value) { setLatencySamples(snap::chainLatency(sampleRate, value > 0.5f)); }

bool SnapProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

float SnapProcessor::scaled(int control) const
{
    const auto& c = controls[(size_t) control];
    return snap::applyMagnitude(*c.spec, c.raw->load(), *magnitude * 0.01f);
}

// Bypass sends the master to neutral (its own ramps keep the switch click-free) and keeps the reported latency.
snap::Master::Params SnapProcessor::masterParams() const
{
    const bool low = *lowLatency > 0.5f;
    if (*bypass > 0.5f)
        return { .eqOn = false, .lowLatency = low };
    return { .eqOn = scaled(eqOn) > 0.5f, .cutLo = scaled(cutLo), .cutHi = scaled(cutHi), .lcShape = (int) scaled(lcShape),
             .hcShape = (int) scaled(hcShape), .tone = scaled(tone) * 0.01f, .toneShape = (int) scaled(toneShape),
             .air = scaled(air) * 0.01f, .attack = scaled(attack) * 0.01f, .sustain = scaled(sustain) * 0.01f,
             .clipOn = scaled(clipOn) > 0.5f, .outDb = scaled(outGain), .lowLatency = low };
}

void SnapProcessor::prepareToPlay(double fs, int)
{
    fs = fs > 0 && fs < 1.0e7 ? fs : 48000.0; // a nonsense rate (NaN, inf, <= 0) must not reach the int casts below
    sampleRate = fs;
    const int channels = jlimit(1, 2, getTotalNumInputChannels());
    master.setParams(masterParams());
    master.prepare(fs, channels, subBlock);
    inputMeter.prepare(fs, channels);
    inGainDb.prepare(fs), inGainDb.reset(*bypass > 0.5f ? 0.0f : scaled(inGain));
    int pad = 0;
    for (bool low : { false, true })
        pad = jmax(pad, snap::chainLatency(fs, low) - snap::Master::latencyFor(fs, low));
    chainPad.prepare(2, pad);
    setLatencySamples(snap::chainLatency(fs, *lowLatency > 0.5f));
    peakIn = peakOut = peakGain = 0;
    decimation = jmax(1, (int) (fs * 2.5 / historySize)), count = 0;
}

void SnapProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), channels = jmin(2, getTotalNumInputChannels(), buffer.getNumChannels());
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    if (channels == 0)
        return;
    for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
        for (float* x = buffer.getWritePointer(c); x != buffer.getWritePointer(c) + n; ++x)
            *x = std::isfinite(*x) ? jlimit(-1000.0f, 1000.0f, *x) : 0.0f;
    inputMeter.process(buffer, n);

    master.setParams(masterParams());
    chainPad.setDelay(snap::chainLatency(sampleRate, *lowLatency > 0.5f) - master.latency());
    inGainDb.setTarget(*bypass > 0.5f ? 0.0f : scaled(inGain));
    for (int start = 0; start < n; start += subBlock)
    {
        const int len = jmin(subBlock, n - start);
        float level = 0, outLevel = 0;
        for (int i = start; i < start + len; ++i)
        {
            const float db = inGainDb.next(), gain = db == 0 ? 1.0f : Decibels::decibelsToGain(db, -1000.0f);
            for (int c = 0; c < channels; ++c)
            {
                const float x = buffer.getSample(c, i);
                level = jmax(level, std::abs(x));
                buffer.setSample(c, i, chainPad.next(c, x * gain));
            }
        }
        AudioBuffer<float> slice(buffer.getArrayOfWritePointers(), channels, start, len);
        master.process(slice, len, nullptr);
        for (int c = 0; c < channels; ++c)
            outLevel = jmax(outLevel, slice.getMagnitude(c, 0, len));

        const float applied = master.shaperGainDb();
        peakIn = jmax(peakIn, level), peakOut = jmax(peakOut, outLevel);
        peakGain = std::abs(applied) > std::abs(peakGain) ? applied : peakGain;
        if ((count += len) >= decimation)
        {
            const int w = history.write.load(std::memory_order_relaxed);
            history.in[(size_t) w] = peakIn, history.out[(size_t) w] = peakOut, history.gainDb[(size_t) w] = peakGain;
            history.write.store((w + 1) % historySize, std::memory_order_release);
            peakIn = peakOut = peakGain = 0, count = 0;
        }
    }
}

// Output is Magnitude-scaled, so the raw value is the needed gain divided by Magnitude. Both meters run continuously and
// the pre-Output one does not see Output, so one call lands within a fraction of a dB once the window has filled.
void SnapProcessor::matchLoudness()
{
    const float m = *magnitude * 0.01f, in = inputLufs(), pre = preOutputLufs();
    if (m <= 0 || in <= snap::LoudnessMeter::silence || pre <= snap::LoudnessMeter::silence)
        return;
    auto* out = apvts.getParameter("out_gain");
    out->setValueNotifyingHost(out->convertTo0to1(jlimit(-24.0f, 24.0f, (in - pre) / m)));
}

void SnapProcessor::getStateInformation(MemoryBlock& dest) { snap::writeState(apvts, chainOrder, dest); }
void SnapProcessor::setStateInformation(const void* data, int size) { snap::readState(apvts, chainOrder, data, size); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SnapProcessor(); }
