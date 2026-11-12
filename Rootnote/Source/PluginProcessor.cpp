#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace rootnote;

namespace
{
const juce::Identifier kEditorWidth { "editorWidth" };
const juce::Identifier kEditorHeight { "editorHeight" };

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

    engine.setParams(parameters.read());

    // Each MIDI event takes effect on its own sample: process up to it, apply it, carry on.
    int position = 0;
    for (const auto metadata : midi)
    {
        MidiEvent event;
        if (!toEngineEvent(metadata.getMessage(), event))
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
    boostHz.store(engine.boostHz(), std::memory_order_relaxed);
    cutHz.store(engine.cutHz(), std::memory_order_relaxed);
    boostFromMidi.store(engine.boostFollowsMidi(), std::memory_order_relaxed);
    cutFromMidi.store(engine.cutFollowsMidi(), std::memory_order_relaxed);
    midiNoteOns.store(engine.midiNoteOnCount(), std::memory_order_relaxed);
    limiterGain.store(static_cast<float>(engine.limiterGain()), std::memory_order_relaxed);

    if (displayUsers.load(std::memory_order_relaxed) <= 0)
        return;

    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = buffer.getMagnitude(juce::jmin(ch, numChannels - 1), 0, buffer.getNumSamples());
        if (peak > outputPeak[ch].load(std::memory_order_relaxed))
            outputPeak[ch].store(peak, std::memory_order_relaxed);
    }

    // A full FIFO only means the editor is not drawing; dropping the block is fine.
    const float* left = buffer.getReadPointer(0);
    const float* right = buffer.getReadPointer(numChannels - 1);
    spectrumFifo.write(static_cast<size_t>(buffer.getNumSamples()), [=](size_t i) { return 0.5f * (left[i] + right[i]); });
}

double RootnoteProcessor::getTailLengthSeconds() const
{
    const double sampleRate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
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
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

RootnoteProcessor::DisplayState RootnoteProcessor::displayState() const noexcept
{
    DisplayState state;
    state.boostHz = boostHz.load(std::memory_order_relaxed);
    state.cutHz = cutHz.load(std::memory_order_relaxed);
    state.boostFromMidi = boostFromMidi.load(std::memory_order_relaxed);
    state.cutFromMidi = cutFromMidi.load(std::memory_order_relaxed);
    state.midiNoteOns = midiNoteOns.load(std::memory_order_relaxed);
    state.limiterGain = limiterGain.load(std::memory_order_relaxed);
    return state;
}

void RootnoteProcessor::setDisplayActive(bool active) noexcept
{
    analysis.enable(active);
    displayUsers.fetch_add(active ? 1 : -1, std::memory_order_relaxed);
}

void RootnoteProcessor::setParameterValue(const char* id, double value)
{
    auto* parameter = apvts.getParameter(id);
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value)));
    parameter->endChangeGesture();
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
