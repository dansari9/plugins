#include "Widgets.h"
#include "../PluginProcessor.h"
#include "Riso.h"

using namespace juce;

namespace snap::ui
{
namespace
{
String minus(String s) { return s.replaceCharacter('-', (juce_wchar) 0x2212); }
String signedValue(float v, int decimals)
{
    const auto s = String(std::abs(v), decimals);
    return (s.getDoubleValue() == 0 ? "" : v > 0 ? "+" : "-") + s;
}
} // namespace

String format(const ParamSpec& s, float v)
{
    switch (s.unit)
    {
        case Unit::percent: return String(v, 0) + "%";
        case Unit::signedPercent: return minus(signedValue(v, 0) + "%");
        case Unit::bipolar: return minus(signedValue(v * 100, 0) + "%");
        case Unit::decibels: return minus((std::abs(v) < 0.05f ? String("0.0") : signedValue(v, 1)) + " dB");
        case Unit::hertz: return v < 1000 ? String(roundToInt(v)) + " Hz" : String(v / 1000, v < 9950 ? 1 : 0) + " kHz";
        case Unit::milliseconds: return v < 1000 ? String(roundToInt(v)) + " ms" : String(v / 1000, 1) + " s";
        case Unit::note: return MidiMessage::getMidiNoteName(roundToInt(v), true, true, 3).toLowerCase();
        case Unit::semitones: return minus(signedValue(v, 1) + " st");
        case Unit::toggle: return v > 0.5f ? "on" : "off";
        case Unit::choice: return s.choices[(size_t) jlimit(0, (int) s.choices.size() - 1, roundToInt(v))];
    }
    return String(v);
}

std::optional<float> parse(const ParamSpec& s, const String& typed)
{
    auto t = typed.trim().toLowerCase().replaceCharacter((juce_wchar) 0x2212, '-');
    if (s.unit == Unit::choice)
    {
        for (size_t i = 0; i < s.choices.size(); ++i)
            if (t == s.choices[i])
                return (float) i;
    }
    if (s.unit == Unit::toggle && (t == "on" || t == "off"))
        return t == "on" ? 1.0f : 0.0f;
    if (s.unit == Unit::note)
        for (int n = (int) s.lo; n <= (int) s.hi; ++n)
            if (t == format(s, (float) n))
                return (float) n;
    if (!t.containsAnyOf("0123456789"))
        return std::nullopt;
    float v = t.getFloatValue();
    if (s.unit == Unit::hertz && (t.endsWith("k") || t.endsWith("khz")))
        v *= 1000;
    if (s.unit == Unit::milliseconds && t.endsWith("s") && !t.endsWith("ms"))
        v *= 1000;
    if (s.unit == Unit::bipolar && (t.endsWith("%") || std::abs(v) > 1))
        v /= 100;
    return jlimit(s.lo, s.hi, v);
}

bool Field::keyPressed(const KeyPress& k)
{
    if (k.getKeyCode() == KeyPress::tabKey && tab)
        return tab(k.getModifiers().isShiftDown() ? -1 : 1), true;
    if (k.getKeyCode() == KeyPress::returnKey)
        return onReturnKey ? onReturnKey() : void(), true;
    if (k.getKeyCode() == KeyPress::escapeKey)
        return onEscapeKey ? onEscapeKey() : void(), true;
    return TextEditor::keyPressed(k);
}

// Parameters are written directly (gesture, value, end) and never listened to: a host's automation may arrive on the
// audio thread, so the editor polls the values at 30 Hz and repaints instead of taking change callbacks.
Control::Control(SnapProcessor& p, Kind k, const char* id, const char* secondId) : kind(k), proc(p)
{
    for (const auto* i : { id, secondId })
        if (i != nullptr)
        {
            params.push_back(p.apvts.getParameter(i));
        }
    setComponentID(id), setTitle(params[0]->getName(64).toLowerCase()), setWantsKeyboardFocus(true);
    setMouseCursor(kind == Kind::fader || kind == Kind::range ? MouseCursor::LeftRightResizeCursor : MouseCursor::UpDownResizeCursor);
    field.setColour(TextEditor::backgroundColourId, colours::rule), field.setColour(TextEditor::textColourId, riso::paper);
    field.setColour(TextEditor::outlineColourId, {}), field.setColour(TextEditor::focusedOutlineColourId, {});
    field.setJustification(Justification::centred);
    field.setPopupMenuEnabled(false), field.setFont(mono(12)), field.setIndents(2, 1);
    field.onReturnKey = [this] { commit(); };
    field.onEscapeKey = [this] { field.setVisible(false), grabKeyboardFocus(); };
    field.onFocusLost = [this] { commit(true); };
    field.tab = [this](int direction) {
        if (!commit())
            return;
        auto order = createKeyboardFocusTraverser()->getAllComponents(findKeyboardFocusContainer());
        std::erase_if(order, [](Component* c) { return dynamic_cast<Control*>(c) == nullptr; });
        const auto at = (int) (std::find(order.begin(), order.end(), this) - order.begin()), n = (int) order.size();
        if (n > 0)
            static_cast<Control*>(order[(size_t) ((at + direction + n) % n)])->edit();
    };
    addChildComponent(field);
}

Control::~Control()
{
    if (isTimerRunning())
        timerCallback();
    else if (dragging)
        param(active).endChangeGesture();
}

void Control::resized()
{
    const auto b = getLocalBounds();
    field.setBounds(kind == Kind::dial ? b.withTrimmedTop(knob > 60 ? 82 : 56) : kind == Kind::amount ? b.withSizeKeepingCentre(80, 20)
                                                                                     : b.withSizeKeepingCentre(jmin(b.getWidth(), 90), 18));
}

void Control::paint(Graphics& g)
{
    using namespace colours;
    const float w = (float) getWidth();
    const bool focused = hasKeyboardFocus(false);
    const Lattice lattice(*this);
    switch (kind)
    {
        case Kind::dial:
        {
            const bool big = knob > 60;
            ui::centred(g, label, 0, 0, w, dimmed(focused ? primary : muted, dim), 11);
            arc(g, (w - knob) / 2, big ? 18.0f : 17.0f, knob, { norm(), centred, dim });
            if (!field.isVisible())
                ui::centred(g, valueText ? valueText() : format(spec(getComponentID().toRawUTF8()), param().convertFrom0to1(norm())), 0,
                            big ? 84.0f : 58.0f, w, dimmed(primary, dim), big ? 13.0f : 12.0f, true);
            break;
        }
        case Kind::amount:
            if (params.size() == 2)
                arc(g, 0, 0, w, { norm(1), false, dim, norm(0), active == 0, arcInk });
            else
                arc(g, 0, 0, w, { norm(), centred, dim, -1, false, arcInk });
            break;
        case Kind::fader:
            fader(g, 0, heavy ? (float) getHeight() - 16 : 0.0f, w, centred ? FaderKind::centred : FaderKind::value, norm(), 0, accent, dim, heavy);
            break;
        case Kind::range: fader(g, 0, heavy ? (float) getHeight() - 16 : 0.0f, w, FaderKind::range, norm(0), norm(1), accent, dim, heavy); break;
    }
    if (focused && kind != Kind::dial)
        box(g, getLocalBounds().toFloat(), accent.withAlpha(0.5f));
}

void Control::edit()
{
    if (isTimerRunning())
        timerCallback();
    field.setText(format(spec(params[(size_t) active]->paramID.toRawUTF8()), param(active).convertFrom0to1(norm(active))), false);
    const auto ink = riso::inkAt(*this);
    field.setColour(TextEditor::backgroundColourId, colours::rule), field.setColour(TextEditor::highlightColourId, ink.withAlpha(0.4f));
    field.setColour(CaretComponent::caretColourId, ink);
    field.setVisible(true), field.grabKeyboardFocus(), field.selectAll();
    repaint();
}

bool Control::apply(const String& text)
{
    const auto typed = parse(spec(params[(size_t) active]->paramID.toRawUTF8()), text);
    if (typed)
        set(active, *typed);
    return typed.has_value();
}

// Unreadable text sets nothing: the field flashes red and stays open, unless it is closing anyway.
bool Control::commit(bool closing)
{
    if (!field.isVisible())
        return true;
    if (!apply(field.getText()) && !closing)
    {
        field.setColour(TextEditor::backgroundColourId, riso::inkAt(*this));
        Timer::callAfterDelay(300, [f = SafePointer<Field>(&field)] {
            if (f)
                f->setColour(TextEditor::backgroundColourId, colours::rule);
        });
        return false;
    }
    field.setVisible(false), repaint();
    return true;
}

void Control::set(int i, float hostValue)
{
    if (isTimerRunning())
        timerCallback();
    proc.beginUndoStep();
    auto& q = param(i);
    q.beginChangeGesture(), q.setValueNotifyingHost(q.convertTo0to1(hostValue)), q.endChangeGesture();
    repaint();
}

void Control::begin()
{
    proc.beginUndoStep();
    value = norm(active);
    param(active).beginChangeGesture();
}

void Control::move(double delta)
{
    value = jlimit(0.0, 1.0, value + delta);
    auto& q = param(active);
    q.setValueNotifyingHost(q.convertTo0to1(q.convertFrom0to1((float) value)));
    repaint();
}

int Control::pick(Point<float> at) const
{
    if (kind == Kind::range)
        return std::abs(at.x - 2 - norm(0) * ((float) getWidth() - 4)) <= std::abs(at.x - 2 - norm(1) * ((float) getWidth() - 4)) ? 0 : 1;
    if (kind == Kind::amount && params.size() == 2)
    {
        const float r = (float) getWidth() / 2 - 2.5f;
        return at.getDistanceFrom({ (float) getWidth() / 2, r + 2.5f }) < r * 0.81f ? 0 : 1;
    }
    return 0;
}

float Control::travel(Point<float> d) const // screen px along the control's axis
{
    const float scale = jmax(0.01f, getApproximateScaleFactorForComponent(this));
    return (kind == Kind::fader || kind == Kind::range || std::abs(d.x) > std::abs(d.y) ? d.x : -d.y) * scale;
}

void Control::mouseDown(const MouseEvent& e)
{
    if (isTimerRunning())
        timerCallback();
    dragging = false;
    if (params.size() == 2)
        if (const int i = pick(e.position); i != active || kind == Kind::amount)
        {
            active = i, repaint();
            if (onPick)
                onPick(i);
        }
    if (e.mods.isPopupMenu() && !e.mods.isCommandDown())
    {
        reset = true; // right-click belongs to the menu
        return;
    }
    if ((reset = e.mods.isAltDown() || e.mods.isCommandDown()))
        field.setVisible(false), set(active, param(active).convertFrom0to1(param(active).getDefaultValue()));
}

void Control::mouseDrag(const MouseEvent& e)
{
    if (reset)
        return;
    const float along = travel(e.position - (dragging ? last : e.mouseDownPosition));
    if (!dragging)
    {
        if (std::abs(along) <= 3.0001f)
            return; // a click
        dragging = true, begin();
    }
    last = e.position; // Shift applies from here on, so pressing it never jumps the value
    move(along / (e.mods.isShiftDown() ? 2500.0 : 250.0));
}

void Control::mouseUp(const MouseEvent& e)
{
    if (dragging)
        param(active).endChangeGesture();
    dragging = false;
    if (!e.mods.isPopupMenu() || e.mods.isCommandDown() || e.getDistanceFromDragStart() > 3)
        return;
    auto& q = param(active);
    const auto& s = spec(q.paramID.toRawUTF8());
    PopupMenu menu;
    menu.addItem(1, "type value...");
    menu.addItem(2, "reset to " + format(s, q.convertFrom0to1(q.getDefaultValue())));
    menu.addItem(3, "copy value");
    menu.addItem(4, "paste value");
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), [self = SafePointer<Control>(this)](int item) {
        if (!self)
            return;
        auto& p = self->param(self->active);
        if (item == 1)
            self->edit();
        else if (item == 2)
            self->set(self->active, p.convertFrom0to1(p.getDefaultValue()));
        else if (item == 3)
            SystemClipboard::copyTextToClipboard(format(spec(p.paramID.toRawUTF8()), p.convertFrom0to1(p.getValue())));
        else if (item == 4)
            self->apply(SystemClipboard::getTextFromClipboard());
    });
}

