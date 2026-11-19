// silt 0.2's v6 editor: one cell grid (0.1.0's cell, 106 x 49) drawn into a device-pixel backing image.
// Coordinates are the v6 mockup's cells (gui/v6/v6.py); each group owns its hit cells, and only changed cells repaint.
#include "BinaryData.h"
#include "Silt.h"
#include <tuple>
using namespace juce;
namespace
{
constexpr int C = 106, R = 49, W = 905, H = 836; // 905 x 836 px at 100%
constexpr double designCell = 1024.0 / 120;
constexpr float cw = (float) designCell, ch = cw * 2;
const Colour page { 0xff0d0d0d }, white { 0xffececec }, grey { 0xff7a7a7a }, light { 0xffbcbcbc }, rule { 0xff2a2a2a }, dark { 0xff454545 }, red { 0xffff3d1f };
const Colour heads[] { white, red, light, grey }; // each head's caret
String u(const char* s) { return String::fromUTF8(s); }
const auto caret = u("▲"), filledBox = u("■"), emptyBox = u("□");
int py(double v) { return (int) std::nearbyint(v); } // Python's round (half to even), as the mockup renderer
Rectangle<int> cells(int x, int y, int w, int h)
{
    return Rectangle<int>::leftTopRightBottom(roundToInt(cw * (float) x), roundToInt(ch * (float) y), roundToInt(cw * (float) (x + w)), roundToInt(ch * (float) (y + h)));
}
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size));
}
struct Cell
{
    juce_wchar c = ' ';
    Colour fg, bg; // bg transparent: the page
    bool bold = false, line = false;
    std::array<uint32, 8> dots {}; // RGB, radius in % of a cell width in the top byte; 0 = no dot
    bool operator!=(const Cell& o) const { return c != o.c || fg != o.fg || bg != o.bg || bold != o.bold || line != o.line || dots != o.dots; }
};
enum Mode { under, over }; // a dot onto a dot: keep the old one, or replace it
struct Grid
{
    Cell at[R][C];
    int ox = 0, oy = 0;
    void put(int x, int y, StringRef s, Colour fg, bool bold = false, Colour bg = {})
    {
        x += ox, y += oy;
        for (auto p = s.text; !p.isEmpty(); ++x)
            if (const auto c = p.getAndAdvance(); isPositiveAndBelow(x, C) && isPositiveAndBelow(y, R))
                at[y][x] = { c, fg, bg, bold };
    }
    void centre(int x, int w, int y, String s, Colour fg, bool bold = false) { s = s.substring(0, w), put(x + (w - s.length()) / 2, y, s, fg, bold); }
    void right(int x, int y, const String& s, Colour fg, bool bold = false) { put(x - s.length() + 1, y, s, fg, bold); }
    void dot(int dx, int dy, Colour fg, int radius = 12, Mode mode = over) // in dots: 2 per cell across, 4 down
    {
        dx += 2 * ox, dy += 4 * oy;
        if (!isPositiveAndBelow(dx, 2 * C) || !isPositiveAndBelow(dy, 4 * R)) return;
        auto& c = at[dy / 4][dx / 2];
        if (mode == under && c.c != ' ') return; // under text: hidden
        auto& d = c.dots[(size_t) (dy % 4 * 2 + dx % 2)];
        if (d == 0 || mode == over) d = (uint32) radius << 24 | (fg.getARGB() & 0xffffff);
    }
    void rule(int x0, int x1, int y)
    {
        for (int x = x0; x < x1; ++x) at[oy + y][ox + x].line = true;
    }
    void underline(int x, int w, int y)
    {
        for (int i = x; i < x + w; ++i) dot(i * 2, y * 4 + 3, red, 8);
    }
    void clear(int x, int y, int w, int h)
    {
        for (int j = y; j < y + h; ++j) std::fill_n(at[j] + x, w, Cell {});
    }
    void toggle(int x, int y, StringRef label, bool active)
    {
        put(x, y, active ? filledBox : emptyBox, active ? red : grey), put(x + 2, y, label, active ? white : grey);
    }
};

struct Key : Button
{
    explicit Key(const String& id) : Button(id) { setComponentID(id), setRepaintsOnMouseActivity(false); }
    void paintButton(Graphics&, bool, bool) override {}
    std::function<void(int)> wheel, choose;
    std::function<void()> finish;
    std::function<void(bool)> hold;
    bool holding = false;
    bool active = false;
    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override
    {
        if (!isToggleable()) return Button::createAccessibilityHandler();
        struct Handler : AccessibilityHandler
        {
            explicit Handler(Key& k) : AccessibilityHandler(k, k.choose ? AccessibilityRole::radioButton : AccessibilityRole::toggleButton,
                AccessibilityActions().addAction(AccessibilityActionType::press, [&k] { if (k.onClick) k.onClick(); })), key(k) {}
            AccessibleState getCurrentState() const override
            {
                auto state = AccessibilityHandler::getCurrentState().withCheckable();
                return key.active ? state.withChecked() : state;
            }
            Key& key;
        };
        return std::make_unique<Handler>(*this);
    }
    void mouseDown(const MouseEvent& e) override
    {
        if (finish) finish();
        if (hold && e.mods.isShiftDown() && !e.mods.isAltDown() && !e.mods.isCommandDown())
        {
            holding = true;
            hold(true);
        }
        else Button::mouseDown(e);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (holding) holding = false, hold(false);
        else Button::mouseUp(e);
    }
    bool keyPressed(const KeyPress& k) override
    {
        const auto code = k.getKeyCode();
        if (code == KeyPress::escapeKey) { giveAwayKeyboardFocus(); return true; }
        if (code == KeyPress::returnKey) { if (onClick) onClick(); return true; }
        if (choose && (code == KeyPress::leftKey || code == KeyPress::upKey || code == KeyPress::rightKey || code == KeyPress::downKey
                       || code == KeyPress::homeKey || code == KeyPress::endKey))
        {
            choose(code == KeyPress::homeKey ? -100 : code == KeyPress::endKey ? 100
                : code == KeyPress::leftKey || code == KeyPress::upKey ? -1 : 1);
            return true;
        }
        return false; // including Space (transport) and Tab (component traversal)
    }
    void mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (const float d = (std::abs(w.deltaY) > 0 ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f); wheel && std::abs(d) > 0) wheel(d > 0 ? 1 : -1);
        else Button::mouseWheelMove(e, w);
    }
};
// A v6 dial in an 11-cell module: name, an 11-dot 270 degree arc over 3 rows (centred: from the top), value.
void knob(Grid& g, int x, int y, float norm, const String& name, const String& value, bool centred, bool dim, bool named, bool focused, float defaultDot)
{
    const double cx = 2 * x + 10, cy = 4 * (y + 1) + 6.6, r = 5.3, v = 270.0 * norm;
    for (int t = 0; t <= 270; t += 6)
    {
        const double a = degreesToRadians(225.0 + t);
        const bool on = centred ? jmin(135.0, v) <= t && t <= jmax(135.0, v) : t <= v;
        g.dot(py(cx + r * std::sin(a)), py(cy - r * std::cos(a)), on ? (dim ? dark : red) : (dim ? rule : dark));
    }
    const double a = degreesToRadians(225 + v);
    for (int k = 0; k < 5; ++k) g.dot(py(cx + (r - 1.5) * std::sin(a) * k / 4), py(cy - (r - 1.6) * std::cos(a) * k / 4), dim ? grey : white);
    if (defaultDot >= 0)
    {
        const double d = degreesToRadians(225 + 270.0 * defaultDot);
        g.dot(py(cx + r * std::sin(d)), py(cy - r * std::cos(d)), white);
    }
    if (named) g.centre(x, 11, y, name, dim ? dark : grey);
    g.centre(x, 11, y + 4, value, dim ? grey : white, !dim);
    if (focused) g.underline(x + (11 - name.length()) / 2, name.length(), y + !named);
}

