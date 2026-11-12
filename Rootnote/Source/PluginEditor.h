#pragma once

#include "PluginProcessor.h"
#include "ui/Screen.h"
#include "ui/Widgets.h"

/**
    The Faceplate editor: a light hardware panel with one dark screen, twelve
    pitch keys, function and page keys and eight encoders that change role
    with the page.

    It is a thin layer: every control reads and writes the parameters in the
    APVTS, and everything live comes from the processor's editor interface.
    The plate is laid out in the mockup's 960 x 540 pixels and scaled as a
    whole, so the editor keeps its aspect ratio when resized.
*/
class RootnoteEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    static constexpr int kDefaultWidth = rootnote::ui::kPlateWidth;
    static constexpr int kDefaultHeight = rootnote::ui::kPlateHeight;

    explicit RootnoteEditor(RootnoteProcessor&);
    ~RootnoteEditor() override;

    void resized() override;

    /** Brings everything on screen up to date. The timer calls it 30 times a second. */
    void refresh();

    /** Shows a page (0 boost, 1 cut, 2 ring, 3 out, 4 midi) on the encoders and the legend. */
    void selectPage(int page);

    /** Holding a page key this long only shows its page on the display; a shorter press selects it. */
    static constexpr juce::uint32 kPagePeekMs = 350;

private:
    struct Plate : juce::Component
    {
        explicit Plate(RootnoteEditor& e) : editor(e) {}
        void paint(juce::Graphics& g) override { editor.paintPlate(g); }
        RootnoteEditor& editor;
    };

    void timerCallback() override { refresh(); }
    void paintPlate(juce::Graphics&);
    void paintFooter(juce::Graphics&);

    /** Everything the footer shows; it is repainted only when this changes. */
    struct Footer
    {
        int midiMode = 0, meterLeds = 0;
        bool boostLit = false, cutLit = false, hold = true, constantQ = true;
        double glide = 0.0, mix = 0.0, output = 0.0, a4 = 0.0;
        auto tied() const { return std::tie(midiMode, meterLeds, boostLit, cutLit, hold, constantQ, glide, mix, output, a4); }
        bool operator!=(const Footer& other) const { return tied() != other.tied(); }
    };
    Footer footer;
    void bindEncoders();
    void setParameter(const char* id, double value);
    void toggleParameter(const char* id);
    double parameter(const char* id) const;
    void showSettings();
    void showMenu(juce::PopupMenu, juce::Component& target, std::function<void(int)> onChosen);
    void pageKeyChanged(int index);
    void place(juce::Component&, float x, float y, float w, float h);

    RootnoteProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    rootnote::ui::FaceLookAndFeel lookAndFeel;
    Plate plate { *this };

    rootnote::ui::PresetDisplay preset;
    rootnote::ui::Key slotA { "A" }, slotB { "B" };
    rootnote::ui::RoundButton settings { rootnote::ui::RoundButton::Icon::gear };
    rootnote::ui::RoundButton power { rootnote::ui::RoundButton::Icon::power };
    rootnote::ui::Screen screen;

    juce::OwnedArray<rootnote::ui::Key> pitchKeys, pageKeys, seriesKeys;
    rootnote::ui::Key octaveDown { juce::String::fromUTF8("oct \xe2\x88\x92") }, octaveUp { "oct +" };
    rootnote::ui::Key matchCut { "match cut" }, tuneToKey { "tune to key" }, keyLock { "key lock" };
    rootnote::ui::Key link { "link" }, limit { "limit" };

    rootnote::ui::Encoder encoders[rootnote::ui::kNumEncoders];
    std::unique_ptr<rootnote::ui::EncoderBinding> bindings[rootnote::ui::kNumEncoders]; // after the encoders

    int page = 0;
    int peekKey = -1;        // the page key held down, if any
    juce::uint32 peekStartMs = 0;
    bool peeked = false;     // the last press was a hold: its click selects nothing
    int octave = 1;          // octave of the pitch keys
    float meterDb = -100.0f;
    uint32_t lastNoteOns = 0;
    int midiLit = 0;         // frames the MIDI dots stay lit after a note

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RootnoteEditor)
};
