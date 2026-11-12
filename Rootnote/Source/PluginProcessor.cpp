#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"

#include <cmath>
#include <optional>

using namespace rootnote;

namespace
{
const juce::Identifier kStateType { "RootnoteState" };
const juce::Identifier kEditorWidth { "editorWidth" };
const juce::Identifier kEditorHeight { "editorHeight" };
const juce::Identifier kPreset { "preset" };
const juce::Identifier kSchema { "schema" };
const juce::Identifier kEditorPage { "editorPage" };
const juce::Identifier kAb { "AB" };
const juce::Identifier kActive { "active" };
const juce::Identifier kSlot { "SLOT" };
const juce::Identifier kIndex { "index" };

/** A number stored in the state, or nothing when it is missing, not a number or not finite. */
std::optional<double> storedNumber(const juce::ValueTree& tree, const juce::Identifier& name)
{
    const auto text = tree.getProperty(name).toString().trim();
    if (text.isEmpty() || !text.containsOnly("0123456789+-.eE") || !text.containsAnyOf("0123456789"))
        return {};
    const double value = text.getDoubleValue();
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

int storedInt(const juce::ValueTree& tree, const juce::Identifier& name, int low, int high, int fallback)
{
    const auto value = storedNumber(tree, name);
    return value ? static_cast<int>(juce::jlimit(static_cast<double>(low), static_cast<double>(high), *value)) : fallback;
}

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
      apvts(*this, nullptr, kStateType, createParameterLayout()),
      bypassParameter(dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter(ids::bypass)))
{
    for (const Preset& preset : factoryPresets())
    {
        auto& values = presetValues.emplace_back();
        for (auto* parameter : getParameters())
        {
            auto* ranged = static_cast<juce::RangedAudioParameter*>(parameter); // an APVTS only holds ranged parameters
            const juce::String id = ranged->getParameterID();
            float value = presetKeeps(id.toRawUTF8()) ? -1.0f : ranged->getDefaultValue();
            for (const PresetValue& entry : preset.values)
                if (id == entry.id)
                    value = ranged->convertTo0to1(entry.value);
            values.push_back(value);
        }
    }
}

void RootnoteProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (!(sampleRate > 0.0) || !std::isfinite(sampleRate))
        return; // no usable rate: stay unprepared, which passes the audio through
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
    const int numChannels = getTotalNumOutputChannels();
    if (getTotalNumInputChannels() == 1 && numChannels == 2)
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    if (!engine.isPrepared())
        return;

    float* const channels[2] = { buffer.getWritePointer(0), numChannels == 2 ? buffer.getWritePointer(1) : nullptr };
    analysis.push(channels[0], channels[1], numSamples);

    // A restored state plays its own notes, not a MIDI note left over from before.
    if (stateRestored.exchange(false, std::memory_order_acquire))
        engine.releaseMidi();
    const unsigned takeovers = knobTakeovers.exchange(0, std::memory_order_acquire);
    engine.setParams(parameters.read());
    engine.takeBackFromMidi(takeovers);

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

    auto& frame = displaySlots[audioSlot];
    frame.sampleRate = displaySampleRate.load(std::memory_order_relaxed);
    frame.boostHz = engine.boostHz();
    frame.cutHz = engine.cutHz();
    frame.boostNote = engine.boostTargetNote();
    frame.cutNote = engine.cutTargetNote();
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
    audioSlot = newestSlot.exchange(audioSlot | kFresh, std::memory_order_acq_rel) & 3;

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
    // Parameters in parameter order, so the same settings always give the same bytes.
    const auto live = apvts.copyState();
    juce::ValueTree state(kStateType);
    for (auto* parameter : getParameters())
        state.appendChild(live.getChildWithProperty("id", static_cast<juce::RangedAudioParameter*>(parameter)->getParameterID())
                              .createCopy(),
                          nullptr);
    state.setProperty(kSchema, kStateSchema, nullptr);
    state.setProperty(kPreset, currentPreset(), nullptr);
    const auto size = storedEditorSize();
    if (size.x > 0)
    {
        state.setProperty(kEditorWidth, size.x, nullptr);
        state.setProperty(kEditorHeight, size.y, nullptr);
    }
    state.setProperty(kEditorPage, storedEditorPage(), nullptr);

