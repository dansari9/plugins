#include "PluginProcessor.h"
#include "chain/Latency.h"
#include "presets/PresetStore.h"

using namespace juce;

SnapProcessor::SnapProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, &undoManager, "SNAP", snap::createLayout()), magnitude(apvts.getRawParameterValue("magnitude")),
      bypass(apvts.getRawParameterValue("bypass")), lowLatency(apvts.getRawParameterValue("low_latency")),
      oversampling(apvts.getRawParameterValue("oversampling"))
{
    for (auto& s : snap::parameterTable())
        controls.emplace(s.id, Control { &s, apvts.getRawParameterValue(s.id) });
    apvts.state.setProperty("order", String(snap::toString(snap::defaultChainOrder)), nullptr);
    apvts.state.addListener(this);
    apvts.addParameterListener("low_latency", this);
    apvts.addParameterListener("oversampling", this);
}

SnapProcessor::~SnapProcessor()
{
    apvts.removeParameterListener("oversampling", this);
    apvts.removeParameterListener("low_latency", this);
    apvts.state.removeListener(this);
}

// Low Latency Mode switches on the audio thread; a change of the oversampling factor needs a re-prepare. Both the new
// latency report and a re-prepare happen on the message thread (a change from another thread is handed over).
void SnapProcessor::parameterChanged(const String&, float)
{
    if (!prepared)
        return;
    if (MessageManager::existsAndIsCurrentThread())
        handleAsyncUpdate();
    else
        triggerAsyncUpdate();
}

void SnapProcessor::handleAsyncUpdate()
{
    if (!prepared)
        return;
    if (factorFor(*lowLatency > 0.5f) != factor)
        reprepare();
    else
        setLatencySamples(latencyFor(*lowLatency > 0.5f));
}

void SnapProcessor::reprepare()
{
    suspendProcessing(true); // waits for a running block; the wrappers output silence until resumed
    prepareToPlay(getSampleRate() > 0 ? getSampleRate() : sampleRate, jmax(1, getBlockSize()));
    suspendProcessing(false);
}

void SnapProcessor::valueTreePropertyChanged(ValueTree& tree, const Identifier& property)
{
    if (tree == apvts.state && property == Identifier("order"))
        requestedOrder.store(snap::pack(order()), std::memory_order_release);
}

snap::ChainOrder SnapProcessor::order() const
{
    return snap::parseChainOrder(apvts.state.getProperty("order").toString().toStdString()).value_or(snap::defaultChainOrder);
}

void SnapProcessor::setOrder(const snap::ChainOrder& o)
{
    beginUndoStep();
    apvts.state.setProperty("order", String(snap::toString(o)), &undoManager);
}

void SnapProcessor::storeSnapshot(int slot)
{
    snap::Snapshot s { {}, order() };
    for (auto& spec : snap::parameterTable())
        s.values.push_back(apvts.getParameter(spec.id)->getValue());
    snapshots[(size_t) slot] = std::move(s);
}

bool SnapProcessor::recallSnapshot(int slot)
{
    const auto& s = snapshots[(size_t) slot];
    if (!s)
        return false;
    beginUndoStep();
    for (size_t i = 0; i < snap::parameterTable().size(); ++i)
        if (const auto* id = snap::parameterTable()[i].id; !snap::isSessionToggle(id))
            apvts.getParameter(id)->setValueNotifyingHost(s->values[i]);
    commit();
    apvts.state.setProperty("order", String(snap::toString(s->order)), &undoManager);
    return true;
}

bool SnapProcessor::loadPreset(const void* data, int size)
{
    beginUndoStep();
    const bool ok = snap::readState(apvts, locks, snapshots, data, size, snap::Load::preset);
    commit();
    return ok;
}

bool SnapProcessor::undo()
{
    commit(); // pending parameter changes belong to the step being undone
    return undoManager.undo();
}

bool SnapProcessor::redo()
{
    commit();
    return undoManager.redo();
}

