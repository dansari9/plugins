#include "PluginProcessor.h"
#include "chain/Latency.h"
#include "presets/StateIO.h"

using namespace juce;

SnapProcessor::SnapProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, nullptr, "SNAP", snap::createLayout()), magnitude(apvts.getRawParameterValue("magnitude")),
      bypass(apvts.getRawParameterValue("bypass")), lowLatency(apvts.getRawParameterValue("low_latency"))
{
    for (auto& s : snap::parameterTable())
        controls.emplace(s.id, Control { &s, apvts.getRawParameterValue(s.id) });
    apvts.addParameterListener("low_latency", this);
}

SnapProcessor::~SnapProcessor() { apvts.removeParameterListener("low_latency", this); }

void SnapProcessor::parameterChanged(const String&, float v) { setLatencySamples(snap::chainLatency(sampleRate, v > 0.5f)); }

bool SnapProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

float SnapProcessor::value(std::string_view id) const
{
    const auto& c = controls.at(id);
    return snap::applyMagnitude(*c.spec, c.raw->load(), *magnitude * 0.01f);
}

// Bypass (and a module's switch) ramps its amounts to zero, so the switch is click-free and the latency stays put.
snap::SubHiss::Settings SnapProcessor::subHissSettings() const
{
    snap::SubHiss::Settings s;
    s.enabled = active("subhiss_on") > 0;
    auto& sub = s.sub;
    sub.amount = value("sub_amt"), sub.sensitivity = value("sub_sens"), sub.detectLoHz = value("sub_det_lo");
    sub.detectHiHz = value("sub_det_hi"), sub.decayMs = value("sub_decay"), sub.duck = value("sub_duck");
    sub.solo = on("sub_solo"), sub.post = on("sub_post"), sub.pitch = value("sub_pitch"), sub.stereo = value("sub_stereo");
    sub.color = value("sub_color"), sub.acid = on("sub_acid");
    auto& hiss = s.hiss;
    hiss.amount = value("hiss_amt"), hiss.sensitivity = value("hiss_sens"), hiss.detectLoHz = value("hiss_det_lo");
    hiss.detectHiHz = value("hiss_det_hi"), hiss.decayMs = value("hiss_decay"), hiss.duck = value("hiss_duck");
    hiss.solo = on("hiss_solo"), hiss.post = on("hiss_post"), hiss.pitch = value("hiss_pitch"), hiss.tone = value("hiss_tone");
    hiss.env = value("hiss_env"), hiss.res = value("hiss_res"), hiss.type = (int) value("hiss_type");
    return s;
}

snap::Space::Settings SnapProcessor::spaceSettings() const
{
    return { .enabled = active("space_on") > 0, .type = (int) value("space_type"), .amount = value("space_amt"),
             .decay = value("space_decay"), .stretch = value("space_stretch"), .predelayMs = value("space_predelay"),
             .duck = value("space_duck"), .pan = value("space_pan"), .width = value("space_width"),
             .focusLoHz = value("space_focus_lo"), .focusHiHz = value("space_focus_hi") };
}

void SnapProcessor::setModuleParams()
{
    shift.setParams({ .amount = active("shift_on") * value("shift_amt") * 0.01f, .balance = value("shift_balance"),
                      .invert = on("shift_invert"), .stereo = value("shift_stereo") * 0.01f,
                      .stretch = value("shift_stretch") * 0.01f, .formantMix = value("shift_fmix") * 0.01f,
                      .target = value("shift_target"), .focusLo = value("shift_focus_lo"), .focusHi = value("shift_focus_hi") });
    press.setParams({ .amount = active("press_on") * value("press_amt") * 0.01f, .mode = (int) value("press_mode"),
                      .downUp = value("press_downup"), .mix = value("press_mix") * 0.01f, .bands = value("press_bands") });
    drive.setParams({ .amount = active("drive_on") * value("drive_amt") * 0.01f, .mix = value("drive_mix") * 0.01f,
                      .target = value("drive_target"), .focusLo = value("drive_focus_lo"), .focusHi = value("drive_focus_hi"),
                      .mode = (int) value("drive_mode"), .flavor = value("drive_flavor") });
    push.setParams({ .amount = active("push_on") * value("push_amt") * 0.01f, .mix = value("push_mix") * 0.01f,
                     .target = value("push_target"), .focusLo = value("push_focus_lo"), .focusHi = value("push_focus_hi"),
                     .mult = (int) value("push_mult"), .tone = value("push_tone") });
}

