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

double SpringTankProcessor::getTailLengthSeconds() const
{
    const float decay = decayRaw != nullptr ? decayRaw->load() : 2.2f;
    const float preMs = predelayRaw != nullptr ? predelayRaw->load() : 0.0f;
    return static_cast<double>(decay) + static_cast<double>(preMs) * 0.001 + 0.05;
}

void SpringTankProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void SpringTankProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
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
