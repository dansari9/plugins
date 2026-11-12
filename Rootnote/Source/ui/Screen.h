#pragma once

#include "PluginProcessor.h"
#include "analysis/Fft.h"
#include "ui/Pages.h"

/**
    The dark display: the boost note in large type, the status lines, the
    output spectrum with white boost and orange cut pillars, and the legend
    of the eight encoder values. 560 x 252 mockup pixels.
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

    /** A click on a legend value, with the encoder it belongs to. Elsewhere the screen ignores the mouse. */
    std::function<void(int encoder)> onValueClick;

    void paint(juce::Graphics&) override;
    bool hitTest(int x, int y) override;
    void mouseUp(const juce::MouseEvent&) override;

private:
    static constexpr int kFftOrder = 13;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kPoints = 261;
    static constexpr float kTopDb = 6.0f, kBottomDb = -70.0f;

    void analyse(double sampleRate);
    void buildPaths();
    void buildText(const TunerReading&);
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
    juce::Path line, area;

    // What the text was made from: every parameter (normalised) and the reading.
    std::vector<float> shownParameters;
    TunerReading shownReading;

    juce::String note, noteDetail, hitLine, seriesLine, cutLine, ringLine;
    bool hitValid = false;
};
} // namespace rootnote::ui