void Control::mouseDoubleClick(const MouseEvent& e)
{
    if (!e.mods.isAltDown() && !e.mods.isCommandDown() && !e.mods.isPopupMenu())
        edit();
}

void Control::mouseWheelMove(const MouseEvent&, const MouseWheelDetails& w)
{
    if (dragging || field.isVisible())
        return;
    const float delta = (std::abs(w.deltaY) > 0 ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    if (std::abs(delta) <= 0)
        return;
    if (!isTimerRunning())
        begin();
    const bool fine = ModifierKeys::currentModifiers.isShiftDown();
    const int steps = param(active).getNumSteps();
    if (w.isSmooth)
        move(delta * 512 / (fine ? 2500.0 : 250.0));
    else if (steps > 1 && steps < 64)
        move((delta > 0 ? 1.0 : -1.0) / (steps - 1));
    else
        move((delta > 0 ? 1 : -1) * (fine ? 0.001 : 0.01));
    startTimer(300); // a wheel burst ends 300 ms after its last event
}

void Control::timerCallback()
{
    stopTimer();
    param(active).endChangeGesture();
}

bool Control::keyPressed(const KeyPress& k)
{
    const int code = k.getKeyCode();
    if (code == KeyPress::returnKey)
        return edit(), true;
    if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey)
        return set(active, param(active).convertFrom0to1(param(active).getDefaultValue())), true;
    if (code == KeyPress::upKey || code == KeyPress::rightKey || code == KeyPress::downKey || code == KeyPress::leftKey)
    {
        if (!isTimerRunning())
            begin();
        move((code == KeyPress::upKey || code == KeyPress::rightKey ? 1 : -1)
             * (k.getModifiers().isShiftDown() ? 0.001 : k.getModifiers().isCommandDown() ? 0.1 : 0.01));
        startTimer(300);
        return true;
    }
    return false; // Space (transport) and Tab (focus traversal) pass through
}