struct Panel { const char* name; int x, y, w, h; };
// Groups span the window (absolute cells) and take clicks only on their children, except the cloud.
constexpr Panel panels[] { {"header",0,0,C,R}, {"cloud",3,5,100,12}, {"tier1",0,0,C,R}, {"tier2",0,0,C,R}, {"macros",0,0,C,R}, {"freeze",0,0,C,R} };
struct Group : Component
{
    SiltProcessor& p;
    Panel spec;
    struct Choice : Timer
    {
        RangedAudioParameter& param;
        ParameterAttachment attachment { param, [](float) {} };
        explicit Choice(RangedAudioParameter& q) : param(q) {}
        ~Choice() override { if (isTimerRunning()) timerCallback(); }
        void timerCallback() override { stopTimer(), attachment.endGesture(); }
        void step(int delta)
        {
            const int next = jlimit(0, param.getNumSteps() - 1, roundToInt(param.convertFrom0to1(param.getValue())) + delta);
            if (!isTimerRunning())
            {
                if (roundToInt(param.convertFrom0to1(param.getValue())) == next) return;
                attachment.beginGesture();
            }
            attachment.setValueAsPartOfGesture((float) next);
            startTimer(300);
        }
    };
    std::vector<std::unique_ptr<ParameterAttachment>> attachments;
    std::vector<std::unique_ptr<Choice>> choices;
    OwnedArray<Key> keys;
    Group(SiltProcessor& proc, Panel panel) : p(proc), spec(panel)
    {
        setComponentID(String("group:") + panel.name), setBounds(cells(panel.x, panel.y, panel.w, panel.h));
        setInterceptsMouseClicks(String(panel.name) == "cloud", true);
    }
    virtual void draw(Grid&) = 0;
    float value(const char* id) const { return p.apvts.getRawParameterValue(id)->load(); }
    Key& key(const String& id, int x, int y, int w, std::function<void()> action)
    {
        auto& k = *keys.add(new Key(id)); k.onClick = std::move(action);
        addAndMakeVisible(k), k.setBounds(cells(x, y, w, 1)); return k;
    }
    ParameterAttachment& attach(const char* id) { return *attachments.emplace_back(std::make_unique<ParameterAttachment>(*p.apvts.getParameter(id), [](float) {})); }
    static bool resetting() { const auto m = ModifierKeys::getCurrentModifiersRealtime(); return m.isAltDown() || m.isCommandDown(); }
    void word(const char* id, int index, int x, int y, int w)
    {
        auto& q = *p.apvts.getParameter(id);
        if (index == 0) choices.push_back(std::make_unique<Choice>(q));
        auto& choice = *choices.back();
        auto& k = key(String(id) + ":" + String(index), x, y, w, [index, &choice, &q] {
            if (choice.isTimerRunning()) choice.timerCallback();
            choice.attachment.setValueAsCompleteGesture(resetting() ? q.convertFrom0to1(q.getDefaultValue()) : (float) index);
        });
        k.setTitle(q.name + ", " + q.getAllValueStrings()[index] + ", " + String(index + 1) + " of " + String(q.getNumSteps()));
        k.setToggleable(true);
        k.active = roundToInt(value(id)) == index;
        k.wheel = k.choose = [&choice](int d) { choice.step(d); };
        k.finish = [&choice] { if (choice.isTimerRunning()) choice.timerCallback(); };
    }
    Key& toggle(const char* id, const char* text, int x, int y)
    {
        auto& a = attach(id);
        auto* q = p.apvts.getParameter(id);
        auto& k = key(id, x, y, String(text).length() + 2, [this, id, &a, q] {
            a.setValueAsCompleteGesture(resetting() ? q->getDefaultValue() : value(id) > .5f ? 0.0f : 1.0f);
        });
        k.setTitle(q->name);
        k.setToggleable(true);
        k.active = value(id) > .5f;
        return k;
    }
};

struct Header : Group
{
    String hint, name, notice;
    uint32 noticeTime = 0;
    int program = -2;
    explicit Header(SiltProcessor& proc) : Group(proc, panels[0])
    {
        key("prev", 9, 1, 2, [this] { p.stepPreset(-1); }).setTitle("previous preset");
        key("next", 21, 1, 1, [this] { p.stepPreset(1); }).setTitle("next preset");
        auto& picker = key("presets", 12, 1, 8, [this] {
            PopupMenu menu;
            auto files = p.presetFolder().findChildFiles(File::findFiles, false, "*.xml");
            files.sort();
            for (int i = 0; i < silt::numPresets; ++i)
                menu.addItem(i + 1, String(i + 1).paddedLeft('0', 2) + " " + silt::presets[i].name, true, p.preset == i);
            if (!files.isEmpty()) menu.addSeparator();
            for (int i = 0; i < files.size(); ++i)
                menu.addItem(silt::numPresets + i + 1, files[i].getFileNameWithoutExtension(), true, p.preset < 0 && files[i].getFileNameWithoutExtension() == name);
            menu.showMenuAsync(PopupMenu::Options().withTargetComponent(keys[2]), [self = SafePointer<Header>(this), files = std::move(files)](int r) {
                if (self && r > 0)
                {
                    if (r <= silt::numPresets) self->p.setCurrentProgram(r - 1);
                    else self->p.loadUserPreset(files[r - silt::numPresets - 1]);
                }
            });
        });
        picker.setTitle("preset list");
        picker.wheel = [this](int d) { p.stepPreset(d); };
        key("save", 26, 1, 6, [this] {
            if (const auto file = p.saveUserPreset(); file != File())
                tell(u("saved · ") + file.getFileNameWithoutExtension());
        }).setTitle("save a user preset");
        for (int slot = 0; slot < 2; ++slot)
            key(slot == 0 ? "A" : "B", 33 + 2 * slot, 1, 2, [this, slot] {
                const bool copy = resetting();
                if (copy && p.abSlot() != slot) tell(u(slot == 1 ? "A → B" : "B → A"));
                p.selectAb(slot, copy);
            }).setTitle(slot == 0 ? "slot A" : "slot B");
        key("rnd", 41, 1, 11, [this] {
            const auto seed = Time::getHighResolutionTicks();
            if (ModifierKeys::getCurrentModifiersRealtime().isShiftDown()) p.nudge(seed);
            else p.randomise(seed);
        }).setTitle("randomize");
        for (int i = 0; i < 4; ++i) word("oversample", i, i ? 76 + 3 * i : 75, 1, 2 + (i == 0));
    }
    void tell(String message) { notice = std::move(message), noticeTime = Time::getMillisecondCounter(); }
    void place(int index, const String& user)
    {
        if (index == program && (index >= 0 || name == user.substring(0, 9))) return;
        program = index;
        name = (index >= 0 ? p.getProgramName(index) : user).substring(0, 9);
        keys[1]->setBounds(cells(15 + name.length(), 1, 2, 1));
        keys[2]->setBounds(cells(11, 1, 4 + name.length(), 1));
    }
    void draw(Grid& g) override
    {
        g.put(3, 1, "silt", white, true), g.put(10, 1, u("‹"), grey);
        g.put(12, 1, program >= 0 ? String(program + 1).paddedLeft('0', 2) : String("--"), grey);
        g.put(15, 1, name, white, true), g.put(16 + name.length(), 1, u("›"), grey);
        g.put(27, 1, "save", grey);
        for (int s = 0; s < 2; ++s) g.put(34 + 2 * s, 1, s ? "B" : "A", p.abSlot() == s ? page : grey, p.abSlot() == s, p.abSlot() == s ? white : Colour());
        g.put(42, 1, "randomize", grey);
        g.put(64, 1, "oversample", grey);
        const char* q[] { "off", "2x", "4x", "8x" };
        for (int i = 0; i < 4; ++i)
        {
            auto& key = *keys[7 + i];
            key.active = roundToInt(value("oversample")) == i;
            g.put(i ? 76 + 3 * i : 75, 1, q[i], key.active || key.isMouseOver() ? white : dark, key.active);
        }
        if (hint.isEmpty()) g.put(93, 1, "daveydidit", dark);
        else g.right(102, 4, hint.substring(0, 100), grey);
        g.rule(3, 103, 2);
    }
};
struct Freeze : Group
{
    explicit Freeze(SiltProcessor& proc) : Group(proc, panels[5])
    {
        auto& k = toggle("freeze", "freeze", 95, 16);
        k.hold = [this, restore = 0.0f](bool pressed) mutable {
            auto& a = *attachments.front();
            if (pressed) restore = value("freeze"), a.beginGesture();
            a.setValueAsPartOfGesture(pressed ? 1.0f : restore);
            if (!pressed) a.endGesture();
        };
    }
    ~Freeze() override { if (keys[0]->holding) keys[0]->hold(false); }
    void draw(Grid& g) override
    {
        auto& key = *keys[0];
        g.toggle(95, 16, "freeze", key.active = value("freeze") > .5f);
        if (key.isMouseOver() && !key.active) g.put(95, 16, emptyBox, white);
    }
};

