#include "Widgets.h"

namespace rootnote::ui
{
Key::Key(const juce::String& t, float fontPx, float trackingEm)
    : juce::Button(t), text(t), font(mono(fontPx, Weight::regular, trackingEm))
{
    setTitle(t);
}

void Key::setLook(Look newLook)
{
    if (look != newLook)
    {
        look = newLook;
        repaint();
    }
}

void Key::setLed(Led newLed)
{
    if (led != newLed)
    {
        led = newLed;
        repaint();
    }
}

void Key::setUnderline(std::optional<juce::Colour> colour)
{
    if (underline != colour)
    {
        underline = colour;
        repaint();
    }
}

void Key::setText(const juce::String& newText)
{
    if (text != newText)
    {
        text = newText;
        repaint();
    }
}

void Key::clicked(const juce::ModifierKeys& modifiers)
{
    if (onPress)
        onPress(modifiers);
}

void Key::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto box = getLocalBounds().toFloat().withTrimmedBottom(1.0f);
    juce::Colour fill = colours::key, textColour = colours::ink;
    switch (look)
    {
        case Look::dark:     fill = colours::darkKey; textColour = colours::white; break;
        case Look::filled:   fill = colours::orange; textColour = colours::white; break;
        case Look::lcd:      fill = colours::lcd; break;
        case Look::lcdOn:    fill = colours::ink; textColour = colours::white; break;
        case Look::light:
        case Look::outlined: break;
    }
    if (down)
        fill = fill.interpolatedWith(juce::Colours::black, 0.08f);
    else if (over)
        fill = fill.interpolatedWith(look == Look::dark ? juce::Colours::white : juce::Colours::black, 0.03f);

    if (look == Look::lcd || look == Look::lcdOn) // the A/B keys sit flush in the header
    {
        g.setColour(fill);
        g.fillRoundedRectangle(box, 6.0f);
    }
    else
    {
        drawKeyBody(g, box, fill, 8.0f);
    }
    if (look == Look::outlined)
    {
        g.setColour(colours::orange);
        g.drawRoundedRectangle(box.reduced(1.0f), 7.0f, 2.0f);
    }

    g.setFont(font);
    g.setColour(textColour);
    if (underline.has_value())
    {
        // Label, 6 px gap, 14 x 2 bar, centred as one column.
        const float top = box.getCentreY() - 10.5f;
        g.drawText(text, box.withY(top).withHeight(13.0f), juce::Justification::centred, false);
        g.setColour(*underline);
        g.fillRoundedRectangle(juce::Rectangle<float>(14.0f, 2.0f).withCentre({ box.getCentreX(), top + 20.0f }), 1.0f);
    }
    else
    {
        g.drawText(text, box, juce::Justification::centred, false);
    }

    if (led != Led::none)
        drawLed(g, { box.getRight() - 8.5f, box.getY() + 8.5f }, led == Led::on ? colours::orange : colours::ledOff);
}

Encoder::Encoder()
    : juce::Slider(juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox)
{
    setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
}

void Encoder::setCap(juce::Colour colour, const juce::String& newLabel)
{
    cap = colour;
    label = newLabel;
    setTitle(newLabel);
    repaint();
}

void Encoder::paint(juce::Graphics& g)
{
    const juce::Point<float> centre { getWidth() * 0.5f, 30.0f };
    const auto ring = juce::Rectangle<float>(60.0f, 60.0f).withCentre(centre);
    const auto top = juce::Rectangle<float>(36.0f, 36.0f).withCentre(centre);

    // Soft shadow under the ring (0 2px 3px), then each disc with a darker bottom edge.
    for (int i = 0; i < 3; ++i)
    {
        g.setColour(juce::Colours::black.withAlpha(0.06f));
        g.fillEllipse(ring.expanded(1.5f - static_cast<float>(i)).translated(0.0f, 2.0f));
    }
    const auto disc = [&g](juce::Rectangle<float> r, juce::Colour fill, float edge) {
        g.setColour(fill.interpolatedWith(juce::Colours::black, edge));
        g.fillEllipse(r);
        g.setColour(fill);
        g.fillEllipse(r.withTrimmedBottom(2.0f).withTrimmedTop(0.5f).reduced(0.3f, 0.0f));
    };
    disc(ring, colours::key, 0.06f);
    disc(top, cap, 0.12f);

    const double proportion = valueToProportionOfLength(getValue());
    const float angle = juce::degreesToRadians(135.0f + static_cast<float>(proportion) * 270.0f);
    const juce::Point<float> dot { centre.x + 20.0f * std::cos(angle), centre.y + 20.0f * std::sin(angle) };
    g.setColour(colours::ink);
    g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(dot));

    g.setFont(labelFont());
    g.setColour(colours::sub);
    g.drawText(label.toUpperCase(), juce::Rectangle<float>(0.0f, 66.0f, static_cast<float>(getWidth()), 12.0f),
               juce::Justification::centred, false);
}

