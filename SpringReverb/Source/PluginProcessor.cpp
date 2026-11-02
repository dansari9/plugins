#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "decay", 1 },
        "Decay",
        NormalisableRange<float>(0.15f, 5.5f, 0.001f, 0.45f),
        2.2f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float value, int)
            {
                return String(value, 2) + " s";
            })));

    const auto pct = [](float value, int)
    {
        return String(roundToInt(value * 100.0f)) + " %";
    };
    const auto fromPct = [](const String& text)
    {
        return text.retainCharacters("0123456789.-").getFloatValue() / 100.0f;
    };
    const auto percent = AudioParameterFloatAttributes()
                             .withStringFromValueFunction(pct)
                             .withValueFromStringFunction(fromPct);

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "tension", 1 },
        "Tension",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.28f,
        percent));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "tone", 1 },
        "Tone",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.58f,
        percent));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "drip", 1 },
        "Drip",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.70f,
        percent));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "predelay", 1 },
        "Pre-delay",
        NormalisableRange<float>(0.0f, 100.0f, 0.01f, 0.55f),
        0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float value, int)
            {
                return String(value, 1) + " ms";
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "mix", 1 },
        "Mix",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.42f,
        percent));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "output", 1 },
        "Output",
        NormalisableRange<float>(-24.0f, 12.0f, 0.01f),
        0.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float value, int)
            {
                return String(value, 1) + " dB";
            })));

    layout.add(std::make_unique<AudioParameterBool>(
        ParameterID { "bypass", 1 },
        "Bypass",
        false));

    return layout;
}

struct FactoryPreset
{
    const char* name;
    float decay;
    float tension;
    float tone;
    float drip;
    float preDelayMs;
    float mix;
    float outputDb;
};

// Surf Guitar matches the parameter defaults, so a new instance is already on program 0.
constexpr FactoryPreset kFactoryPresets[] = {
    { "Surf Guitar",          2.2f, 0.28f, 0.58f, 0.70f,  0.0f, 0.42f,  0.0f },
    { "Dub Splash",           5.0f, 0.16f, 0.32f, 0.96f, 48.0f, 0.72f, -3.0f },
    { "Subtle Room Spring",   0.85f, 0.76f, 0.46f, 0.20f,  7.0f, 0.20f,  0.0f },
    { "Vocal Shimmer",        2.9f, 0.40f, 0.84f, 0.46f, 20.0f, 0.36f, -1.5f },
};

constexpr int kNumFactoryPresets = static_cast<int>(sizeof(kFactoryPresets) / sizeof(kFactoryPresets[0]));

void setPlainValue(juce::AudioProcessorValueTreeState& apvts, const char* id, float plain)
{
    if (auto* parameter = apvts.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
}
} // namespace

SpringTankProcessor::SpringTankProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createLayout())
{
    decayRaw = apvts.getRawParameterValue("decay");
    tensionRaw = apvts.getRawParameterValue("tension");
    toneRaw = apvts.getRawParameterValue("tone");
    dripRaw = apvts.getRawParameterValue("drip");
    predelayRaw = apvts.getRawParameterValue("predelay");
    mixRaw = apvts.getRawParameterValue("mix");
    outputRaw = apvts.getRawParameterValue("output");
    bypassRaw = apvts.getRawParameterValue("bypass");
}

void SpringTankProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int channels = juce::jlimit(1, 2, getTotalNumOutputChannels());
    reverb.prepare(sampleRate, samplesPerBlock, channels);
    reverb.setParameters(readParams(), true);
    dryBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, false, true);
    bypassMix.reset(sampleRate, 0.02);
    const bool bypassed = bypassRaw != nullptr && bypassRaw->load() >= 0.5f;
    bypassMix.setCurrentAndTargetValue(bypassed ? 0.0f : 1.0f);
}

void SpringTankProcessor::releaseResources()
{
    reverb.reset();
}

bool SpringTankProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();
    if (out != mono && out != stereo)
        return false;
    return in == out || (in == mono && out == stereo);
}

void storeMax(std::atomic<float>& slot, float value) noexcept
{
    float previous = slot.load(std::memory_order_relaxed);
    while (previous < value
           && !slot.compare_exchange_weak(previous, value, std::memory_order_relaxed))
    {
    }
}

float takePeak(std::atomic<float>& slot) noexcept
{
    return slot.exchange(0.0f, std::memory_order_relaxed);
}

void SpringTankProcessor::storePeaks(const juce::AudioBuffer<float>& buffer, std::atomic<float>* dest) noexcept
{
    const int numSamples = buffer.getNumSamples();
    for (int ch = 0; ch < 2; ++ch)
    {
        float peak = 0.0f;
        if (ch < buffer.getNumChannels() && numSamples > 0)
            peak = buffer.getMagnitude(ch, 0, numSamples);
        storeMax(dest[ch], peak);
    }
}

float SpringTankProcessor::takeInputPeak(int channel) noexcept
{
    return channel >= 0 && channel < 2 ? takePeak(inputMeter[channel]) : 0.0f;
}

float SpringTankProcessor::takeOutputPeak(int channel) noexcept
{
    return channel >= 0 && channel < 2 ? takePeak(outputMeter[channel]) : 0.0f;
}

