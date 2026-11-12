#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"

using namespace rootnote;

namespace
{
const juce::Identifier kEditorWidth { "editorWidth" };
const juce::Identifier kEditorHeight { "editorHeight" };
const juce::Identifier kPreset { "preset" };

/** As a host sees a move on the plug-in's own interface. Does nothing when the value is already there. */
void setNormalised(juce::AudioProcessorParameter& parameter, float value)
{
    if (parameter.getValue() == value)
        return;
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(value);
    parameter.endChangeGesture();
}

bool toEngineEvent(const juce::MidiMessage& message, MidiEvent& event) noexcept
{
    if (message.isNoteOn())
        event.type = MidiEvent::Type::noteOn;
    else if (message.isNoteOff())
        event.type = MidiEvent::Type::noteOff;
    else if (message.isAllNotesOff() || message.isAllSoundOff())
        event.type = MidiEvent::Type::allNotesOff;
    else
        return false;

    event.channel = message.getChannel();
    event.note = message.isNoteOnOrOff() ? message.getNoteNumber() : 0;
    return true;
}
} // namespace

RootnoteProcessor::RootnoteProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "RootnoteState", createParameterLayout()),
      bypassParameter(dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter(ids::bypass)))
{
}

void RootnoteProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    displaySampleRate.store(sampleRate, std::memory_order_relaxed);
    engine.prepare(sampleRate, samplesPerBlock);
    analysis.setSampleRate(sampleRate);
}

void RootnoteProcessor::reset()
{
    engine.reset();
}

bool RootnoteProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return outOk && (in == out || in == juce::AudioChannelSet::mono());
}

void RootnoteProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = juce::jmin(2, getTotalNumOutputChannels(), buffer.getNumChannels());
    if (numChannels < 1 || !engine.isPrepared())
        return;

    if (numChannels == 2 && getTotalNumInputChannels() == 1)
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);

    float* const channels[2] = { buffer.getWritePointer(0), numChannels == 2 ? buffer.getWritePointer(1) : nullptr };
    analysis.push(channels[0], channels[1], numSamples);

    // A restored state plays its own notes, not a MIDI note left over from before.
    if (stateRestored.exchange(false, std::memory_order_acquire))
        engine.releaseMidi();
    engine.setParams(parameters.read());

    // Each MIDI event takes effect on its own sample: process up to it, apply it, carry on.
    int position = 0;
    for (const auto metadata : midi)
    {
        // Longer messages (sysex) are of no interest, and building one would allocate.
        MidiEvent event;
        if (metadata.numBytes > 3 || !toEngineEvent(metadata.getMessage(), event))
            continue;
        const int at = juce::jlimit(position, numSamples, metadata.samplePosition);
        processSpan(channels, numChannels, position, at);
        position = at;
        engine.handleMidi(event);
    }
    processSpan(channels, numChannels, position, numSamples);

    publishDisplay(buffer, numChannels);
}

void RootnoteProcessor::processSpan(float* const* channels, int numChannels, int from, int to) noexcept
{
    if (to <= from)
        return;
    float* const span[2] = { channels[0] + from, numChannels == 2 ? channels[1] + from : nullptr };
    engine.process(span, numChannels, to - from);
}

