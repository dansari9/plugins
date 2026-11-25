#include "Vibrate.h"

using namespace juce;

namespace
{
AudioProcessorValueTreeState::ParameterLayout parameterLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto snap = [](float low, float high, float value) {
        return jlimit(low, high, std::round(value * 10.0f) / 10.0f);
    };
    const NormalisableRange<float> percent(0.0f, 100.0f,
        [](float low, float high, float n) { return low + (high - low) * n; },
        [](float low, float high, float value) { return (value - low) / (high - low); },
        snap);
    const auto attributes = AudioParameterFloatAttributes()
        .withLabel("%")
        .withStringFromValueFunction([](float value, int) {
            return String(value, std::abs(value - std::round(value)) < 0.05f ? 0 : 1) + " %";
        })
        .withValueFromStringFunction([](const String& text) { return text.getFloatValue(); });
    for (std::size_t i = 0; i < vibrate::numControls; ++i)
    {
        const String id(vibrate::ids[i]);
        layout.add(std::make_unique<AudioParameterFloat>(ParameterID(id, 1), id, percent,
                   static_cast<float>(100.0 * vibrate::defaults[i]), attributes));
    }
    layout.add(std::make_unique<AudioParameterBool>(ParameterID("bypass", 1), "bypass", false));
    return layout;
}
}

VibrateProcessor::VibrateProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo(), true)
                                      .withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "VIBRATE", parameterLayout())
{
    for (std::size_t i = 0; i < parameters.size(); ++i)
        parameters[i] = apvts.getRawParameterValue(vibrate::ids[i]);
    setLatencySamples(0);
}

vibrate::Controls VibrateProcessor::controls() const
{
    vibrate::Controls values;
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = static_cast<double>(parameters[i]->load(std::memory_order_relaxed)) / 100.0;
    return values;
}

bool VibrateProcessor::bypassed() const { return parameters.back()->load(std::memory_order_relaxed) >= 0.5f; }

AudioProcessorParameter* VibrateProcessor::getBypassParameter() const { return apvts.getParameter("bypass"); }

bool VibrateProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    const auto input = layout.getMainInputChannelSet();
    return layout.getMainOutputChannelSet() == AudioChannelSet::stereo()
        && (input == AudioChannelSet::mono() || input == AudioChannelSet::stereo());
}

void VibrateProcessor::prepareToPlay(double rate, int)
{
    engine.prepare(rate, controls(), bypassed());
    reset();
}

void VibrateProcessor::processBlock(AudioBuffer<float>& audio, MidiBuffer&) { process(audio, false); }
void VibrateProcessor::processBlockBypassed(AudioBuffer<float>& audio, MidiBuffer&) { process(audio, true); }

void VibrateProcessor::process(AudioBuffer<float>& audio, bool hostBypass)
{
    const ScopedNoDenormals noDenormals;
    const bool bypass = hostBypass || bypassed();
    const auto values = controls();
    if (resetPending.exchange(false, std::memory_order_acq_rel))
        engine.reset(values, bypass);
    else
        engine.setControls(values, bypass);

    auto* left = audio.getWritePointer(0);
    auto* right = audio.getWritePointer(1);
    const bool stereo = getTotalNumInputChannels() == 2;
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto sample = engine.process(left[i], stereo ? right[i] : left[i]);
        left[i] = sample.left;
        right[i] = sample.right;
    }
}

void VibrateProcessor::getStateInformation(MemoryBlock& output)
{
    if (const auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, output);
}
void VibrateProcessor::setStateInformation(const void* data, int size)
{
    if (const auto xml = getXmlFromBinary(data, size); xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(ValueTree::fromXml(*xml));
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VibrateProcessor(); }