void Encoder::mouseDown(const juce::MouseEvent& e)
{
    juce::Slider::mouseDown(e); // starts the gesture
    dragProportion = valueToProportionOfLength(getValue());
    lastY = e.position.y;
}

void Encoder::mouseDrag(const juce::MouseEvent& e)
{
    if (!isEnabled())
        return;
    // Relative, so Shift can change the speed mid-drag without a jump: 200 px for the full range.
    const double pixels = static_cast<double>(lastY - e.position.y);
    lastY = e.position.y;
    dragProportion = juce::jlimit(0.0, 1.0, dragProportion + pixels / (e.mods.isShiftDown() ? 2000.0 : 200.0));
    double value = proportionOfLengthToValue(dragProportion);
    if (snapToInteger)
        value = std::round(value);
    setValue(value, juce::sendNotificationSync);
}

RoundButton::RoundButton(Icon i)
    : juce::Button(i == Icon::gear ? "Settings" : "On"), icon(i)
{
}

void RoundButton::setLit(bool on)
{
    if (lit != on)
    {
        lit = on;
        repaint();
    }
}

void RoundButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto box = getLocalBounds().toFloat();
    juce::Colour fill = lit ? colours::orange : colours::lcd;
    if (down)
        fill = fill.darker(0.1f);
    else if (over)
        fill = fill.darker(0.03f);
    g.setColour(fill);
    g.fillEllipse(box);

    const auto c = box.getCentre();
    juce::Path path;
    if (icon == Icon::gear)
    {
        // The mockup's 16 px gear: a ring and eight spokes.
        path.addEllipse(juce::Rectangle<float>(4.5f, 4.5f).withCentre(c));
        for (int i = 0; i < 8; ++i)
        {
            const float a = juce::MathConstants<float>::pi * 0.25f * static_cast<float>(i);
            const float inner = i % 2 == 0 ? 4.75f : 3.8f;
            const float outer = i % 2 == 0 ? 6.25f : 5.3f;
            path.startNewSubPath(c + juce::Point<float>(std::cos(a), std::sin(a)) * inner);
            path.lineTo(c + juce::Point<float>(std::cos(a), std::sin(a)) * outer);
        }
        g.setColour(colours::ink);
    }
    else
    {
        path.startNewSubPath(c.x, c.y - 5.5f);
        path.lineTo(c.x, c.y - 0.5f);
        path.addCentredArc(c.x, c.y + 0.6f, 4.6f, 4.6f, 0.0f, juce::MathConstants<float>::pi * 0.22f,
                           juce::MathConstants<float>::pi * 1.78f, true);
        g.setColour(lit ? colours::white : colours::sub);
    }
    g.strokePath(path, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void PresetDisplay::set(int newNumber, const juce::String& newName)
{
    if (number != newNumber || name != newName)
    {
        number = newNumber;
        name = newName;
        repaint();
    }
}

void PresetDisplay::paint(juce::Graphics& g)
{
    const auto box = getLocalBounds().toFloat();
    g.setColour(colours::lcd);
    g.fillRoundedRectangle(box, 6.0f);

    const auto font = mono(11.0f);
    const float y = box.getCentreY();
    drawTextAt(g, juce::String(number).paddedLeft('0', 2), font, colours::sub, 10.0f, y);
    g.setFont(font);
    g.setColour(colours::ink);
    g.drawText(name, box.reduced(40.0f, 0.0f), juce::Justification::centred, false);

    g.setColour(colours::sub);
    for (const float sign : { -1.0f, 1.0f }) // small step triangles, as the mockup's ◂ ▸
    {
        const float x = box.getRight() - (sign < 0.0f ? 28.0f : 16.0f);
        juce::Path triangle;
        triangle.addTriangle(x - 2.0f * sign, y - 2.5f, x - 2.0f * sign, y + 2.5f, x + 2.0f * sign, y);
        g.fillPath(triangle);
    }
}

void PresetDisplay::mouseUp(const juce::MouseEvent& e)
{
    if (!getLocalBounds().contains(e.getPosition()))
        return;
    const float x = e.position.x;
    if (x > static_cast<float>(getWidth()) - 22.0f)
    {
        if (onStep) onStep(1);
    }
    else if (x > static_cast<float>(getWidth()) - 36.0f)
    {
        if (onStep) onStep(-1);
    }
    else if (onMenu)
    {
        onMenu();
    }
}
} // namespace rootnote::ui
