#include "Widgets.h"

#include "Pages.h"

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
    setWantsKeyboardFocus(true);

    // Over the name; only there while typing.
    addChildComponent(entry);
    entry.setBounds(2, 63, 76, 17);
    entry.setFont(mono(11.0f));
    entry.setJustification(juce::Justification::centred);
    entry.setIndents(2, 1);
    entry.setColour(juce::TextEditor::backgroundColourId, colours::key);
    entry.setColour(juce::TextEditor::textColourId, colours::ink);
    entry.setColour(juce::TextEditor::highlightColourId, colours::orange.withAlpha(0.3f));
    entry.setColour(juce::TextEditor::outlineColourId, colours::orange);
    entry.setColour(juce::TextEditor::focusedOutlineColourId, colours::orange);
    entry.setColour(juce::CaretComponent::caretColourId, colours::ink);
    entry.onReturnKey = [this] { commitEntry(); };
    entry.onFocusLost = [this] { commitEntry(); };
    entry.onEscapeKey = [this] {
        entry.setVisible(false);
        grabKeyboardFocus();
    };
}

void Encoder::showValueEntry()
{
    entry.setText(getTextFromValue(getValue()), false);
    entry.setVisible(true);
    entry.selectAll();
    entry.grabKeyboardFocus();
}

void Encoder::commitEntry()
{
    if (!entry.isVisible())
        return;
    const auto text = entry.getText().trim();
    entry.setVisible(false); // first: hiding it loses the focus, which commits again
    if (text.isNotEmpty())
        setValue(getValueFromText(text), juce::sendNotificationSync);
}

void Encoder::cancelDrag()
{
    dragging = false;
    entry.setVisible(false);
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
    dragging = false;
    if (e.mods.isPopupMenu() || e.mods.isAltDown() || e.mods.isCommandDown())
    {
        showValueEntry();
        return;
    }
    juce::Slider::mouseDown(e); // starts the gesture
    dragging = true;
    dragProportion = valueToProportionOfLength(getValue());
    lastY = e.position.y;
}

bool Encoder::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::returnKey)
    {
        showValueEntry();
        return true;
    }
    const int direction = code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey     ? 1
                          : code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey ? -1
                                                                                                : 0;
    if (direction == 0 || !isEnabled())
        return false;

    // As a drag of 2 px (Shift: 0.2 px), but at least one step of a stepped parameter.
    const double before = getValue();
    const double step = key.getModifiers().isShiftDown() ? 0.001 : 0.01;
    double value = proportionOfLengthToValue(juce::jlimit(0.0, 1.0, valueToProportionOfLength(before) + direction * step));
    if (snapToInteger)
        value = std::round(value);
    if (getNormalisableRange().snapToLegalValue(value) == before)
        value = before + direction * (snapToInteger ? 1.0 : getInterval());
    setValue(value, juce::sendNotificationSync);
    return true;
}

void Encoder::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging || !isEnabled())
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

EncoderBinding::EncoderBinding(juce::RangedAudioParameter& p, Encoder& e, bool centsOfNote)
    : parameter(p), encoder(e), cents(centsOfNote), attachment(p, [this](float value) { show(value); })
{
    if (cents)
    {
        encoder.setNormalisableRange({ -50.0, 50.0, 0.1 });
        encoder.textFromValueFunction = [](double value) { return signedNumber(value, 1) + " ct"; };
        encoder.valueFromTextFunction = [](const juce::String& text) {
            return text.replace(juce::String::fromUTF8("\xe2\x88\x92"), "-").retainCharacters("+-.0123456789").getDoubleValue();
        };
        encoder.setDoubleClickReturnValue(true, 0.0, {});
    }
    else
    {
        const auto range = p.getNormalisableRange();
        juce::NormalisableRange<double> own {
            static_cast<double>(range.start), static_cast<double>(range.end),
            [range](double, double, double v) { return static_cast<double>(range.convertFrom0to1(static_cast<float>(v))); },
            [range](double, double, double v) { return static_cast<double>(range.convertTo0to1(static_cast<float>(v))); },
            [range](double, double, double v) { return static_cast<double>(range.snapToLegalValue(static_cast<float>(v))); }
        };
        own.interval = static_cast<double>(range.interval);
        own.skew = static_cast<double>(range.skew);
        own.symmetricSkew = range.symmetricSkew;
        encoder.setNormalisableRange(own);
        encoder.textFromValueFunction = [&p](double value) { return p.getText(p.convertTo0to1(static_cast<float>(value)), 0); };
        encoder.valueFromTextFunction = [&p](const juce::String& text) {
            return static_cast<double>(p.convertFrom0to1(p.getValueForText(text)));
        };
        encoder.setDoubleClickReturnValue(true, static_cast<double>(p.convertFrom0to1(p.getDefaultValue())), {});
    }

    encoder.onDragStart = [this] {
        if (inGesture)
            return; // a double-click inside a drag
        inGesture = true;
        centsBase = std::round(static_cast<double>(parameter.convertFrom0to1(parameter.getValue())));
        attachment.beginGesture();
    };
    encoder.onDragEnd = [this] {
        if (inGesture) // not for the end of a drag that began under another binding
        {
            inGesture = false;
            attachment.endGesture();
        }
    };
    encoder.onValueChange = [this] {
        const float value = toParameter(encoder.getValue());
        if (inGesture)
            attachment.setValueAsPartOfGesture(value);
        else
            attachment.setValueAsCompleteGesture(value); // a key, a typed value, accessibility
    };
    attachment.sendInitialUpdate();
}

EncoderBinding::~EncoderBinding()
{
    encoder.onValueChange = nullptr;
    encoder.onDragStart = nullptr;
    encoder.onDragEnd = nullptr;
    encoder.textFromValueFunction = nullptr;
    encoder.valueFromTextFunction = nullptr;
    encoder.cancelDrag();
    if (inGesture)
        attachment.endGesture();
}

void EncoderBinding::show(float value)
{
    const auto v = static_cast<double>(value);
    encoder.setValue(cents ? (v - (inGesture ? centsBase : std::round(v))) * 100.0 : v, juce::dontSendNotification);
}

float EncoderBinding::toParameter(double encoderValue) const
{
    if (!cents)
        return static_cast<float>(encoderValue);
    const double base = inGesture ? centsBase : std::round(static_cast<double>(parameter.convertFrom0to1(parameter.getValue())));
    return static_cast<float>(base + encoderValue / 100.0);
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