int SnapProcessor::latencyFor(bool low) const
{
    if (low)
        return snap::constants::lowLatencySamples;
    return (snap::chainLatency(sampleRate * factor, false) + factor - 1) / factor + os.latency();
}

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
    for (auto& [shift, space, press, drive] : middle.sets)
    {
    shift.setParams({ .amount = active("shift_on") * value("shift_amt") * 0.01f, .balance = value("shift_balance"),
                      .invert = on("shift_invert"), .stereo = value("shift_stereo") * 0.01f,
                      .stretch = value("shift_stretch") * 0.01f, .formantMix = value("shift_fmix") * 0.01f,
                      .target = value("shift_target"), .focusLo = value("shift_focus_lo"), .focusHi = value("shift_focus_hi") });
    press.setParams({ .amount = active("press_on") * value("press_amt") * 0.01f, .mode = (int) value("press_mode"),
                      .downUp = value("press_downup"), .mix = value("press_mix") * 0.01f, .bands = value("press_bands"),
                      .attack = value("press_attack") });
    drive.setParams({ .amount = active("drive_on") * value("drive_amt") * 0.01f, .mix = value("drive_mix") * 0.01f,
                      .target = value("drive_target"), .focusLo = value("drive_focus_lo"), .focusHi = value("drive_focus_hi"),
                      .mode = (int) value("drive_mode"), .flavor = value("drive_flavor") });
    }
    push.setParams({ .amount = active("push_on") * value("push_amt") * 0.01f, .mix = value("push_mix") * 0.01f,
                     .target = value("push_target"), .focusLo = value("push_focus_lo"), .focusHi = value("push_focus_hi"),
                     .mult = (int) value("push_mult"), .tone = value("push_tone") });
}

// Bypass sends the master to neutral (its own ramps keep the switch click-free) and keeps the reported latency.
snap::Master::Params SnapProcessor::masterParams() const
{
    const bool low = lowLatencyNow; // the mode the rest of the chain runs in, even while a re-prepare is pending
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
    middle.setLowLatency(low);
    push.setLowLatency(low);
    chainPad.reset(), postDelay.reset();
}

