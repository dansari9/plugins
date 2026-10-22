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
            .withLabel("s")
            .withStringFromValueFunction([](float value, int)
            {
                return String(value, 2) + " s";
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "tension", 1 },
        "Tension",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.28f,
        AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int)
            {
                return String(roundToInt(value * 100.0f));
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "tone", 1 },
        "Tone",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.58f,
        AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int)
            {
                return String(roundToInt(value * 100.0f));
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "drip", 1 },
        "Drip",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.70f,
        AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int)
            {
                return String(roundToInt(value * 100.0f));
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "predelay", 1 },
        "Pre-delay",
        NormalisableRange<float>(0.0f, 100.0f, 0.01f, 0.55f),
        0.0f,
        AudioParameterFloatAttributes()
            .withLabel("ms")
            .withStringFromValueFunction([](float value, int)
            {
                return String(value, 1) + " ms";
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "mix", 1 },
        "Mix",
        NormalisableRange<float>(0.0f, 1.0f, 0.001f),
        0.42f,
        AudioParameterFloatAttributes()
            .withLabel("%")
            .withStringFromValueFunction([](float value, int)
            {
                return String(roundToInt(value * 100.0f)) + " %";
            })));

    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID { "output", 1 },
        "Output",
        NormalisableRange<float>(-24.0f, 12.0f, 0.01f),
        0.0f,
        AudioParameterFloatAttributes()
            .withLabel("dB")
            .withStringFromValueFunction([](float value, int)
            {
                return String(value, 1) + " dB";
            })));

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
}

void SpringTankProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int channels = juce::jlimit(1, 2, getTotalNumOutputChannels());
    reverb.prepare(sampleRate, samplesPerBlock, channels);
    reverb.setParameters(readParams(), true);
}

void SpringTankProcessor::releaseResources()
{
    reverb.reset();
}

bool SpringTankProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainIn != mainOut)
        return false;

    return mainIn == juce::AudioChannelSet::mono()
        || mainIn == juce::AudioChannelSet::stereo();
}

void SpringTankProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    juce::ScopedNoDenormals noDenormals;

    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    if (!reverb.isPrepared())
        return;

    reverb.setParameters(readParams(), false);
    reverb.process(buffer);
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
    if (!juce::isPositiveAndBelow(index, kNumFactoryPresets))
        return;

    applyProgram(index);
    programBroadcaster.sendChangeMessage();
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
    const float decay = decayRaw != nullptr ? decayRaw->load() : 2.2f;
    const float preMs = predelayRaw != nullptr ? predelayRaw->load() : 0.0f;
    return static_cast<double>(decay) + static_cast<double>(preMs) * 0.001 + 0.05;
}

void SpringTankProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty("program", currentProgram.load(), nullptr);
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
            apvts.replaceState(state);
            programBroadcaster.sendChangeMessage();
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