std::unique_ptr<AccessibilityHandler> Control::createAccessibilityHandler()
{
    struct Value : AccessibilityTextValueInterface
    {
        explicit Value(Control& c) : control(c) {}
        bool isReadOnly() const override { return false; }
        String getCurrentValueAsString() const override
        {
            String s;
            for (int i = 0; i < (int) control.params.size(); ++i)
            {
                auto& p = control.param(i);
                s << (i > 0 ? ", " : "") << format(spec(p.paramID.toRawUTF8()), p.convertFrom0to1(p.getValue()));
            }
            return s;
        }
        void setValueAsString(const String& text) override { control.apply(text); }
        Control& control;
    };
    return std::make_unique<AccessibilityHandler>(*this, AccessibilityRole::slider, AccessibilityActions(),
                                                  AccessibilityHandler::Interfaces(std::make_unique<Value>(*this)));
}

Key::Key(const String& title, Role r) : role(r)
{
    setTitle(title), setWantsKeyboardFocus(true), setMouseClickGrabsKeyboardFocus(false), setRepaintsOnMouseActivity(true);
    setMouseCursor(MouseCursor::PointingHandCursor);
}

void Key::mouseUp(const MouseEvent& e)
{
    if (onClick && e.mouseWasClicked() && getLocalBounds().contains(e.getPosition()))
        onClick(e.mods);
}

