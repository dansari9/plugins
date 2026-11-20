#pragma once
#include "../presets/PresetStore.h"
#include "Style.h"

class SnapProcessor;

// The preset browser overlay: 1392 x 434 over the six module columns (top bar, amount row and master stay live).
// Four panes: sources, filter chips, the list (search, module filter, rows with chain glyphs and favourite hearts) and
// info (name, tags, description, numbered chain, locks, cancel / ok). Clicking a row (or up / down) auditions it; ok
// keeps it, cancel undoes everything since the browser opened (auditions and live edits) without adding history.
// Message thread only.
namespace snap::ui
{
class PresetBrowser : public juce::Component
{
public:
    // audition loads a preset (the editor records it as the current one).
    PresetBrowser(SnapProcessor&, PresetStore&, std::function<void(const Preset&)> audition);
    ~PresetBrowser() override;
    void open(const juce::String& currentKey); // remembers the state to return to on cancel, selects the current preset
    void finish(bool cancel);                  // ok (the audition stays) or cancel (back to the state at open)
    std::function<void()> onClose;             // after ok or cancel; the editor hides the overlay

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace snap::ui
