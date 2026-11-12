#include "PluginEditor.h"

using namespace rootnote;

namespace
{
constexpr int kHeaderHeight = 64;

juce::String describe(double hz, double a4)
{
    return juce::String(formatNote(hzToNote(hz, a4))) + "  " + juce::String(hz, 1) + " Hz";
}
} // namespace

RootnoteEditor::RootnoteEditor(RootnoteProcessor& p)
    : AudioProcessorEditor(p), processor(p), controls(p)
{
    status.setJustificationType(juce::Justification::centredLeft);
    status.setFont(juce::FontOptions(14.0f));
    addAndMakeVisible(status);
    addAndMakeVisible(matchCut);
    addAndMakeVisible(tuneToKey);
    addAndMakeVisible(controls);

    matchCut.onClick = [this] { processor.matchCutToHit(); };
    tuneToKey.onClick = [this] { processor.tuneBoostToKey(); };

    const auto stored = processor.storedEditorSize(); // read first: setting the limits already resizes
    setResizable(true, true);
    setResizeLimits(400, 320, 1200, 1400);
    setSize(stored.x > 0 ? stored.x : kDefaultWidth, stored.y > 0 ? stored.y : kDefaultHeight);

    processor.setDisplayActive(true);
    startTimerHz(15);
    timerCallback();
}

RootnoteEditor::~RootnoteEditor()
{
    processor.setDisplayActive(false);
}

void RootnoteEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0b0b0f));
}

void RootnoteEditor::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop(kHeaderHeight).reduced(8);
    auto buttons = header.removeFromRight(220);
    matchCut.setBounds(buttons.removeFromLeft(106).reduced(0, 10));
    tuneToKey.setBounds(buttons.removeFromRight(106).reduced(0, 10));
    status.setBounds(header);
    controls.setBounds(area);

    processor.storeEditorSize(getWidth(), getHeight());
}

void RootnoteEditor::timerCallback()
{
    const double a4 = static_cast<double>(processor.getApvts().getRawParameterValue(ids::a4)->load());
    const auto state = processor.displayState();
    const auto hit = processor.tunerReading();

    juce::String text;
    if (state.boostHz > 0.0)
        text << "Boost  " << describe(state.boostHz, a4) << (state.boostFromMidi ? "  (MIDI)" : "")
             << "      Cut  " << describe(state.cutHz, a4) << (state.cutFromMidi ? "  (MIDI)" : "") << "\n";
    text << "Hit  " << (hit.valid ? describe(hit.hz, a4) : juce::String("-"));

    status.setText(text, juce::dontSendNotification);
    matchCut.setEnabled(hit.valid);
    tuneToKey.setEnabled(hit.valid);
}
