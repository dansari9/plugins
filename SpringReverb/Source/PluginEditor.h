#pragma once

#include "PluginProcessor.h"

/**
    The only editor Spring Tank creates. Logic's window title is the plugin
    name; the view itself is this faceplate, never a text-only placeholder.
*/
class SpringTankEditor : public juce::AudioProcessorEditor,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    static constexpr int kWidth = 840;
    static constexpr int kHeight = 420;

    explicit SpringTankEditor(SpringTankProcessor&);
    ~SpringTankEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class LookAndFeel;
    class Knob;
    class Meter;

    void detachLookAndFeel();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void timerCallback() override;
    void syncPresetBox();
    void stepPreset(int delta);
    float uiScale() const noexcept;

    float phase = 0.0f;

    SpringTankProcessor& processor;
    juce::ComponentBoundsConstrainer constrainer;
    std::unique_ptr<LookAndFeel> lookAndFeel;
    juce::ComboBox presetBox;
    juce::TextButton presetPrev { "<" };
    juce::TextButton presetNext { ">" };
    juce::ToggleButton power;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment;
    std::array<std::unique_ptr<Knob>, 7> knobs;
    Meter* inputMeter = nullptr;
    Meter* outputMeter = nullptr;
    std::unique_ptr<Meter> inputMeterOwned;
    std::unique_ptr<Meter> outputMeterOwned;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpringTankEditor)
};