// The chain runs at fs * factor between the oversampler's up and down; the reported latency rounds its share up to whole
// base-rate samples (chainPad fills the difference).
void SnapProcessor::prepareToPlay(double fs, int)
{
    fs = fs > 0 && fs < 1.0e7 ? fs : 48000.0; // a nonsense rate (NaN, inf, <= 0) must not reach the int casts below
    sampleRate = fs;
    const int channels = jlimit(1, 2, getTotalNumInputChannels());
    const bool low = lowLatencyNow = *lowLatency > 0.5f;
    factor = factorFor(low);
    os.prepare(channels, subBlock, factor > 1 ? snap::constants::oversamplingStages : 0, false);
    const double hi = fs * factor;
    const int hiBlock = subBlock * factor;
    master.setParams(masterParams());
    master.prepare(hi, channels, hiBlock);
    setModuleParams(); // before prepare(), so the modules start at their values without a ramp
    const dsp::ProcessSpec spec { hi, (uint32) hiBlock, (uint32) channels };
    subHiss.prepare(spec, subHissSettings(), low);
    middle.prepare(hi, channels, hiBlock, low, spaceSettings(), snap::unpack(requestedOrder.load()));
    push.prepare(hi, channels, hiBlock, low);
    postBus.setSize(channels, hiBlock);
    postDelay.prepare(channels, snap::chainLatency(hi, false));
    chainPad.prepare(channels, snap::chainLatency(hi, false) + factor);
    inputMeter.prepare(fs, channels);
    inGainDb.prepare(fs), inGainDb.reset(*bypass > 0.5f ? 0.0f : value("in_gain"));
    setLatencySamples(latencyFor(low));
    prepared = true;
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

    if (const bool low = *lowLatency > 0.5f; low != lowLatencyNow && factorFor(low) == factor) // else a re-prepare is coming
        setLowLatency(low);
    master.setParams(masterParams());
    setModuleParams();
    const auto subHissNow = subHissSettings();
    const auto spaceNow = spaceSettings();
    const auto wanted = snap::unpack(requestedOrder.load(std::memory_order_acquire));
    // The stages after sub/hiss up to the post-bus sum, and whatever the constant latency has left over (at the chain rate).
    const int afterSubHiss = middle.latency() + push.latency();
    const int stages = subHiss.getLatencySamples() + afterSubHiss + master.latency();
    postDelay.setDelay(afterSubHiss + master.postBusLatency());
    chainPad.setDelay((latencyFor(lowLatencyNow) - os.latency()) * factor - stages);
    inGainDb.setTarget(*bypass > 0.5f ? 0.0f : value("in_gain"));
    for (int start = 0; start < n; start += subBlock)
    {
        const int len = jmin(subBlock, n - start), hiLen = len * factor;
        float level = 0, outLevel = 0;
        int w = display.scopeWrite.load(std::memory_order_relaxed);
        for (int i = start; i < start + len; ++i)
        {
            const float db = inGainDb.next(), gain = db == 0 ? 1.0f : Decibels::decibelsToGain(db, -1000.0f);
            float mono = 0;
            for (int c = 0; c < channels; ++c)
            {
                const float x = buffer.getSample(c, i);
                level = jmax(level, std::abs(x));
                buffer.setSample(c, i, x * gain);
                mono += x * gain / (float) channels;
            }
            display.scope[(size_t) w].store(mono, std::memory_order_relaxed);
            w = (w + 1) % Display::scopeSize;
        }
        display.scopeWrite.store(w, std::memory_order_release);
        // os up -> pad -> sub/hiss -> the middle four in the stored order -> push -> master (Post bus before Clip) -> os down.
        dsp::AudioBlock<float> base(buffer.getArrayOfWritePointers(), (size_t) channels, (size_t) start, (size_t) len);
        auto up = os.up(base);
        std::array<float*, 2> hi { up.getChannelPointer(0), up.getChannelPointer((size_t) channels - 1) };
        AudioBuffer<float> slice(hi.data(), channels, hiLen);
        for (int c = 0; c < channels; ++c)
            for (float* x = hi[(size_t) c]; x != hi[(size_t) c] + hiLen; ++x)
                *x = chainPad.next(c, *x);
        AudioBuffer<float> post(postBus.getArrayOfWritePointers(), channels, 0, hiLen);
        subHiss.process(slice, post, subHissNow);
        middle.process(wanted, slice, spaceNow);
        push.process(slice, hiLen);
        for (int c = 0; c < channels; ++c)
            for (float* x = post.getWritePointer(c); x != post.getWritePointer(c) + hiLen; ++x)
                *x = postDelay.next(c, *x);
        master.process(slice, hiLen, post.getArrayOfReadPointers());
        os.down(base);
        for (int c = 0; c < channels; ++c)
            outLevel = jmax(outLevel, buffer.getMagnitude(c, start, len));

        const auto& modules = middle.current();
        const auto keep = [](std::atomic<float>& meter, float v) { meter.store(jmax(v, meter.load(std::memory_order_relaxed)), std::memory_order_relaxed); };
        keep(display.spaceWet, modules.space.wetPeak()), keep(display.driveIn, modules.drive.inPeak());
        keep(display.driveAdded, modules.drive.addedPeak()), keep(display.pushIn, push.inPeak()), keep(display.pushAdded, push.addedPeak());
        display.inPeak.store(jmax(level, display.inPeak.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        display.outPeak.store(jmax(outLevel, display.outPeak.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    }
    const auto& press = middle.current().press;
    display.pressDownDb.store(press.downwardDb(), std::memory_order_relaxed);
    display.pressUpDb.store(press.upwardDb(), std::memory_order_relaxed);
    display.pushLimitDb.store(push.limitDb(), std::memory_order_relaxed);
    for (int layer = 0; layer < 2; ++layer)
        display.hits[(size_t) layer].store(subHiss.hits(layer), std::memory_order_relaxed);
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

void SnapProcessor::getStateInformation(MemoryBlock& dest) { snap::writeState(apvts, locks, snapshots, dest); }

// A host restore replaces everything and starts a fresh undo history.
void SnapProcessor::setStateInformation(const void* data, int size)
{
    if (snap::readState(apvts, locks, snapshots, data, size, snap::Load::session))
        commit(), undoManager.clearUndoHistory();
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() // a new instance starts on the bold default preset; a host restore replaces it
{
    auto* p = new SnapProcessor();
    snap::loadDefaultPreset(*p);
    return p;
}
