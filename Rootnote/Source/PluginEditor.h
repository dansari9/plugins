#pragma once

#include "PluginProcessor.h"
#include "analysis/Fft.h"

#include <juce_audio_utils/juce_audio_utils.h>

/**
    Plain plot of what the processor feeds an editor: the output spectrum, the
    boost bands (up) and cut bands (down) at their live frequencies, the
    output peaks and the limiter's gain reduction.
*/
class FeedView : public juce::Component
{
public:
    explicit FeedView(RootnoteProcessor&);

    /** Pulls the feed. Returns nothing; repaints itself. */
    void refresh(const RootnoteProcessor::DisplayState&);
    void paint(juce::Graphics&) override;

private:
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kPoints = 160;
    static constexpr float kFloorDb = -90.0f;

    void analyse();

    RootnoteProcessor& processor;
    RootnoteProcessor::DisplayState state;
    rootnote::Fft fft { kFftOrder };
    std::vector<float> recent = std::vector<float>(kFftSize, 0.0f); // circular
    std::vector<double> re = std::vector<double>(kFftSize), im = std::vector<double>(kFftSize);
    size_t writeAt = 0;
    float spectrumDb[kPoints];
    float peakDb[2] { kFloorDb, kFloorDb };
    juce::Path curve;
};

/**
    A working editor without a design: presets and A/B, the live notes and the
    tuner reading with its two actions, the feed plot, and every parameter as
    a generic control.

    It is deliberately thin. Everything it shows comes from the parameters and
    from the "Editor interface" section of RootnoteProcessor, so a designed
    editor can replace this class without touching anything else.
*/
class RootnoteEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    static constexpr int kDefaultWidth = 560;
    static constexpr int kDefaultHeight = 760;

    explicit RootnoteEditor(RootnoteProcessor&);
    ~RootnoteEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Brings everything on screen up to date. The timer calls it 30 times a second. */
    void refresh();

private:
    void timerCallback() override { refresh(); }

    RootnoteProcessor& processor;
    juce::ComboBox presets;
    juce::TextButton previous { "<" }, next { ">" };
    juce::TextButton slotA { "A" }, slotB { "B" };
    juce::Label status;
    juce::TextButton matchCut { "Match cut" };
    juce::TextButton tuneToKey { "Tune to key" };
    FeedView feed;
    juce::GenericAudioProcessorEditor controls;
    uint32_t lastNoteOns = 0;
    int midiLit = 0; // frames the MIDI mark stays on

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RootnoteEditor)
};
