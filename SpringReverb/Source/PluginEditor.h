#pragma once

#include "PluginProcessor.h"

class SpringTankEditor : public juce::AudioProcessorEditor,
                            private juce::ChangeListener
{
public:
    static constexpr int kWidth = 700;
    static constexpr int kHeight = 300;

    explicit SpringTankEditor(SpringTankProcessor&);
    ~SpringTankEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class LookAndFeel;
    class Knob;

    void detachLookAndFeel();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void syncPresetBox();

    SpringTankProcessor& processor;
    std::unique_ptr<LookAndFeel> lookAndFeel;
    juce::ComboBox presetBox;
    std::array<std::unique_ptr<Knob>, 7> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpringTankEditor)
};
