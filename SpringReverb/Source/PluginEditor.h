#pragma once

#include "PluginProcessor.h"

class SpringTankEditor : public juce::AudioProcessorEditor
{
public:
    explicit SpringTankEditor(SpringTankProcessor&);
    ~SpringTankEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void drawScrew(juce::Graphics&, float x, float y) const;
    int headerHeight() const;

    SpringTankProcessor& processor;
    struct Content;
    std::unique_ptr<Content> content;
    juce::Image grain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpringTankEditor)
};
