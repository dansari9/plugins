#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
    The Faceplate look: colours and type of the design mockup (option 2).
    Every size in the editor is in mockup pixels; the editor scales the whole
    plate, so 1 unit is 1 CSS pixel of the mockup at the default size.
*/
namespace rootnote::ui
{
namespace colours
{
const juce::Colour face { 0xffd9d8d2 };
const juce::Colour key { 0xffecebe6 };
const juce::Colour darkKey { 0xff2a2a27 };
const juce::Colour ink { 0xff1a1a18 };
const juce::Colour sub { 0xff6e6d68 };
const juce::Colour orange { 0xffff5a1f };
const juce::Colour blue { 0xff2f6bff };
const juce::Colour grey { 0xff9a9993 };
const juce::Colour white { 0xfff6f5f0 };
const juce::Colour screen { 0xff0f100f };
const juce::Colour screenText { 0xffa5a59e };
const juce::Colour legendLabel { 0xff7a7a74 };
const juce::Colour axis { 0xff6b6b66 };
const juce::Colour rule { 0xff262624 };
const juce::Colour lcd { 0xffc9c8c1 };
const juce::Colour panel { 0xffcdccc5 };
const juce::Colour ledOff { 0xffb9b8b1 };
const juce::Colour outOfKey { 0xff5a5a55 };
} // namespace colours

constexpr int kPlateWidth = 960;
constexpr int kPlateHeight = 540;

enum class Weight { regular, medium, semiBold };

/** Geist Mono at a CSS font size in px, with CSS letter-spacing in em. */
juce::Font mono(float px, Weight weight = Weight::regular, float trackingEm = 0.02f);

/** The small uppercase labels: 9 px, wide tracking, grey. */
inline juce::Font labelFont() { return mono(9.0f, Weight::regular, 0.08f); }

float textWidth(const juce::Font& font, const juce::String& text);

/** Draws text left-aligned with its vertical centre at y; returns the x after it. */
float drawTextAt(juce::Graphics&, const juce::String& text, const juce::Font&, juce::Colour, float x, float centreY);

/** A key cap: drop line below, slightly darker bottom edge, rounded corners. */
void drawKeyBody(juce::Graphics&, juce::Rectangle<float> box, juce::Colour fill, float radius);

void drawLed(juce::Graphics&, juce::Point<float> centre, juce::Colour);

/** Popup menus and default text in the plate's colours and font. */
class FaceLookAndFeel : public juce::LookAndFeel_V4
{
public:
    FaceLookAndFeel();
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font&) override;
    juce::Font getPopupMenuFont() override { return mono(12.0f); }
};
} // namespace rootnote::ui