void Key::mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& w)
{
    if (!onStep)
        return Component::mouseWheelMove(e, w);
    wheel += (std::abs(w.deltaY) > 0 ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    if (std::abs(wheel) >= 0.12f) // one notch of a stepped wheel; a few px of a smooth one
        onStep(wheel > 0 ? 1 : -1), wheel = 0;
}

bool Key::keyPressed(const KeyPress& k)
{
    const int code = k.getKeyCode();
    if (onStep && (code == KeyPress::upKey || code == KeyPress::rightKey || code == KeyPress::downKey || code == KeyPress::leftKey))
        return onStep(code == KeyPress::upKey || code == KeyPress::rightKey ? 1 : -1), true;
    if (code != KeyPress::returnKey || !onClick)
        return false;
    onClick({});
    return true;
}

std::unique_ptr<AccessibilityHandler> Key::createAccessibilityHandler()
{
    struct Handler : AccessibilityHandler
    {
        explicit Handler(Key& k)
            : AccessibilityHandler(k, k.role == Role::toggle ? AccessibilityRole::toggleButton : k.role == Role::radio ? AccessibilityRole::radioButton
                                                                                                                       : AccessibilityRole::button,
                                   AccessibilityActions().addAction(AccessibilityActionType::press, [&k] { k.keyPressed(KeyPress(KeyPress::returnKey)); })),
              key(k)
        {
        }
        AccessibleState getCurrentState() const override
        {
            auto state = AccessibilityHandler::getCurrentState();
            if (key.role == Role::button)
                return state;
            state = state.withCheckable();
            return key.on && key.on() ? state.withChecked() : state;
        }
        Key& key;
    };
    return std::make_unique<Handler>(*this);
}
} // namespace snap::ui
