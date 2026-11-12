#pragma once

#include "Theme.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace rootnote::ui
{
/**
    A hardware key: light or dark cap, optional LED, optional underline (the
    pitch keys' in-key bar), filled orange or outlined orange. Its bounds are
    the cap plus one pixel for the drop line. The setters repaint only on a
    change, so they can be called every frame.
*/
class Key : public juce::Button
{
public:
    enum class Led { none, off, on };
    enum class Look { light, dark, lcd, lcdOn, filled, outlined };

    explicit Key(const juce::String& text, float fontPx = 10.0f, float trackingEm = 0.06f);

    void setLook(Look);
    void setLed(Led);
    Led getLed() const { return led; }
    void setUnderline(std::optional<juce::Colour>);
    void setText(const juce::String&);

    /** Called with the modifiers of the click: Alt-click is a second action. */
    std::function<void(const juce::ModifierKeys&)> onPress;

private:
    using juce::Button::clicked;
    void clicked(const juce::ModifierKeys&) override;
    void paintButton(juce::Graphics&, bool over, bool down) override;

    juce::String text;
    juce::Font font;
    Look look = Look::light;
    Led led = Led::none;
    std::optional<juce::Colour> underline;
};

/**
    An encoder: a light ring, a coloured cap and a dot, with its name below.
    Drag up/down; Shift drags ten times finer; double-click resets. With the
    keyboard focus the arrow keys step it (Shift finer). Alt-, Cmd- or
    right-click, or Return, asks for typed entry (onValueEntry: the editor
    opens the value's field on the display). An EncoderBinding connects it
    to a parameter.
*/
class Encoder : public juce::Slider
{
public:
    Encoder();

    void setCap(juce::Colour, const juce::String& label);

    /** Typed entry was asked for. */
    std::function<void()> onValueEntry;
    void showValueEntry() { if (onValueEntry) onValueEntry(); }

    /** The rest of the current drag is ignored. */
    void cancelDrag() { dragging = false; }

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    juce::Colour cap;
    juce::String label;
    bool dragging = false;
    double dragProportion = 0.0;
    float lastY = 0.0f;
};

/**
    Binds an Encoder to a parameter, as juce::SliderParameterAttachment does,
    or to the cents (-50..+50) of a note parameter. It installs its own range,
    text conversion and reset value, so nothing of an earlier binding stays.

    Every change reaches the host inside a gesture: a drag is one, and so is
    each arrow key, typed value or reset. Destroying the binding ends a drag
    in progress and the encoder ignores the rest of it, so remapping a page or
    closing the editor mid-drag leaves no gesture open and moves nothing else.
*/
class EncoderBinding
{
public:
    EncoderBinding(juce::RangedAudioParameter&, Encoder&, bool cents = false);
    ~EncoderBinding();

private:
    void show(float value);
    float toParameter(double encoderValue) const;

    juce::RangedAudioParameter& parameter;
    Encoder& encoder;
    const bool cents;
    double centsBase = 0.0; // the semitone the cents are relative to during a gesture
    bool inGesture = false;
    juce::ParameterAttachment attachment;

    JUCE_DECLARE_NON_COPYABLE(EncoderBinding)
};

/** A plain round button with a painted icon: the settings gear or the power switch. */
class RoundButton : public juce::Button
{
public:
    enum class Icon { gear, power };
    explicit RoundButton(Icon);
    void setLit(bool);

private:
    void paintButton(juce::Graphics&, bool over, bool down) override;
    Icon icon;
    bool lit = false;
};

/** The preset LCD: number, name and the two step arrows. */
class PresetDisplay : public juce::Component
{
public:
    void set(int number, const juce::String& name);
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;

    std::function<void(int step)> onStep;  // -1 or +1
    std::function<void()> onMenu;

private:
    int number = 0;
    juce::String name;
};
} // namespace rootnote::ui