// Event history is published by the audio thread, so even an editor opened mid-note has a cloud. It advances
// only with processed audio: stopped/idle visuals retain their last frame, and frozen capture stops scrolling.
// The timeline under it spans the 8 s capture: a caret per head at its read anchor, trailing scan's direction.
// Double-click the cloud: freeze. Click or drag the timeline: position (⇧ fine).
struct Cloud : Group
{
    ParameterAttachment &freeze, &position;
    std::function<void(int)> focusHead;
    std::array<int, 4> carets { -1, -1, -1, -1 };
    bool scrubbing = false;
    double scrubbed = 0;
    explicit Cloud(SiltProcessor& proc) : Group(proc, panels[1]), freeze(attach("freeze")), position(attach("position")) { setTitle("cloud"); }
    ~Cloud() override { if (scrubbing) position.endGesture(); }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.y < cells(0, 11, 1, 1).getY())
            freeze.setValueAsCompleteGesture(value("freeze") > .5f ? 0.0f : 1.0f);
    }
    void mouseDown(const MouseEvent& e) override
    {
        if (e.y < cells(0, 11, 1, 1).getY() || e.position.x >= cw * 88) return;
        const auto hit = std::find(carets.begin(), carets.end(), (int) (e.position.x / cw));
        if (hit != carets.end()) { if (focusHead) focusHead((int) (hit - carets.begin())); return; }
        scrubbing = true, scrubbed = value("position"), position.beginGesture(), mouseDrag(e);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (!scrubbing) return;
        const double at = 8000 * (1 - e.position.x / (cw * 87)); // ms behind now under the pointer
        scrubbed = e.mods.isShiftDown() ? scrubbed + (at - scrubbed) * .1 : at;
        const double step = e.mods.isShiftDown() ? 125 : 500; // the sync position parameter is referenced to 120 BPM
        const double back = value("sizeSync") < .5f ? std::round(scrubbed / step) * step : scrubbed;
        position.setValueAsPartOfGesture((float) jlimit(0.0, 4000.0, back));
    }
    void mouseUp(const MouseEvent&) override { if (scrubbing) position.endGesture(); scrubbing = false; }
    void draw(Grid& g) override
    {
        constexpr int w = 100, h = 11, ww = 2 * w, hh = 4 * h;
        const auto& f = p.feed;
        const auto now = f.time.load(); const double fs = f.sampleRate.load();
        const bool frozen = f.frozen.load();
        const double pos = jlimit(0.0, 8.0, (double) f.centre.load()), scatter = value("scatter") / 100.0;
        const auto dot = [](double sec) { return (1 - sec / 8) * (ww - 1); };
        const int xa = roundToInt(dot(jmin(8.0, pos + scatter * 2))), xb = roundToInt(dot(jmax(0.0, pos - scatter * 2)));
        const double mid = hh / 2.0 - .5;
        // Sparse dots mirror the recorded input's amplitude. Zero amplitude leaves the bed empty.
        const int at = f.at.load();
        for (int x = 0; x < ww; x += 3)
        {
            const int bin = (at - 1 - py((ww - 1 - x) * 1.0 / ww * silt::Feed::bins) + silt::Feed::bins * 2) % silt::Feed::bins;
            const double extent = jlimit(0.0, 1.0, (double) f.wave[(size_t) bin].load() * 3) * hh * .5;
            for (int y = 0; y < hh; y += 3)
                if (std::abs(y - mid) < extent && (x * 71 + y * 37) % 11 < 8)
                    g.dot(x, y, xa <= x && x <= xb ? dark : rule, 7);
        }
        constexpr double limits[] { 0, .025, .07, .17, .29, .44, .60, .78, 1.01 };
        const uint32 ramp[] { 0xffff5a33, 0xffff3d1f, 0xfff4f4f4, 0xffc4c4c4, 0xff949494, 0xff686868, 0xff474747, 0xff2e2e2e };
        const auto count = f.events.load(std::memory_order_acquire);
        const auto head = f.captureHead.load();
        std::array<uint8_t, ww * hh> shades {}; // a newer, brighter grain owns a shared dot
        for (auto i = jmax((juce::int64) 0, count - (juce::int64) silt::Feed::historySize); i < count; ++i)
        {
            const auto& e = f.history[(size_t) i % silt::Feed::historySize];
            const auto sequence = e.sequence.load(std::memory_order_acquire);
            if (sequence & 1u) continue;
            const auto frame = e.frame.load(std::memory_order_relaxed), eventHead = e.captureHead.load(std::memory_order_relaxed);
            const auto grain = e.grain.load(std::memory_order_relaxed);
            const float eventBack = e.back.load(std::memory_order_relaxed), eventPitch = e.pitch.load(std::memory_order_relaxed), eventSize = e.size.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire); // the field reads complete before the sequence re-check
            if (sequence != e.sequence.load(std::memory_order_relaxed)) continue;
            const double age = (now - frame) / fs / 3.0;
            const int shade = (int) (std::upper_bound(std::begin(limits), std::end(limits), age) - std::begin(limits)) - 1;
            if (shade < 0 || shade > 7) continue;
            const double back = eventBack + (head - eventHead) / fs;
            if (back < 0 || back > 8) continue;
            // Stable scatter gives each actual grain a legible pitch lane; pitch remains the y-axis.
            const uint32 hash = grain * 2654435761u;
            const double jitter = std::sqrt(-2 * std::log(jmax(.001, (double) (hash >> 8) * 0x1p-24))) * std::cos((hash & 255) * MathConstants<double>::twoPi / 256) * (1.8 + scatter * hh * .27);
            const double y = jlimit(4.0, (double) hh - 4, mid - eventPitch / 24.0 * (hh / 2 - 3) * .3 + jitter);
            const int len = jmax(2, roundToInt(eventSize / 8 * ww));
            const double v = (double) (hash & 255) / 255, amp = shade == 0 ? jmax(.8, std::pow(v, 1.5)) : std::pow(v, 1.5); // mostly thin spindles
            for (int k = 0; k < len; ++k)
            {
                const int thickness = (int) (std::sin(MathConstants<double>::pi * (k + .5) / len) * amp * jmin(2.0, len / 6.0) + .35);
                const int x = (int) (dot(back) - len / 2.0 + k);
                for (int d = -thickness; d <= thickness; ++d)
                    if (const int row = roundToInt(y + d); isPositiveAndBelow(x, ww) && isPositiveAndBelow(row, hh))
                    {
                        auto& previous = shades[(size_t) (row * ww + x)];
                        if (8 - shade >= previous)
                            previous = (uint8_t) (8 - shade), g.dot(x, row, Colour(ramp[shade]));
                    }
            }
        }
        // The timeline: cells 0 .. 87 for 8 s .. now, labels breaking the rule.
        constexpr int span = 87;
        g.rule(0, span + 1, h);
        const char* labels[] { "8 s ", "6 s ", "4 s ", "2 s " };
        for (int i = 0; i < 4; ++i) g.put(py(i * span / 4.0), h, labels[i], grey);
        // 'now' is written over the rule in the renderer.
        g.put(span - 3, h, frozen ? "held" : " now", frozen ? red : grey);
        g.rule(span - 3, span + 1, h);
        const double scan = value("scan") / 100;
        const int numHeads = roundToInt(value("heads")) + 1;
        const double spray = value("scatter") * .02 + .006 * value("chaos");
        carets.fill(-1);
        for (int k = 0; k < numHeads; ++k)
        {
            const int x = carets[(size_t) k] = jlimit(4, span - 4, py(span * (1 - (pos + k * .25 * spray) / 8)));
            if (std::abs(scan - 1) > .0001)
                for (int n = py(6 * std::abs(1 - scan)) + 2, j = 1; j <= n; ++j)
                {
                    const int dx = 2 * x + 1 + (scan < 1 ? -2 : 2) * j;
                    if (isPositiveAndBelow(dx, 2 * (span + 1)))
                        g.dot(dx, 4 * h + 2, j < 3 ? heads[k] : dark, 9, under);
                }
        }
        for (int k = 0; k < numHeads; ++k)
        {
            const int x = carets[(size_t) k];
            if (g.at[g.oy + h][g.ox + x].line)
                g.put(x, h, caret, heads[k]);
        }
    }
};
// The inline value field: Return commits, Esc cancels, Tab / ⇧Tab commit and open the next / previous dial's field.
struct Field : TextEditor
{
    std::function<void(int)> tab, nudge;
    // Return, Esc and Tab run now. TextEditor posts them as command messages, which is too late for a cancel.
    bool keyPressed(const KeyPress& k) override
    {
        if (k.getKeyCode() == KeyPress::tabKey) { tab(k.getModifiers().isShiftDown() ? -1 : 1); return true; }
        if (k.getKeyCode() == KeyPress::upKey || k.getKeyCode() == KeyPress::downKey)
        {
            nudge(k.getKeyCode() == KeyPress::upKey ? 1 : -1);
            return true;
        }
        if (k.getKeyCode() == KeyPress::returnKey) { if (onReturnKey) onReturnKey(); return true; }
        if (k.getKeyCode() == KeyPress::escapeKey) { if (onEscapeKey) onEscapeKey(); return true; }
        return TextEditor::keyPressed(k);
    }
};
// A dial or fader (PLAN 4.1). It draws nothing: the grid shows it. Drag on the larger axis (faders: horizontal) over
// 250 screen px at any zoom, ⇧ 2,500 px; 3 px or less is a click and moves nothing. ⌥/⌘-click resets, double-click
// types. The wheel moves 1% a notch (⇧ 0.1%, stepped dials one step), a trackpad 250 px for the travel. Each drag,
// reset, typed value and wheel burst (ending 300 ms after its last event) is one host gesture.
struct Dial : Component, Timer
{
    SiltProcessor& p;
    RangedAudioParameter& q;
    ParameterAttachment attachment;
    const bool fader;
    std::function<double(double, int)> step; // a stepped dial's value one notch away
    Field field;
    double value = 0; // unsnapped through a drag or wheel burst
    bool dragging = false, reset = false;
    Point<float> last;
    Dial(SiltProcessor& proc, const char* id, bool horizontal)
        : p(proc), q(*proc.apvts.getParameter(id)), attachment(q, [](float) {}), fader(horizontal)
    {
        setComponentID(id), setTitle(q.name), setRepaintsOnMouseActivity(false), setWantsKeyboardFocus(true);
        setMouseCursor(horizontal ? MouseCursor::LeftRightResizeCursor : MouseCursor::UpDownResizeCursor);
        field.setColour(TextEditor::backgroundColourId, rule), field.setColour(TextEditor::textColourId, white);
        field.setColour(TextEditor::outlineColourId, {}), field.setColour(TextEditor::focusedOutlineColourId, {});
        field.setColour(TextEditor::highlightColourId, red.withAlpha(.4f)), field.setColour(TextEditor::highlightedTextColourId, white);
        field.setColour(CaretComponent::caretColourId, red), field.setJustification(fader ? Justification::centredRight : Justification::centred);
        field.setPopupMenuEnabled(false); // a modal edit menu would trap a host's GUI test
        field.onReturnKey = [this] { commit(); };
        field.onEscapeKey = [this] { field.setVisible(false); };
        field.onFocusLost = [this] { commit(true); };
        field.tab = [this](int direction) { if (commit()) next(direction).edit(); };
        field.nudge = [this](int direction) {
            if (const auto typed = p.parse(q.paramID, field.getText()))
            {
                const auto n = q.convertTo0to1(*typed);
                field.setText(q.getText((float) jlimit(0.0, 1.0, step ? step(n, direction) : n + direction * .01), 0));
                field.selectAll();
            }
        };
        addChildComponent(field);
    }
    ~Dial() override
    {
        if (isTimerRunning()) timerCallback(); // a wheel burst still ends if the editor closes mid-scroll
        else if (dragging) attachment.endGesture();
    }
    void resized() override { field.setBounds(getLocalBounds().removeFromBottom(roundToInt(ch)).removeFromRight(roundToInt(cw * 10))); }
    float scale() const { return jmax(.01f, getApproximateScaleFactorForComponent(this)); } // screen px per layer px
    void edit()
    {
        field.setColour(TextEditor::backgroundColourId, rule);
        field.setFont(mono(jmax(cw / 0.6f, 11 / scale()), false)), field.setText(q.getCurrentValueAsText(), false);
        field.setVisible(true), field.grabKeyboardFocus(), field.selectAll();
    }
    // Invalid text sets nothing: the field flashes red and stays open, unless it is closing anyway.
    bool commit(bool closing = false)
    {
        if (!field.isVisible()) return true;
        const auto typed = p.parse(q.paramID, field.getText());
        if (!typed && !closing)
        {
            field.setColour(TextEditor::backgroundColourId, red);
            Timer::callAfterDelay(300, [f = SafePointer<Field>(&field)] { if (f) f->setColour(TextEditor::backgroundColourId, rule); });
            return false;
        }
        field.setVisible(false);
        if (typed) attachment.setValueAsCompleteGesture(*typed); // nothing when unchanged
        return true;
    }
    Dial& next(int direction) // reading order over the visible dials, wrapping
    {
        auto* top = getTopLevelComponent();
        std::vector<Dial*> dials;
        std::function<void(Component&)> collect = [&](Component& c) {
            for (auto* child : c.getChildren())
                if (child->isVisible()) { if (auto* d = dynamic_cast<Dial*>(child)) dials.push_back(d); else collect(*child); }
        };
        collect(*top);
        const auto at = [top](Dial* d) { const auto o = top->getLocalPoint(d, Point<int>()); return std::pair(o.y, o.x); };
        std::sort(dials.begin(), dials.end(), [&](Dial* a, Dial* b) { return at(a) < at(b); });
        const int k = (int) (std::find(dials.begin(), dials.end(), this) - dials.begin()), n = (int) dials.size();
        return *dials[(size_t) ((k + direction + n) % n)];
    }
    void move(double delta) { value = jlimit(0.0, 1.0, value + delta), attachment.setValueAsPartOfGesture(q.convertFrom0to1((float) value)); }
    void mouseDown(const MouseEvent& e) override
    {
        if (isTimerRunning()) timerCallback();
        dragging = false;
        if (e.mods.isPopupMenu() && !e.mods.isCommandDown()) { reset = true; return; } // right-click is the menu's (4.1), never a drag
        if ((reset = e.mods.isAltDown() || e.mods.isCommandDown()))
            field.setVisible(false), attachment.setValueAsCompleteGesture(q.convertFrom0to1(q.getDefaultValue())); // ⌥/⌘, and ctrl where it is the command key
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (reset) return;
        const auto d = e.position - (dragging ? last : e.mouseDownPosition);
        const float along = (fader || std::abs(d.x) > std::abs(d.y) ? d.x : -d.y) * scale(); // screen px; faders ignore vertical
        if (!dragging)
        {
            if (std::abs(along) <= 3) return; // 3 screen px or less is a click
            dragging = true, value = q.getValue();
            attachment.beginGesture(), e.source.enableUnboundedMouseMovement(true); // the cursor hides; the drag never stops at the screen edge
        }
        last = e.position; // ⇧ applies from here on, so pressing it never jumps the value
        move(along / (e.mods.isShiftDown() ? 2500 : 250));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (dragging)
        {
            if (e.source.isUnboundedMouseMovementEnabled())
            {
                e.source.enableUnboundedMouseMovement(false); // show the cursor again; JUCE's setter is non-const
                const_cast<MouseInputSource&>(e.source).setScreenPosition(e.getMouseDownScreenPosition().toFloat());
            }
            attachment.endGesture();
        }
        dragging = false;
        if (e.mods.isPopupMenu() && !e.mods.isCommandDown() && e.getDistanceFromDragStart() <= 3)
        {
            const float def = q.getDefaultValue();
            PopupMenu menu;
            menu.addItem(1, "type value...");
            menu.addItem(2, "reset to " + q.getText(def, 0));
            menu.addItem(3, "copy value");
            menu.addItem(4, "paste value");
            menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), [self = SafePointer<Dial>(this), def](int item) {
                if (!self) return;
                if (item == 1) self->edit();
                else if (item == 2) self->attachment.setValueAsCompleteGesture(self->q.convertFrom0to1(def));
                else if (item == 3) SystemClipboard::copyTextToClipboard(self->q.getCurrentValueAsText());
                else if (item == 4) self->applyText(SystemClipboard::getTextFromClipboard());
            });
        }
    }
    void mouseDoubleClick(const MouseEvent& e) override { if (!e.mods.isAltDown() && !e.mods.isCommandDown() && !e.mods.isPopupMenu()) edit(); }
    void mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (dragging || field.isVisible()) return; // never a second gesture inside a drag, nor under its own open field
        const float delta = (std::abs(w.deltaY) > 0 ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f); // up / right increases
        if (std::abs(delta) <= 0) return;
        if (!isTimerRunning()) value = q.getValue(), attachment.beginGesture();
        const bool fine = e.mods.isShiftDown();
        if (w.isSmooth) move(delta * 512 / (fine ? 2500 : 250)); // JUCE's precise deltas are px / 512
        else if (step && !fine) value = step(value, delta > 0 ? 1 : -1), move(0);
        else move((delta > 0 ? 1 : -1) * (fine ? .001 : .01));
        startTimer(300);
    }
    void applyText(const String& text)
    {
        if (const auto typed = p.parse(q.paramID, text))
        {
            if (isTimerRunning()) timerCallback();
            attachment.setValueAsCompleteGesture(*typed);
        }
    }
    std::unique_ptr<AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Value : AccessibilityTextValueInterface
        {
            explicit Value(Dial& d) : dial(d) {}
            bool isReadOnly() const override { return false; }
            String getCurrentValueAsString() const override { return dial.q.getCurrentValueAsText(); }
            void setValueAsString(const String& text) override { dial.applyText(text); }
            Dial& dial;
        };
        return std::make_unique<AccessibilityHandler>(*this, AccessibilityRole::slider, AccessibilityActions(),
            AccessibilityHandler::Interfaces(std::unique_ptr<AccessibilityValueInterface>(new Value(*this))));
    }
    bool keyPressed(const KeyPress& k) override
    {
        const int code = k.getKeyCode();
        if (code == KeyPress::escapeKey) { giveAwayKeyboardFocus(); return true; }
        if (code == KeyPress::returnKey)
        {
            if (isTimerRunning()) timerCallback();
            edit();
            return true;
        }
        if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey)
        {
            if (isTimerRunning()) timerCallback();
            attachment.setValueAsCompleteGesture(q.convertFrom0to1(q.getDefaultValue()));
            return true;
        }
        if (code == KeyPress::upKey || code == KeyPress::rightKey || code == KeyPress::downKey || code == KeyPress::leftKey)
        {
            if (dragging) return true;
            if (!isTimerRunning()) value = q.getValue(), attachment.beginGesture();
            move((code == KeyPress::upKey || code == KeyPress::rightKey ? 1 : -1)
                 * (k.getModifiers().isShiftDown() ? .001 : k.getModifiers().isCommandDown() ? .1 : .01));
            startTimer(300);
            return true;
        }
        const auto chTyped = k.getTextCharacter();
        if (!k.getModifiers().isCommandDown() && (CharacterFunctions::isDigit(chTyped) || chTyped == '+' || chTyped == '-' || chTyped == '.' || chTyped == 0x2212))
        {
            edit();
            field.setText(String::charToString(chTyped));
            return true;
        }
        return false;
    }
    void timerCallback() override { stopTimer(), attachment.endGesture(); }
};
struct DialSpec { const char *id, *name; int x, y; bool centred = false; std::function<String()> idle = {}; bool named = true; }; // idle: why it does nothing now
struct ListSpec { const char *id, *name; int x, y, cols = 1, colw = 0; bool row = false; }; // row: a dial's sub-choice, items side by side
// A set of dials (or macro faders) and choice lists at absolute cells, plus any extra marks.
struct Controls : Group
{
    std::vector<DialSpec> specs;
    std::vector<ListSpec> lists;
    bool fader;
    std::function<void(Grid&)> extra;
    OwnedArray<Dial> dials;
    Controls(SiltProcessor& proc, Panel panel, std::initializer_list<DialSpec> ds, std::initializer_list<ListSpec> ls = {}, bool horizontal = false)
        : Group(proc, panel), specs(ds), lists(ls), fader(horizontal)
    {
        for (const auto& d : specs)
        {
            auto& k = *dials.add(new Dial(p, d.id, fader));
            addAndMakeVisible(k), k.setBounds(fader ? cells(d.x, d.y, 22, 1) : cells(d.x, d.y + !d.named, 11, 5 - !d.named));
        }
        for (const auto& l : lists)
            place(l, [this, &l](int i, int x, int y, const String& item, int w) {
                const bool leftPad = !l.row && x != 3 && x != 25;
                const int left = x - (int) leftPad;
                const int width = l.row ? item.length() + (i == 0 ? 1 : 0) : w + 1 + (int) leftPad;
                word(l.id, i, left, y, width);
            });
    }
    // Each item's cell: a row puts them side by side, centred in the module; a list fills `cols` columns top-down.
    template <typename F> void place(const ListSpec& l, F&& at)
    {
        const auto items = p.apvts.getParameter(l.id)->getAllValueStrings();
        int widest = 0;
        for (auto& s : items) widest = jmax(widest, s.length());
        const int rows = (items.size() + l.cols - 1) / l.cols, colw = l.colw ? l.colw : widest + 2;
        int x = l.x + (11 - items.joinIntoString(" ").length()) / 2;
        for (int i = 0; i < items.size(); x += items[i].length() + 1, ++i)
            l.row ? at(i, x, l.y, items[i], 0) : at(i, l.x + i / rows * colw, l.y + 1 + i % rows, items[i], widest);
    }
    void draw(Grid& g) override
    {
        for (int i = 0; i < dials.size(); ++i)
        {
            const auto& d = specs[(size_t) i];
            auto& dial = *dials[i];
            auto& q = dial.q;
            const bool hovering = dial.isMouseOver();
            const bool focused = dial.hasKeyboardFocus(false);
            if (!fader)
            {
                auto text = q.getCurrentValueAsText().replaceCharacter('-', 0x2212);
                if (q.paramID == "centre" && text.endsWith("0 kHz"))
                    text = text.upToFirstOccurrenceOf(" ", false, false).trimCharactersAtEnd("0").trimCharactersAtEnd(".") + " kHz";
                knob(g, d.x, d.y, q.getValue(), d.name, text, d.centred,
                     !hovering && d.idle && d.idle().isNotEmpty(), d.named, focused, hovering ? q.getDefaultValue() : -1);
            }
            else
            {
                g.put(d.x, d.y, d.name, grey);
                for (int tick = 0; tick < 18; ++tick) g.dot(2 * (d.x + 8) + tick, 4 * d.y + 2, tick <= q.getValue() * 18 ? red : dark, 9);
                for (int j = 0; j < 4; ++j) g.dot(2 * (d.x + 8) + py(q.getValue() * 17), 4 * d.y + j, white);
                if (hovering) g.dot(2 * (d.x + 8) + py(q.getDefaultValue() * 17), 4 * d.y + 2, white);
                g.right(d.x + 21, d.y, q.getCurrentValueAsText(), white, true);
                if (focused) g.underline(d.x, (int) strlen(d.name), d.y);
            }
        }
        int keyIndex = 0;
        for (const auto& l : lists)
        {
            if (!l.row) g.put(l.x, l.y, l.name, grey);
            const int active = roundToInt(value(l.id));
            place(l, [&](int i, int x, int y, const String& item, int) {
                auto& key = *keys[keyIndex++];
                key.active = active == i;
                g.put(x, y, item, key.active ? red : key.isMouseOver() ? white : light, key.active);
                if (key.hasKeyboardFocus(false)) g.underline(x, item.length(), y);
            });
        }
        if (extra) extra(g);
    }
};
// A section title: white, a rule to the section's end, and the flow mark before it (v6 SPACING).
void title(Grid& g, int x, int y, const char* name, int end, const char* mark)
{
    if (*mark) g.put(x - 2, y, u(mark), grey);
    g.put(x, y, name, white), g.rule(x + (int) strlen(name) + 1, end, y);
}
struct Editor : AudioProcessorEditor, Timer
{
    SiltProcessor& proc;
    Component layer;
    Header header;
    Cloud cloud;
    Controls tier1, tier2, macros;
    Freeze freeze;
    std::unique_ptr<Grid> next { new Grid }, shown { new Grid };
    std::array<float, silt::numParams> drawnParams;
    std::tuple<juce::int64, juce::int64, juce::int64, int, float, bool> drawnCloud {};
    std::array<int, 3> drawnMeters { -1, -1, -1 };
    int drawnNote = -1, drawnSlot = -1;
    bool drawnHeld = false;
    Component::SafePointer<Component> drawnFocus;
    String drawnName;
    Component::SafePointer<Component> hovered;
    uint32 hintUntil = 0;
    Image image;
    std::unique_ptr<Graphics> imageGraphics;
    double pixelCell = designCell;
    Font fonts[2] { mono(1, false), mono(1, true) };
    explicit Editor(SiltProcessor& p) : AudioProcessorEditor(p), proc(p), header(p), cloud(p),
        tier1(p, panels[2], { { "size", "size", 20, 20 }, { "density", "density", 31, 20, false, [this] { return tier1.value("trigger") > .5f ? String("idle while trigger = ") + (tier1.value("trigger") > 1.5f ? "sidechain" : "transient") : String(); } },
                              { "position", "position", 20, 27 }, { "scan", "scan", 31, 27, true }, { "scatter", "scatter", 42, 27 },
                              { "shape", "shape", 70, 20 }, { "tilt", "tilt", 81, 20, true }, { "reverse", "reverse", 70, 27 }, { "drop", "drop", 81, 27 }, { "repeat", "repeat", 92, 27 } },
              { { "trigger", "trigger", 3, 20 }, { "sizeSync", "", 20, 25, 1, 0, true }, { "lock", "lock", 44, 20 }, { "onset", "onset", 56, 20 }, { "sprayDir", "spray", 56, 27 } }),
        tier2(p, panels[3], { { "pitch", "pitch", 3, 36, true }, { "detune", "detune", 14, 36 }, { "band", "band", 42, 36 },
                              { "centre", "centre", 53, 36, false, [this] { return tier2.value("band") <= 0 && tier2.value("spectra") <= 0 ? String("idle while band and spectra = 0") : String(); } },
                              { "spectra", "spectra", 42, 43 }, { "filter", "filter", 53, 43, true }, { "width", "width", 70, 36 },
                              { "reverb", "reverb", 81, 36, false, [this] { return tier2.value("reverbOn") < .5f ? String("idle while reverb is off") : String(); }, false },
                              { "feedback", "feedback", 81, 43 }, { "mix", "mix", 92, 36 }, { "output", "output", 92, 43, true } },
              { { "pitchMode", "", 3, 41, 1, 0, true }, { "scale", "scale", 25, 36 }, { "heads", "heads", 6, 43 }, { "harmony", "harmony", 16, 43, 2, 9 }, { "space", "stereo", 71, 43 } }),
        macros(p, panels[4], { { "motion", "motion", 3, 3 }, { "texture", "texture", 29, 3 }, { "bloom", "bloom", 55, 3 }, { "chaos", "chaos", 81, 3 } }, {}, true),
        freeze(p)
    {
        drawnParams.fill(-1);
        tier1.extra = [this](Grid& g) {
            title(g, 3, 18, "input", 17, ""), title(g, 20, 18, "grain", 67, "→"), title(g, 70, 18, "window", 103, "→");
            // The window's shape: bell to flat top, its peak moved by tilt (v6 lib.window_glyph), filled down to the base.
            const double flat = tier1.value("shape") / 100, peak = .5 + .4 * tier1.value("tilt") / 100;
            for (int i = 0; i < 16; ++i)
            {
                const double t = i / 15.0, s = t < peak ? t / peak : (1 - t) / (1 - peak), q = jmin(1.0, s / (1 - flat * .85));
                const int top = py((.5 + .5 * std::cos(MathConstants<double>::pi * q)) * 11);
                g.dot(188 + i, 84 + top, red);
                for (int y = top + 2; y <= 11; y += 2) g.dot(188 + i, 84 + y, rule, 8);
            }
        };
        auto& window = tier1.key("window-glyph", 94, 21, 8, [this] { dial("shape")->grabKeyboardFocus(); });
        window.setBounds(cells(94, 21, 8, 3)), window.setTitle("grain window shape");
        tier2.toggle("reverbOn", "reverb", 83, 36);
        cloud.focusHead = [this](int index) {
            const auto id = "heads:" + String(index);
            for (auto* k : tier2.keys) if (k->getComponentID() == id) k->grabKeyboardFocus();
        };
        tier2.extra = [this](Grid& g) {
            g.put(1, 34, u("↳"), grey), title(g, 3, 34, "voices", 39, ""), title(g, 42, 34, "colour", 67, "→");
            title(g, 70, 34, "space", 89, "→"), title(g, 92, 34, "out", 103, "→");
            auto& key = *tier2.keys.getLast();
            g.toggle(83, 36, "reverb", key.active = tier2.value("reverbOn") > .5f);
            if (key.isMouseOver() && !key.active) g.put(83, 36, emptyBox, white);
            if (key.hasKeyboardFocus(false)) g.underline(85, 6, 36);
        };
        setOpaque(true), addAndMakeVisible(layer), addMouseListener(this, true), layer.setBounds(0, 0, W, H);
        for (auto* group : groups()) layer.addAndMakeVisible(group);
        // Stepped wheels: pitch by a semitone, size in sync by a note value.
        auto& pitch = *dial("pitch");
        pitch.step = [q = pitch.q.getNormalisableRange()](double n, int d) { return (double) q.convertTo0to1(jlimit(q.start, q.end, std::round(q.convertFrom0to1((float) n)) + (float) d)); };
        dial("size")->step = [this](double n, int d) {
            if (tier1.value("sizeSync") > .5f) return n + d * .01;
            const int k = jlimit(0, silt::numNotes - 1, roundToInt(std::pow(n, 1.6) * (silt::numNotes - 1)) + d); // Silt.cpp's syncIndex
            return std::pow((double) k / (silt::numNotes - 1), 1 / 1.6); };
        // Saved sizes count 0.1.0's 1024-px-wide window, so old sessions reopen at the same zoom.
        const int stored = proc.editorWidth;
        setResizable(true, false), setResizeLimits(W / 2, H / 2, W * 2, H * 2); getConstrainer()->setFixedAspectRatio((double) W / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, roundToInt(stored * (double) W / 1024)) : W;
        setSize(width, roundToInt((double) width * H / W)), timerCallback(), startTimerHz(30);
    }
    std::array<Controls*, 3> controls() { return { &tier1, &tier2, &macros }; }
    Dial* dial(const char* id) { for (auto* c : controls()) for (auto* d : c->dials) if (d->getComponentID() == id) return d; return nullptr; }
    std::array<Group*, 6> groups() { return { &header, &cloud, &tier1, &tier2, &macros, &freeze }; }
    // A click anywhere but in the open field, or a resize, commits it.
    void commitFields(const Component* except = nullptr) { for (auto* c : controls()) for (auto* d : c->dials) if (except != &d->field && !d->field.isParentOf(except)) d->commit(true); }
    void mouseDown(const MouseEvent& e) override
    {
        commitFields(e.eventComponent);
        if (e.eventComponent == this || e.eventComponent == &layer || (e.eventComponent == &cloud && e.y < cells(0, 11, 1, 1).getY()))
            giveAwayKeyboardFocus();
    }
    // The hint line: what is under the pointer, until 1.5 s after it leaves.
    void mouseEnter(const MouseEvent& e) override { mouseMove(e); }
    void mouseMove(const MouseEvent& e) override
    {
        if (dynamic_cast<Dial*>(e.eventComponent) || dynamic_cast<Key*>(e.eventComponent) || e.eventComponent == &cloud)
            hovered = e.eventComponent, hintUntil = 0;
    }
    void mouseExit(const MouseEvent& e) override { if (hovered == e.eventComponent) hintUntil = Time::getMillisecondCounter() + 1500; }
    String hint()
    {
        const auto time = Time::getMillisecondCounter();
        if (header.notice.isNotEmpty() && time - header.noticeTime < 2000) return header.notice;
        if (hovered == nullptr || (hintUntil != 0 && Time::getMillisecondCounter() > hintUntil)) return hovered = nullptr, String();
        if (auto* d = dynamic_cast<Dial*>(hovered.getComponent()))
        {
            String idle;
            for (auto* c : controls()) for (auto& s : c->specs) if (d->getComponentID() == s.id && s.idle) idle = s.idle();
            return d->q.name.toLowerCase() + " " + d->q.getCurrentValueAsText() + (idle.isNotEmpty() ? u(" · ") + idle : u(" · drag · shift fine · alt/cmd reset · double-click types"));
        }
        if (dynamic_cast<Key*>(hovered.getComponent())) return hovered->getTitle().toLowerCase() + u(" · click") + (hovered->getComponentID().contains(":") ? u(" · alt/cmd reset · wheel") : String());
        if (hovered == &cloud)
        {
            const auto& f = proc.feed;
            const auto live = std::count_if(f.env.begin(), f.env.end(), [](const auto& v) { return v.load() > 0; });
            return String(f.centre.load(), 2) + u(" s back · ") + String(f.lastPitch.load(), 1) + u(" st · ")
                   + String(live) + u(" grains · double-click freezes · drag timeline");
        }
        return {};
    }
    void resized() override
    {
        commitFields();
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = roundToInt(getWidth() * 1024.0 / W);
    }
    void timerCallback() override
    {
        auto& f = proc.feed;
        const auto& parameters = proc.getParameters();
        auto& g = *next;
        bool changed = false;
        for (int i = 0; i < parameters.size(); ++i)
        {
            const auto value = parameters[i]->getValue();
            changed |= std::abs(drawnParams[(size_t) i] - value) > 0;
            drawnParams[(size_t) i] = value;
        }
        const auto frame = std::tuple { f.time.load(), f.events.load(), f.captureHead.load(), f.at.load(), f.centre.load(), f.frozen.load() };
        if (changed || frame != drawnCloud)
        {
            drawnCloud = frame;
            g.clear(3, 5, 100, 11), g.clear(3, 16, 92, 1);
            g.ox = 3, g.oy = 5;
            cloud.draw(g);
            g.ox = g.oy = 0;
            show(3, 5, 100, 11), show(3, 16, 92, 1);
        }
        const auto previousHint = header.hint;
        const auto line = hint();
        auto* focused = Component::getCurrentlyFocusedComponent();
        if (!isParentOf(focused)) focused = nullptr;
        const bool focusChanged = drawnFocus != focused;
        drawnFocus = focused;
        const int preset = proc.preset, slot = proc.abSlot();
        const auto name = preset < 0 ? proc.presetName() : String();
        if (changed || focusChanged || header.program != preset || name != drawnName || slot != drawnSlot || line != header.hint)
        {
            drawnName = name, drawnSlot = slot, header.hint = line;
            header.place(preset, name);
            g.clear(0, 0, C, 3), g.clear(0, 4, C, 1);
            header.draw(g), show(0, 0, C, 3), show(0, 4, C, 1);
        }
        const int note = f.note;
        const bool held = f.held;
        const bool drawTiers = changed || focusChanged || line != previousHint;
        if (drawTiers)
        {
            g.clear(95, 16, 8, 1), freeze.draw(g);
            if (freeze.keys[0]->hasKeyboardFocus(false)) g.underline(97, 6, 16);
            show(95, 16, 8, 1);
        }
        if (drawTiers)
        {
            g.clear(0, 3, C, 1), macros.draw(g), show(0, 3, C, 1);
            g.clear(0, 18, C, 31), tier1.draw(g), tier2.draw(g);
        }
        if (drawTiers || note != drawnNote || held != drawnHeld)
        {
            drawnNote = note, drawnHeld = held;
            g.clear(3, 27, 11, 1);
            g.put(3, 27, "midi", grey), g.put(8, 27, u("●"), held ? red : dark);
            g.put(10, 27, MidiMessage::getMidiNoteName(note, true, true, 3).toLowerCase(), white, true);
            if (!drawTiers) show(3, 27, 11, 1);
        }
        for (int c = 0; c < 3; ++c)
        {
            const bool sc = c == 2;
            const int steps = sc ? 10 : 12;
            const int lit = roundToInt(jlimit(0.0f, 1.0f, f.meter[(size_t) c].load() * 3) * steps);
            if (!drawTiers && lit == drawnMeters[(size_t) c]) continue;
            drawnMeters[(size_t) c] = lit;
            if (sc) g.put(3, 28, "sc", grey);
            for (int k = 0; k < steps; ++k)
            {
                if (sc) g.dot(16 + k, 114, k < lit ? grey : dark, 9);
                else g.dot(202 + 2 * c, 187 - k, k < lit ? grey : k < 9 ? dark : rule, 10);
            }
            if (!drawTiers)
            {
                if (sc) show(8, 28, 5, 1);
                else show(101 + c, 44, 1, 3);
            }
        }
        if (drawTiers) show(0, 18, C, 31);
    }
    // Redraw and invalidate only the changed cells.
    void show(int x, int y, int w, int h)
    {
        const double sx = (double) getWidth() / C, sy = (double) getHeight() / R;
        for (int j = y; j < y + h; ++j)
            for (int i = x, first = -1, last = -1; i <= x + w; ++i)
            {
                if (i < x + w && next->at[j][i] != shown->at[j][i])
                {
                    shown->at[j][i] = next->at[j][i], first = first < 0 ? i : first, last = i;
                    if (imageGraphics) drawCell(*imageGraphics, i, j);
                }
                else if (first >= 0 && (i == x + w || i - last > 8))
                {
                    repaint(Rectangle<double>(sx * first, sy * j, sx * (last + 1 - first), sy).getSmallestIntegerContainer().expanded(1));
                    first = -1;
                }
            }
    }
    void paint(Graphics& g) override
    {
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const int iw = jmax(1, roundToInt(getWidth() * scale)), ih = jmax(1, roundToInt(getHeight() * scale));
        if (image.getWidth() != iw || image.getHeight() != ih)
        {
            imageGraphics.reset();
            image = Image(Image::RGB, iw, ih, false);
            imageGraphics = std::make_unique<Graphics>(image);
            pixelCell = (double) iw / C;
            for (const double zoom : { 1.0, 2.0, 4.0 })
                if (std::abs(iw - zoom * C * designCell) <= 1 && std::abs(ih - zoom * R * 2 * designCell) <= 1)
                    pixelCell = zoom * designCell;
            const float em = (float) std::round(pixelCell / .6) * 1.02f;
            fonts[0] = mono(em, false), fonts[1] = mono(em, true);
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < C; ++i)
                    drawCell(*imageGraphics, i, j);
        }
        g.drawImageTransformed(image, AffineTransform::scale((float) getWidth() / (float) iw, (float) getHeight() / (float) ih));
    }
    void drawCell(Graphics& g, int i, int j)
    {
        const auto& c = shown->at[j][i];
        const double w = pixelCell, h = 2 * w;
        const auto r = Rectangle<int>::leftTopRightBottom(py(i * w), py(j * h),
            i == C - 1 ? image.getWidth() : py((i + 1) * w), j == R - 1 ? image.getHeight() : py((j + 1) * h));
        g.setColour(c.bg.isTransparent() ? page : c.bg), g.fillRect(r);
        if (c.line)
        {
            g.setColour(rule);
            g.fillRect(r.getX(), py((j + .5) * h), r.getWidth(), jmax(1, (int) (w / (2 * cw))));
        }
        for (size_t k = 0; k < c.dots.size(); ++k)
            if (const auto d = c.dots[k])
            {
                const double radius = jmax(.6, w * (double) (d >> 24) / 100);
                const double x = (2 * i + (double) (k % 2) + .5) * w / 2, y = (4 * j + (double) (k / 2) + .5) * h / 4;
                const int x0 = (int) std::floor(x - radius), x1 = (int) std::floor(x + radius);
                const int y0 = (int) std::floor(y - radius), y1 = (int) std::floor(y + radius);
                g.setColour(Colour(0xff000000 | (d & 0xffffff)));
                const double a = (x1 - x0 + 1) / 2.0, b = (y1 - y0 + 1) / 2.0;
                for (int row = y0; row <= y1; ++row)
                {
                    const int edge = jmin(row - y0, y1 - row);
                    // At the reference scales the hard-edged dots are 2–6 pixels across.
                    int inset = 0;
                    if (x1 - x0 >= 2 && y1 - y0 >= 2)
                    {
                        if (x1 - x0 <= 5)
                            inset = edge == 0 ? (x1 - x0 == 5 && y1 - y0 >= 5 ? 2 : 1)
                                              : (edge == 1 && x1 - x0 == 5 && y1 - y0 >= 5 ? 1 : 0);
                        else
                        {
                            const double v = (row + .5 - y0 - b) / b;
                            inset = roundToInt(a * (1 - std::sqrt(1 - v * v)));
                        }
                    }
                    g.fillRect(r.getIntersection({ x0 + inset, row, x1 - x0 + 1 - 2 * inset, 1 }));
                }
            }
        if (c.c == ' ') return;
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r), g.setColour(c.fg);
        const float fw = (float) w, fh = (float) h, cx = (float) ((i + .5) * w), cy = (float) ((j + .55) * h), lw = jmax(1.0f, fw * .12f);
        if (c.c == 0x25cf)
            g.fillEllipse(Rectangle<float>(.92f * fw, .92f * fw).withCentre({ cx, cy - .03f * fh }));
        else if (c.c == 0x25a0 || c.c == 0x25a1)
        {
            const float side = .92f * fw;
            const auto box = Rectangle<float>(side, side).withCentre({ cx, cy + .07f * fh });
            if (c.c == 0x25a0) g.fillRect(box);
            else g.drawRect(box, lw);
        }
        else if (c.c == 0x25b2)
        {
            const int top = py((j + .41) * h), size = jmax(1, py(.94 * w)), x = py((i + .5) * w);
            for (int row = 0; row < size; ++row)
            {
                const int half = (row * size / (2 * jmax(1, size - 1)));
                g.fillRect(x - half, top + row, 2 * half + 1, 1);
            }
        }
        else if (c.c == 0x2039 || c.c == 0x203a || c.c == 0x21b3 || c.c == 0xd7) // ‹ › ↳ × (not in Geist Mono)
        {
            const float side = c.c == 0x2039 ? 1.0f : -1.0f, a = (c.c == 0xd7 ? .26f : .14f) * fw, b = .14f * fh;
            Path p;
            if (c.c == 0x21b3) p.startNewSubPath(cx - 1.6f * a, cy - 1.8f * b), p.lineTo(cx - 1.6f * a, cy + .6f * b), p.lineTo(cx + 2 * a, cy + .6f * b),
                               p.startNewSubPath(cx + a, cy - .2f * b), p.lineTo(cx + 2 * a, cy + .6f * b), p.lineTo(cx + a, cy + 1.4f * b);
            else if (c.c == 0xd7) p.startNewSubPath(cx - a, cy - a), p.lineTo(cx + a, cy + a), p.startNewSubPath(cx + a, cy - a), p.lineTo(cx - a, cy + a);
            else p.startNewSubPath(cx + side * a, cy - b), p.lineTo(cx - side * a, cy + 0.02f * fh), p.lineTo(cx + side * a, cy + 0.18f * fh);
            g.strokePath(p, PathStrokeType(lw));
        }
        else if (const auto id = fonts[c.bold ? 1 : 0].getTypefacePtr()->getNominalGlyphForCodepoint(c.c))
        {
            // Geist Mono: advance 0.6 em, ascent 1.005 + descent 0.295 em; centred as the mockup's (0.55 of the cell, measured).
            const float size = std::round(fw / 0.6f);
            const uint16_t index = (uint16_t) *id;
            const Point<float> at { (float) i * fw + (fw - .6f * size) / 2, ((float) j + .55f) * fh + .355f * size };
            g.setFont(fonts[c.bold ? 1 : 0]);
            g.getInternalContext().drawGlyphs({ &index, 1 }, { &at, 1 }, {});
        }
    }
};
} // namespace
AudioProcessorEditor* SiltProcessor::createEditor() { return new Editor(*this); }