// Bypass sends the master to neutral (its own ramps keep the switch click-free) and keeps the reported latency.
snap::Master::Params SnapProcessor::masterParams() const
{
    const bool low = *lowLatency > 0.5f;
    if (*bypass > 0.5f)
        return { .eqOn = false, .lowLatency = low };
    return { .eqOn = on("eq_on"), .cutLo = value("cut_lo"), .cutHi = value("cut_hi"), .lcShape = (int) value("lc_shape"),
             .hcShape = (int) value("hc_shape"), .tone = value("tone") * 0.01f, .toneShape = (int) value("tone_shape"),
             .air = value("air") * 0.01f, .attack = value("attack") * 0.01f, .sustain = value("sustain") * 0.01f,
             .clipOn = on("clip_on"), .outDb = value("out_gain"), .lowLatency = low };
}

// Audio thread. Every stage was prepared for both modes; shift is off in Low Latency Mode.
void SnapProcessor::setLowLatency(bool low)
{
    lowLatencyNow = low;
    subHiss.setLowLatency(low);
    for (snap::ModuleBase* m : std::array<snap::ModuleBase*, 4> { &shift, &press, &drive, &push })
        m->setLowLatency(low);
    chainPad.reset(), postDelay.reset();
}

void SnapProcessor::prepareToPlay(double fs, int)
{
    fs = fs > 0 && fs < 1.0e7 ? fs : 48000.0; // a nonsense rate (NaN, inf, <= 0) must not reach the int casts below
    sampleRate = fs;
    const int channels = jlimit(1, 2, getTotalNumInputChannels());
    const bool low = lowLatencyNow = *lowLatency > 0.5f;
    master.setParams(masterParams());
    master.prepare(fs, channels, subBlock);
    setModuleParams(); // before prepare(), so the modules start at their values without a ramp
    const dsp::ProcessSpec spec { fs, (uint32) subBlock, (uint32) channels };
    subHiss.prepare(spec, subHissSettings(), low);
    space.prepare(spec, spaceSettings());
    for (snap::ModuleBase* m : std::array<snap::ModuleBase*, 4> { &shift, &press, &drive, &push })
        m->prepare(fs, channels, subBlock, low);
    postBus.setSize(channels, subBlock);
    postDelay.prepare(channels, snap::chainLatency(fs, false));
    chainPad.prepare(channels, snap::chainLatency(fs, true));
    inputMeter.prepare(fs, channels);
    inGainDb.prepare(fs), inGainDb.reset(*bypass > 0.5f ? 0.0f : value("in_gain"));
    setLatencySamples(snap::chainLatency(fs, low));
    peakIn = peakOut = peakGain = 0;
    decimation = jmax(1, (int) (fs * 2.5 / historySize)), count = 0;
}

void SnapProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), channels = jmin(2, getTotalNumInputChannels(), buffer.getNumChannels());
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    if (channels != postBus.getNumChannels()) // no input, or a layout this instance was not prepared for
    {
        buffer.clear();
        return;
    }
    for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
        for (float* x = buffer.getWritePointer(c); x != buffer.getWritePointer(c) + n; ++x)
            *x = std::isfinite(*x) ? jlimit(-1000.0f, 1000.0f, *x) : 0.0f;
    inputMeter.process(buffer, n);

    if (const bool low = *lowLatency > 0.5f; low != lowLatencyNow)
        setLowLatency(low);
    master.setParams(masterParams());
    setModuleParams();
    const auto subHissNow = subHissSettings();
    const auto spaceNow = spaceSettings();
    // The stages after sub/hiss up to the post-bus sum, and whatever the constant latency has left over.
    const int afterSubHiss = shift.latency() + space.getLatencySamples() + press.latency() + drive.latency() + push.latency();
    postDelay.setDelay(afterSubHiss + master.postBusLatency());
    chainPad.setDelay(snap::chainLatency(sampleRate, lowLatencyNow) - subHiss.getLatencySamples() - afterSubHiss - master.latency());
    inGainDb.setTarget(*bypass > 0.5f ? 0.0f : value("in_gain"));
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
        // sub/hiss -> shift -> space -> press -> drive -> push (default order until M7) -> master, Post bus before Clip.
        AudioBuffer<float> slice(buffer.getArrayOfWritePointers(), channels, start, len);
        AudioBuffer<float> post(postBus.getArrayOfWritePointers(), channels, 0, len);
        subHiss.process(slice, post, subHissNow);
        shift.process(slice, len);
        space.process(slice, spaceNow);
        press.process(slice, len);
        drive.process(slice, len);
        push.process(slice, len);
        for (int c = 0; c < channels; ++c)
            for (float* x = post.getWritePointer(c); x != post.getWritePointer(c) + len; ++x)
                *x = postDelay.next(c, *x);
        master.process(slice, len, post.getArrayOfReadPointers());
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
