#include "Theme.h"

#include "BinaryData.h"

namespace rootnote::ui
{
namespace
{
juce::Typeface::Ptr typeface(Weight weight)
{
    // Loaded once, on first use, and shared by every editor.
    static const juce::Typeface::Ptr faces[] = {
        juce::Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
        juce::Typeface::createSystemTypefaceFor(BinaryData::GeistMonoMedium_ttf, BinaryData::GeistMonoMedium_ttfSize),
        juce::Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize),
    };
    return faces[static_cast<int>(weight)];
}
} // namespace

juce::Font mono(float px, Weight weight, float trackingEm)
{
    juce::Font font { juce::FontOptions(typeface(weight)).withPointHeight(px) };
    // JUCE's kerning factor is relative to the font height, CSS letter-spacing to the em.
    font.setExtraKerningFactor(trackingEm * font.getHeightToPointsFactor());
    return font;
}

float textWidth(const juce::Font& font, const juce::String& text)
{
    return juce::GlyphArrangement::getStringWidth(font, text);
}

float drawTextAt(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour colour, float x,
                 float centreY)
{
    const float width = textWidth(font, text);
    g.setFont(font);
    g.setColour(colour);
    g.drawText(text, juce::Rectangle<float>(x, centreY - font.getHeight() * 0.5f, width + 2.0f, font.getHeight()),
               juce::Justification::centredLeft, false);
    return x + width;
}

void drawKeyBody(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour fill, float radius)
{
    g.setColour(juce::Colours::black.withAlpha(0.18f));
    g.fillRoundedRectangle(box.translated(0.0f, 1.0f), radius);
    g.setColour(fill.interpolatedWith(juce::Colours::black, 0.06f));
    g.fillRoundedRectangle(box, radius);
    g.setColour(fill);
    g.fillRoundedRectangle(box.withTrimmedBottom(2.0f), radius);
}

void drawLed(juce::Graphics& g, juce::Point<float> centre, juce::Colour colour)
{
    g.setColour(colour);
    g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(centre));
}

FaceLookAndFeel::FaceLookAndFeel()
{
    setColour(juce::PopupMenu::backgroundColourId, colours::key);
    setColour(juce::PopupMenu::textColourId, colours::ink);
    setColour(juce::PopupMenu::headerTextColourId, colours::sub);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, colours::ink);
    setColour(juce::PopupMenu::highlightedTextColourId, colours::white);
    setColour(juce::TooltipWindow::backgroundColourId, colours::ink);
    setColour(juce::TooltipWindow::textColourId, colours::white);
}

juce::Typeface::Ptr FaceLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    // Fonts without a typeface of their own (menus, tooltips) use the plate's.
    if (font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        return typeface(Weight::regular);
    return juce::LookAndFeel_V4::getTypefaceForFont(font);
}
} // namespace rootnote::ui
