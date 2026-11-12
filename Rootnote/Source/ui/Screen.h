#pragma once

#include "PluginProcessor.h"
#include "analysis/Fft.h"
#include "ui/Pages.h"

/**
    The dark display: the boost note in large type, the status lines, the
    output spectrum with white boost and orange cut pillars under the blue
    Focus window, and the legend of the page's values. 560 x 252 mockup pixels.

    A click on a legend value opens a text field over it: Return applies the
    typed value as one gesture, focus loss too, Escape cancels; text that is
    no value leaves the parameter alone.
*/
namespace rootnote::ui
{
class Screen : public juce::Component
{
public:
    explicit Screen(RootnoteProcessor&);

    /** The page whose values the legend shows. */
    void setPage(int page);

    /** Pulls new spectrum samples and takes the frame; repaints what changed. Call once per UI frame. */
    void update(const RootnoteProcessor::DisplayState&, const TunerReading&);

    /** Opens the text field of a legend value (its slot on the page), with the value selected. */
    void showEntry(int slot);
    bool isShowingEntry() const { return entry.isVisible(); }

    /** Closes the text field without applying it. */
    void cancelEntry() { closeEntry(false); }

    /**
        The boost's Focus window as drawn, in screen x: its centre (the focus
        frequency, or the boost note when linked) and centre -+ Range octaves.
        Not visible while the boost gain is 0.
    */
    struct FocusMarks
    {
        float centre = 0.0f, low = 0.0f, high = 0.0f;
        bool linked = false, visible = false;
    };
    FocusMarks focusMarks() const { return focus; }

    void paint(juce::Graphics&) override;
    bool hitTest(int x, int y) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

private:
    static constexpr int kFftOrder = 13;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kPoints = 261;
    static constexpr float kTopDb = 6.0f, kBottomDb = -70.0f;

    void analyse(double sampleRate);
    void buildPaths();
    void buildText(const TunerReading&);
    void buildFocus();
    void closeEntry(bool apply);
    void enterValue(const Slot&, const juce::String& text);
    int legendSlotAt(int x) const;
    bool sameFrame(const RootnoteProcessor::DisplayState&) const;

    RootnoteProcessor& processor;
    RootnoteProcessor::DisplayState state;
    int page = 0;

    Fft fft { kFftOrder };
    std::vector<double> window = std::vector<double>(kFftSize); // Hann
    std::vector<float> recent = std::vector<float>(kFftSize, 0.0f); // circular
    std::vector<double> re = std::vector<double>(kFftSize), im = std::vector<double>(kFftSize);
    size_t writeAt = 0;
    float spectrumDb[kPoints];
    juce::Path line, area, focusLine, focusArea;
    FocusMarks focus;
    juce::String focusLabel;

    juce::TextEditor entry;
    int entrySlot = -1;
    int hoverSlot = -1;
    juce::Component::SafePointer<juce::Component> focusBeforeEntry;

    // What the text was made from: every parameter (normalised) and the reading.
    std::vector<float> shownParameters;
    TunerReading shownReading;

    juce::String note, noteDetail, hitLine, seriesLine, cutLine, ringLine;
    bool hitValid = false;
};
} // namespace rootnote::ui
