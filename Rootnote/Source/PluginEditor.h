#pragma once

#include "PluginProcessor.h"

#include <juce_audio_utils/juce_audio_utils.h>

/**
    Placeholder editor for listening and testing: every parameter as a generic
    control, plus the live notes, the tuner reading and its two actions. The
    designed interface replaces this class.
*/
class RootnoteEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    static constexpr int kDefaultWidth = 520;
    static constexpr int kDefaultHeight = 640;

    explicit RootnoteEditor(RootnoteProcessor&);
    ~RootnoteEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    RootnoteProcessor& processor;
    juce::Label status;
    juce::TextButton matchCut { "Match cut" };
    juce::TextButton tuneToKey { "Tune to key" };
    juce::GenericAudioProcessorEditor controls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RootnoteEditor)
};