    const auto& all = getParameters();
    juce::ValueTree ab(kAb);
    {
        const std::lock_guard<std::mutex> lock(abMutex);
        ab.setProperty(kActive, currentAbSlot, nullptr);
        for (int slot = 0; slot < 2; ++slot)
        {
            if (abSlots[slot].values.empty())
                continue;
            juce::ValueTree child(kSlot);
            child.setProperty(kIndex, slot, nullptr);
            child.setProperty(kPreset, abSlots[slot].preset, nullptr);
            for (int i = 0; i < all.size(); ++i)
                child.setProperty(static_cast<juce::RangedAudioParameter*>(all[i])->getParameterID(),
                                  abSlots[slot].values[static_cast<size_t>(i)], nullptr);
            ab.appendChild(child, nullptr);
        }
    }
    state.appendChild(ab, nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void RootnoteProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // Everything is read and checked first, then applied: a damaged state changes nothing.
    // Stricter than getXmlFromBinary(), whose lenient parser makes an empty state of
    // a truncated one ("<RootnoteState pres..."), which would reset every setting.
    constexpr juce::uint32 kXmlMagic = 0x21324356; // the header of copyXmlToBinary()
    if (data == nullptr || sizeInBytes <= 8 || juce::ByteOrder::littleEndianInt(data) != kXmlMagic)
        return;
    const auto length = static_cast<int>(juce::ByteOrder::littleEndianInt(juce::addBytesToPointer(data, 4)));
    if (length <= 0 || length > sizeInBytes - 8)
        return; // truncated
    juce::XmlDocument document(juce::String::fromUTF8(static_cast<const char*>(data) + 8, length));
    const auto xml = document.getDocumentElement();
    if (xml == nullptr || document.getLastParseError().isNotEmpty() || !xml->hasTagName(kStateType))
        return;
    const auto stored = juce::ValueTree::fromXml(*xml);
    if (!stored.isValid() || !stored.getChildWithName("PARAM").isValid())
        return;
    // Every schema so far keeps the parameters as APVTS PARAM children, and a
    // newer one only adds to them: known fields are read, the rest ignored.

    // Parameters by ID: missing or unreadable ones take their default, the rest are clamped to the range.
    const auto& all = getParameters();
    juce::ValueTree restoredTree(kStateType);
    for (auto* parameter : all)
    {
        auto* ranged = static_cast<juce::RangedAudioParameter*>(parameter);
        const auto& range = ranged->getNormalisableRange();
        const auto value = storedNumber(stored.getChildWithProperty("id", ranged->getParameterID()), "value");
        const float plain = value ? juce::jlimit(range.start, range.end, static_cast<float>(*value))
                                  : ranged->convertFrom0to1(ranged->getDefaultValue());
        restoredTree.appendChild(juce::ValueTree("PARAM", { { "id", ranged->getParameterID() }, { "value", plain } }), nullptr);
    }

    // A/B: a slot counts only when it has a valid value for every parameter.
    AbSlot slots[2];
    const auto ab = stored.getChildWithName(kAb);
    for (const auto& child : ab)
    {
        const int index = storedInt(child, kIndex, -1, 2, -1);
        if (!child.hasType(kSlot) || !juce::isPositiveAndBelow(index, 2))
            continue;
        AbSlot slot;
        slot.preset = storedInt(child, kPreset, 0, numPresets() - 1, 0);
        for (auto* parameter : all)
        {
            const auto value = storedNumber(child, static_cast<juce::RangedAudioParameter*>(parameter)->getParameterID());
            if (!value || *value < 0.0 || *value > 1.0)
                break;
            slot.values.push_back(static_cast<float>(*value));
        }
        if (slot.values.size() == static_cast<size_t>(all.size()))
            slots[index] = std::move(slot);
    }

    presetIndex.store(storedInt(stored, kPreset, 0, numPresets() - 1, 0), std::memory_order_relaxed);
    storeEditorSize(storedInt(stored, kEditorWidth, 0, 16384, 0), storedInt(stored, kEditorHeight, 0, 16384, 0));
    storeEditorPage(storedInt(stored, kEditorPage, 0, 4, 0));
    {
        const std::lock_guard<std::mutex> lock(abMutex);
        abSlots[0] = std::move(slots[0]);
        abSlots[1] = std::move(slots[1]);
        currentAbSlot = storedInt(ab, kActive, 0, 1, 0);
    }
    apvts.replaceState(restoredTree);

    // The APVTS skips a parameter whose snapped value already matches, so a
    // stepped one the host left between steps (a switch at 0.86) would keep
    // that raw value although the state says 1. Settle every one.
    for (auto* parameter : all)
    {
        auto* ranged = static_cast<juce::RangedAudioParameter*>(parameter);
        const float restored = ranged->convertTo0to1(apvts.getRawParameterValue(ranged->getParameterID())->load());
        if (ranged->getValue() != restored)
            ranged->setValueNotifyingHost(restored);
    }
    stateRestored.store(true, std::memory_order_release);
}

RootnoteProcessor::DisplayState RootnoteProcessor::displayState() noexcept
{
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    if (newestSlot.load(std::memory_order_relaxed) & kFresh)
    {
        editorSlot = newestSlot.exchange(editorSlot, std::memory_order_acq_rel) & 3;
        lastFrameMs = juce::jmax(1u, now);
    }
    // Audio blocks come every few milliseconds; a host that stopped calling
    // must not freeze the bands, so the knobs take over after a short wait.
    if (lastFrameMs != 0 && now - lastFrameMs < kDisplayHoldMs)
        return displaySlots[editorSlot];

    const EngineParams p = parameters.read();
    DisplayState state;
    state.sampleRate = displaySampleRate.load(std::memory_order_relaxed);
    state.boostHz = noteToHz(p.boostNote + p.boostFineCents / 100.0, p.a4);
    state.cutHz = noteToHz(p.cutNote, p.a4);
    state.boostNote = p.boostNote;
    state.cutNote = p.cutNote;
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
        if (newestSlot.load(std::memory_order_relaxed) & kFresh)
            editorSlot = newestSlot.exchange(editorSlot, std::memory_order_acq_rel) & 3;
        lastFrameMs = 0;
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

void RootnoteProcessor::loadPreset(int index)
{
    if (juce::isPositiveAndBelow(index, numPresets()))
    {
        applyPreset(index, true);
        updateHostDisplay(ChangeDetails().withProgramChanged(true));
    }
}

void RootnoteProcessor::applyPreset(int index, bool asGesture)
{
    if (!juce::isPositiveAndBelow(index, numPresets()))
        return;

    const auto& values = presetValues[static_cast<size_t>(index)];
    const auto& all = getParameters();
    for (int i = 0; i < all.size(); ++i)
    {
        const float value = values[static_cast<size_t>(i)];
        if (value < 0.0f)
            continue;
        if (asGesture)
            setNormalised(*all[i], value);
        else if (all[i]->getValue() != value)
            all[i]->setValueNotifyingHost(value);
    }
    presetIndex.store(index, std::memory_order_relaxed);
}

int RootnoteProcessor::abSlot()
{
    const std::lock_guard<std::mutex> lock(abMutex);
    return currentAbSlot;
}

void RootnoteProcessor::selectAbSlot(int slot)
{
    const auto& all = getParameters();
    AbSlot entering;
    {
        const std::lock_guard<std::mutex> lock(abMutex);
        if (slot == currentAbSlot || !juce::isPositiveAndBelow(slot, 2))
            return;
        AbSlot& leaving = abSlots[currentAbSlot];
        leaving.preset = currentPreset();
        leaving.values.resize(static_cast<size_t>(all.size()));
        for (int i = 0; i < all.size(); ++i)
            leaving.values[static_cast<size_t>(i)] = all[i]->getValue();
        currentAbSlot = slot;
        entering = abSlots[slot];
    }

    if (entering.values.empty()) // never left: it starts as a copy of the other slot
        return;
    for (int i = 0; i < all.size(); ++i)
        if (all[i] != bypassParameter)
            setNormalised(*all[i], entering.values[static_cast<size_t>(i)]);
    presetIndex.store(entering.preset, std::memory_order_relaxed);
}

bool RootnoteProcessor::matchCutToHit()
{
    const TunerReading reading = analysis.reading();
    if (!reading.valid)
        return false;

    const EngineParams p = parameters.read();
    setKnobNote(NoteControl::cut, juce::jlimit(kMinNote, kMaxNote, hzToNote(reading.hz, p.a4)));
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
    setKnobNote(NoteControl::boost, foldNoteIntoRange(snapToScale(hitNote, p.key, p.scale)));
    setParameterValue(ids::boostFine, 0.0);
    return true;
}

void RootnoteProcessor::setKnobNote(NoteControl::Target bank, double note)
{
    setParameterValue(bank == NoteControl::boost ? ids::boostNote : ids::cutNote, note);
    // After the value: the audio thread that sees the request also reads the new note.
    knobTakeovers.fetch_or(bank == NoteControl::boost ? NoteControl::boostChanged : NoteControl::cutChanged,
                           std::memory_order_release);
}

juce::Point<int> RootnoteProcessor::storedEditorSize() const noexcept
{
    return { editorWidth.load(std::memory_order_relaxed), editorHeight.load(std::memory_order_relaxed) };
}

void RootnoteProcessor::storeEditorSize(int width, int height) noexcept
{
    editorWidth.store(width, std::memory_order_relaxed);
    editorHeight.store(height, std::memory_order_relaxed);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RootnoteProcessor();
}