void RootnoteProcessor::publishDisplay(const juce::AudioBuffer<float>& buffer, int numChannels) noexcept
{
    if (displayUsers.load(std::memory_order_relaxed) <= 0)
        return;

    const size_t w = displayWrite.load(std::memory_order_relaxed);
    if (w - displayRead.load(std::memory_order_acquire) < kDisplayFrames)
    {
        auto& frame = displayFrames[w % kDisplayFrames];
        frame.sampleRate = displaySampleRate.load(std::memory_order_relaxed);
        frame.boostHz = engine.boostHz();
        frame.cutHz = engine.cutHz();
        frame.boostFromMidi = engine.boostFollowsMidi();
        frame.cutFromMidi = engine.cutFollowsMidi();
        frame.midiNoteOns = engine.midiNoteOnCount();
        frame.limiterGain = static_cast<float>(engine.limiterGain());
        const auto copyBands = [](const BandBank& bank, BandView* out) {
            int count = 0;
            for (int slot = 0; slot < kMaxSlots; ++slot)
                if (bank.slotActive(slot))
                    out[count++] = { bank.currentRootHz() * bank.slotRatio(slot), bank.slotGainDb(slot) };
            return count;
        };
        frame.numBoostBands = copyBands(engine.boostBank(), frame.boostBands);
        frame.numCutBands = copyBands(engine.cutBank(), frame.cutBands);
        displayWrite.store(w + 1, std::memory_order_release);
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = buffer.getMagnitude(juce::jmin(ch, numChannels - 1), 0, buffer.getNumSamples());
        float previous = outputPeak[ch].load(std::memory_order_relaxed);
        while (peak > previous && !outputPeak[ch].compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {}

    }

    // A full FIFO only means the editor is not drawing; dropping the block is fine.
    const float* left = buffer.getReadPointer(0);
    const float* right = buffer.getReadPointer(numChannels - 1);
    spectrumFifo.write(static_cast<size_t>(buffer.getNumSamples()), [=](size_t i) { return 0.5f * (left[i] + right[i]); });
}

double RootnoteProcessor::getTailLengthSeconds() const
{
    const double sampleRate = displaySampleRate.load(std::memory_order_relaxed);
    return Engine::tailSeconds(parameters.read(), sampleRate);
}

juce::AudioProcessorEditor* RootnoteProcessor::createEditor()
{
    return new RootnoteEditor(*this);
}

void RootnoteProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (const auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void RootnoteProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
    {
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
        stateRestored.store(true, std::memory_order_release);
        resetAbPending.store(true, std::memory_order_release);
    }
}

RootnoteProcessor::DisplayState RootnoteProcessor::displayState() noexcept
{
    size_t r = displayRead.load(std::memory_order_relaxed);
    const size_t w = displayWrite.load(std::memory_order_acquire);
    while (r != w)
    {
        lastDisplay = displayFrames[r++ % kDisplayFrames];
        haveDisplay = true;
    }
    displayRead.store(r, std::memory_order_release);
    if (haveDisplay)
        return lastDisplay;

    const EngineParams p = parameters.read();
    DisplayState state;
    state.sampleRate = displaySampleRate.load(std::memory_order_relaxed);
    state.boostHz = noteToHz(p.boostNote + p.boostFineCents / 100.0, p.a4);
    state.cutHz = noteToHz(p.cutNote, p.a4);
    state.numBoostBands = Engine::boostBands(p, state.boostHz, state.sampleRate, state.boostBands);
    state.numCutBands = Engine::cutBands(p, state.cutHz, state.sampleRate, state.cutBands);
    return state;
}

void RootnoteProcessor::setDisplayActive(bool active) noexcept
{
    if (active && displayUsers.load(std::memory_order_relaxed) == 0)
    {
        // Consumer-side discard: reopening starts with fresh audio, not old spectra.
        spectrumFifo.discard();
        displayRead.store(displayWrite.load(std::memory_order_acquire), std::memory_order_release);
        haveDisplay = false;
        for (auto& peak : outputPeak)
            peak.exchange(0.0f, std::memory_order_relaxed);
    }
    analysis.enable(active);
    displayUsers.fetch_add(active ? 1 : -1, std::memory_order_relaxed);
}

void RootnoteProcessor::setParameterValue(const char* id, double value)
{
    auto* parameter = apvts.getParameter(id);
    setNormalised(*parameter, parameter->convertTo0to1(static_cast<float>(value)));
}

int RootnoteProcessor::numPresets() noexcept
{
    return static_cast<int>(factoryPresets().size());
}

juce::String RootnoteProcessor::presetName(int index)
{
    return juce::isPositiveAndBelow(index, numPresets()) ? factoryPresets()[static_cast<size_t>(index)].name : "";
}

int RootnoteProcessor::currentPreset() const
{
    return juce::jlimit(0, numPresets() - 1, static_cast<int>(apvts.state.getProperty(kPreset, 0)));
}

void RootnoteProcessor::loadPreset(int index)
{
    if (!juce::isPositiveAndBelow(index, numPresets()))
        return;

    const Preset& preset = factoryPresets()[static_cast<size_t>(index)];
    for (auto* parameter : getParameters())
    {
        auto* ranged = static_cast<juce::RangedAudioParameter*>(parameter); // an APVTS only holds ranged parameters
        const juce::String id = ranged->getParameterID();
        if (presetKeeps(id.toRawUTF8()))
            continue;

        float value = ranged->getDefaultValue();
        for (const PresetValue& entry : preset.values)
            if (id == entry.id)
                value = ranged->convertTo0to1(entry.value);
        setNormalised(*ranged, value);
    }
    apvts.state.setProperty(kPreset, index, nullptr);
}

void RootnoteProcessor::resetAbIfRestored()
{
    if (resetAbPending.exchange(false, std::memory_order_acquire))
    {
        abSlots[0] = abSlots[1] = {};
        currentAbSlot = 0;
    }
}

int RootnoteProcessor::abSlot()
{
    resetAbIfRestored();
    return currentAbSlot;
}

void RootnoteProcessor::selectAbSlot(int slot)
{
    resetAbIfRestored();
    if (slot == currentAbSlot || !juce::isPositiveAndBelow(slot, 2))
        return;

    const auto& all = getParameters();
    AbSlot& leaving = abSlots[currentAbSlot];
    leaving.preset = currentPreset();
    leaving.values.resize(static_cast<size_t>(all.size()));
    for (int i = 0; i < all.size(); ++i)
        leaving.values[static_cast<size_t>(i)] = all[i]->getValue();

    currentAbSlot = slot;
    const AbSlot& entering = abSlots[slot];
    if (entering.values.empty()) // never left: it starts as a copy of the other slot
        return;
    for (int i = 0; i < all.size(); ++i)
        if (all[i] != bypassParameter)
            setNormalised(*all[i], entering.values[static_cast<size_t>(i)]);
    apvts.state.setProperty(kPreset, entering.preset, nullptr);
}

bool RootnoteProcessor::matchCutToHit()
{
    const TunerReading reading = analysis.reading();
    if (!reading.valid)
        return false;

    const EngineParams p = parameters.read();
    setParameterValue(ids::cutNote, juce::jlimit(kMinNote, kMaxNote, hzToNote(reading.hz, p.a4)));
    setParameterValue(ids::cutOn, 1.0);
    return true;
}

bool RootnoteProcessor::tuneBoostToKey()
{
    const TunerReading reading = analysis.reading();
    if (!reading.valid)
        return false;

    const EngineParams p = parameters.read();
    const double hitNote = juce::jlimit(kMinNote, kMaxNote, hzToNote(reading.hz, p.a4));
    setParameterValue(ids::boostNote, foldNoteIntoRange(snapToScale(hitNote, p.key, p.scale)));
    setParameterValue(ids::boostFine, 0.0);
    return true;
}

juce::Point<int> RootnoteProcessor::storedEditorSize() const
{
    return { apvts.state.getProperty(kEditorWidth, 0), apvts.state.getProperty(kEditorHeight, 0) };
}

void RootnoteProcessor::storeEditorSize(int width, int height)
{
    apvts.state.setProperty(kEditorWidth, width, nullptr);
    apvts.state.setProperty(kEditorHeight, height, nullptr);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RootnoteProcessor();
}
