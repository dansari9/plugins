#pragma once
#include "../Parameters.h"
#include "Style.h"
#include <optional>

class SnapProcessor;

// The interactive pieces. Each carries a help tip (shown in the bottom bar on hover) and an accessible title.
namespace snap::ui
{
// Display text for a parameter value: units attached, Unicode minus; bipolar values as signed percent.
juce::String format(const ParamSpec&, float value);
// Typed text to a host value, accepting the display forms ("−5.3 dB", "1.2 s", "12k", "+20%"); nullopt if unreadable.
std::optional<float> parse(const ParamSpec&, const juce::String&);

struct Field : juce::TextEditor // value entry; Return, Esc and Tab act at once
{
    std::function<void(int)> tab;
    bool keyPressed(const juce::KeyPress&) override;
};

// A dial, fader or range bar bound to one or two parameters. Drag (vertical or horizontal, 250 screen px for the full
// travel, Shift 10x finer), Alt/Cmd-click resets, double-click types a value, the wheel nudges, right-click opens a menu,
// arrows / Return / Delete work with keyboard focus. Every drag, reset, typed value and wheel burst is one host gesture
// and one undo step.
class Control : public juce::Component, private juce::Timer
{
public:
    enum class Kind { dial, amount, fader, range };
    Control(SnapProcessor&, Kind, const char* id, const char* secondId = nullptr);
    ~Control() override;

    Kind kind;
    juce::String label, hint;    // label: a dial's name (drawn by small dials)
    bool centred = false, dim = false;
    int active = 0;                       // the parameter a drag, wheel or typed value moves (range: nearest handle)
    std::function<void(int)> onPick;      // amount with two rings: a click picked ring 0 (inner) or 1 (outer)
    std::function<juce::String()> valueText; // optional replacement for the value under a small dial
    juce::RangedAudioParameter& param(int i = 0) const { return *params[(size_t) i]; }
    float norm(int i = 0) const { return params[(size_t) i]->getValue(); }
    void edit();                      // opens the value field

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    bool commit(bool closing = false);

private:
    void timerCallback() override;
    void begin();
    void move(double delta);
    void set(int i, float hostValue); // one complete gesture
    bool apply(const juce::String&);
    int pick(juce::Point<float>) const;
    float travel(juce::Point<float> delta) const;

    SnapProcessor& proc;
    std::vector<juce::RangedAudioParameter*> params;
    Field field;
    double value = 0;
    bool dragging = false, reset = false, wheeling = false;
    juce::Point<float> last;
};

// A clickable area: a button, toggle, choice or tab. paint is the owner's lambda.
class Key : public juce::Component
{
public:
    enum class Role { button, toggle, radio };
    explicit Key(const juce::String& title, Role = Role::button);
    std::function<void(const juce::ModifierKeys&)> onClick; // right-click: mods.isPopupMenu()
    std::function<void(juce::Graphics&, Key&)> draw;
    std::function<bool()> on; // toggle / radio state, for screen readers
    juce::String hint;
    Role role;
    void paint(juce::Graphics& g) override
    {
        if (draw)
            draw(g, *this);
    }
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
};
} // namespace snap::ui