void SpringTankProcessor::setEditorWidth(int width) noexcept
{
    editorWidth.store(juce::jlimit(630, 1680, width));
}

juce::AudioProcessorParameter* SpringTankProcessor::getBypassParameter() const
{
    return apvts.getParameter("bypass");
}

void SpringTankProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int channels = juce::jmin(2, buffer.getNumChannels());
    if (numIn == 1 && channels > 1)
        buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);
    else
        for (int ch = numIn; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, numSamples);

    storePeaks(buffer, inputMeter);
    hostBypassed = false;

    if (!reverb.isPrepared() || numSamples <= 0 || dryBuffer.getNumSamples() <= 0)
    {
        storePeaks(buffer, outputMeter);
        return;
    }

    reverb.setParameters(readParams(), false);
    bypassMix.setTargetValue(bypassRaw != nullptr && bypassRaw->load() >= 0.5f ? 0.0f : 1.0f);

    const int maxChunk = dryBuffer.getNumSamples();
    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int n = juce::jmin(maxChunk, numSamples - start);
        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), channels, start, n);
        for (int ch = 0; ch < channels; ++ch)
            dryBuffer.copyFrom(ch, 0, view, ch, 0, n);

        reverb.process(view);

        float* wetPtr[2] = { view.getWritePointer(0), channels > 1 ? view.getWritePointer(1) : nullptr };
        const float* dryPtr[2] = { dryBuffer.getReadPointer(0),
                                   channels > 1 ? dryBuffer.getReadPointer(1) : nullptr };
        for (int i = 0; i < n; ++i)
        {
            const float wet = bypassMix.getNextValue();
            for (int ch = 0; ch < channels; ++ch)
                wetPtr[ch][i] = dryPtr[ch][i] + (wetPtr[ch][i] - dryPtr[ch][i]) * wet;
        }
    }

    storePeaks(buffer, outputMeter);
}

void SpringTankProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    if (getTotalNumInputChannels() == 1 && buffer.getNumChannels() > 1)
        buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
    storePeaks(buffer, inputMeter);
    storePeaks(buffer, outputMeter);
    if (!hostBypassed)
    {
        reverb.reset();
        hostBypassed = true;
    }
    bypassMix.setCurrentAndTargetValue(0.0f);
}

juce::AudioProcessorEditor* SpringTankProcessor::createEditor()
{
    return new SpringTankEditor(*this);
}

int SpringTankProcessor::getNumPrograms()
{
    return kNumFactoryPresets;
}

int SpringTankProcessor::getCurrentProgram()
{
    return currentProgram.load();
}

void SpringTankProcessor::setCurrentProgram(int index)
{
    if (juce::isPositiveAndBelow(index, kNumFactoryPresets))
        applyProgram(index);
}

void SpringTankProcessor::selectProgramFromEditor(int index)
{
    if (!juce::isPositiveAndBelow(index, kNumFactoryPresets))
        return;

    static constexpr const char* ids[] = { "decay", "tension", "tone", "drip", "predelay", "mix", "output" };
    for (auto* id : ids)
        if (auto* parameter = apvts.getParameter(id))
            parameter->beginChangeGesture();

    applyProgram(index);

    for (auto* id : ids)
        if (auto* parameter = apvts.getParameter(id))
            parameter->endChangeGesture();

    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}.withProgramChanged(true));
}

const juce::String SpringTankProcessor::getProgramName(int index)
{
    if (!juce::isPositiveAndBelow(index, kNumFactoryPresets))
        return {};

    return kFactoryPresets[index].name;
}

void SpringTankProcessor::applyProgram(int index)
{
    const auto& preset = kFactoryPresets[index];
    currentProgram.store(index);
    setPlainValue(apvts, "decay", preset.decay);
    setPlainValue(apvts, "tension", preset.tension);
    setPlainValue(apvts, "tone", preset.tone);
    setPlainValue(apvts, "drip", preset.drip);
    setPlainValue(apvts, "predelay", preset.preDelayMs);
    setPlainValue(apvts, "mix", preset.mix);
    setPlainValue(apvts, "output", preset.outputDb);
}

double SpringTankProcessor::getTailLengthSeconds() const
{
    return 5.5 + 0.1 + 0.4;
}

void SpringTankProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty("program", currentProgram.load(), nullptr);
    state.setProperty("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void SpringTankProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (xml->hasTagName(apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml(*xml);
            const int stored = static_cast<int>(state.getProperty("program", 0));
            currentProgram.store(juce::jlimit(0, kNumFactoryPresets - 1, stored));
            setEditorWidth(static_cast<int>(state.getProperty("editorWidth", 840)));
            apvts.replaceState(state);
        }
    }
}

SpringReverb::Params SpringTankProcessor::readParams() const
{
    SpringReverb::Params p;
    p.decaySeconds = decayRaw->load();
    p.tension = tensionRaw->load();
    p.tone = toneRaw->load();
    p.drip = dripRaw->load();
    p.preDelayMs = predelayRaw->load();
    p.mix = mixRaw->load();
    p.outputDb = outputRaw->load();
    return p;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SpringTankProcessor();
}
