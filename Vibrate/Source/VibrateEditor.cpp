#include "Vibrate.h"

juce::AudioProcessorEditor* VibrateProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor(*this);
}
