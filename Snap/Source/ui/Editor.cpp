// snap's one-screen editor (approved v3 layout, 1440 x 840 logical, scaled 75-200 %). Coordinates are the mockup's:
// top bar, six module columns in chain order on one shared band grid (Layout.h), the amount row, the master strip of
// seven groups on one centre line, and the bottom bar; the preset browser overlays the columns only. Controls draw
// themselves; each zone paints its own labels and readouts. A 30 Hz timer feeds the visualizers and meters and
// repaints the static layer only when something it shows has changed.
#include "../PluginProcessor.h"
#include "../dsp/modules/ShiftConstants.h"
#include "Braille.h"
#include "Layout.h"
#include "PresetBrowser.h"
#include "Riso.h"
#include "Visualizers.h"
#include "Widgets.h"

using namespace juce;

namespace snap::ui
{
namespace
{
using namespace colours;
using namespace layout;
constexpr int W = 1440, H = 840;
constexpr float CW = colW, YK = bandKnobs, YD = bandDetail, Yc = masterYc;
const String defaultTip = u("magnitude scales all six modules and master at once \xc2\xb7 alt-click any dial to reset, double-click to type");

// A component that paints through a lambda: the fixed zones and the amount cells.
struct Canvas : Component
{
    std::function<void(Graphics&)> draw;
    void paint(Graphics& g) override
    {
        const Lattice lattice(*this);
        if (draw)
            draw(g);
    }
};

// The module column's static parts and every widget in it, in column-local coordinates.
struct Column : Component
{
    Module module;
    std::unique_ptr<Visualizer> viz;
    OwnedArray<Component> parts;
    struct Caption
    {
        String s;
        float x, y, w;
        bool operator==(const Caption&) const = default;
    };
    std::vector<Caption> captions;       // static 11 px labels; w > 0: right-aligned in [x, x + w]
    std::function<void(Graphics&)> body; // live labels and readouts
    void caption(const String& s, float x, float y, float w = 0) // sub and hiss share theirs
    {
        if (const Caption c { s, x, y, w }; std::find(captions.begin(), captions.end(), c) == captions.end())
            captions.push_back(c);
    }
    bool dim = false;
    float headerX() const { return module == Module::subHiss || module == Module::push ? 0.0f : 14.0f; }
    void paint(Graphics& g) override
    {
        const Lattice lattice(*this);
        const float hx = headerX();
        const auto name = String(modules[(size_t) module].name.data());
        if (hx > 0)
            handle(g, 0, 10, dimmed(muted, dim));
        moduleGlyph(g, module, hx, 7, dimmed(accent, dim));
        text(g, name, hx + 16, 7, dimmed(primary, dim), 13, true);
        const float rx = std::ceil(hx + 16 + textWidth(name, 13) + 10);
        hline(g, rx, 16, CW - rx);
        for (auto& c : captions)
            text(g, c.s, c.x, c.y, dimmed(muted, dim), 11, false, c.w, c.w > 0 ? Justification::right : Justification::left);
        if (body)
            body(g);
    }
};

// Drags a middle column sideways to reorder the chain.
struct Grip : Component
{
    std::function<void(float)> drag; // the horizontal offset from the press
    std::function<void()> drop;
    Grip() { setMouseCursor(MouseCursor::DraggingHandCursor), setTitle("drag to reorder"); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (drag)
            drag((float) e.getDistanceFromDragStartX());
    }
    void mouseUp(const MouseEvent&) override
    {
        if (drop)
            drop();
    }
};

// space's pan / width pad: x is pan, y is width (top +100 %).
struct Pad : Component
{
    RangedAudioParameter &pan, &width;
    SnapProcessor& proc;
    bool dim = false, held = false;
    Pad(SnapProcessor& p) : pan(*p.apvts.getParameter("space_pan")), width(*p.apvts.getParameter("space_width")), proc(p)
    {
        setTitle("space pan and width"), setMouseCursor(MouseCursor::CrosshairCursor);
    }
    void paint(Graphics& g) override
    {
        const Lattice lattice(*this);
        {
            Braille b(g);
            for (float x = 3; x < 141; x += 6)
                for (float y = 3; y < 40; y += 6)
                    b.dot(x, y, dimmed(colours::dim, dim));
        }
        vline(g, 69, 0, 40), hline(g, 0, 20, 137);
        led(g, Rectangle<float>(6, 6).withCentre({ 3 + 132 * pan.getValue(), 37 - 32 * width.getValue() }), dimmed(primary, dim));
    }
    void mouseDown(const MouseEvent& e) override
    {
        proc.beginUndoStep();
        pan.beginChangeGesture(), width.beginChangeGesture();
        held = !e.mods.isAltDown() && !e.mods.isCommandDown();
        if (held)
            return mouseDrag(e);
        set(0, 100);
        pan.endChangeGesture(), width.endChangeGesture();
    }
    void set(float panValue, float widthValue)
    {
        pan.setValueNotifyingHost(pan.convertTo0to1(jlimit(-100.0f, 100.0f, panValue)));
        width.setValueNotifyingHost(width.convertTo0to1(jlimit(-100.0f, 100.0f, widthValue)));
        repaint();
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (held)
            set((e.position.x - 3) / 132 * 200 - 100, (37 - e.position.y) / 32 * 200 - 100);
    }
    void mouseUp(const MouseEvent&) override
    {
        if (std::exchange(held, false))
            pan.endChangeGesture(), width.endChangeGesture();
    }
};

struct Editor : AudioProcessorEditor, Timer
{
    SnapProcessor& proc;
    PresetStore store;
    Canvas layer;
    std::array<Column, 6> columns;
    std::array<Canvas, 6> cells; // the amount row
    OwnedArray<Component> parts;    // layer-level widgets
    PresetBrowser browser;
    std::vector<float> drawnParams;
    String tip = defaultTip, notice, drawnState;
    uint32 noticeUntil = 0;
    int tab = 0, snapshot = 0, dragging = -1;
    unsigned lastBlocks = ~0u;
    float lufsIn = -100, lufsOut = -100, meterIn = 0, meterOut = 0;
    Component::SafePointer<Component> hovered;
    std::unique_ptr<AlertWindow> saveDialog;

    explicit Editor(SnapProcessor& p)
        : AudioProcessorEditor(p), proc(p), browser(p, store, [this](const Preset& preset) { load(preset); })
    {
        setOpaque(true), setFocusContainerType(FocusContainerType::keyboardFocusContainer), setTitle("snap");
        addAndMakeVisible(layer), layer.setBounds(0, 0, W, H), layer.setInterceptsMouseClicks(false, true);
        buildTopBar();
        for (int m = 0; m < 6; ++m)
            buildColumn((Module) m), buildAmount((Module) m);
        buildMaster();
        buildBottomBar();
        layer.draw = [this](Graphics& g) { paintLayer(g); };
        layer.addChildComponent(browser), browser.setBounds(24, 58, 1392, 434);
        browser.onClose = [this] { browser.setVisible(false), repaintAll(); };
        place();
        const double scale = jlimit(0.75, 2.0, (double) proc.apvts.state.getProperty("ui_scale", 1.0));
        setResizable(true, true), setResizeLimits(W * 3 / 4, H * 3 / 4, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio((double) W / H);
        setSize(roundToInt(W * scale), roundToInt(H * scale));
        startTimerHz(30);
    }
    ~Editor() override { stopTimer(); }

    // ---- helpers ------------------------------------------------------------------------------------------------
    float raw(const char* id) const { return proc.apvts.getRawParameterValue(id)->load(); }
    bool on(const char* id) const { return raw(id) > 0.5f; }
    String show(const char* id) const { return format(spec(id), raw(id)); }
    void setParam(const char* id, float hostValue)
    {
        auto* q = proc.apvts.getParameter(id);
        proc.beginUndoStep();
        q->beginChangeGesture(), q->setValueNotifyingHost(q->convertTo0to1(hostValue)), q->endChangeGesture();
    }
    float defaultOf(const char* id) const { return spec(id).def; }
    template <typename T> T& add(Component& parent, T* c)
    {
        parent.addAndMakeVisible(c);
        if (&parent == &layer)
            parts.add(c);
        else if (auto* column = dynamic_cast<Column*>(&parent))
            column->parts.add(c);
        else
            parts.add(c);
        return *c;
    }
    Control& control(Component& parent, Control::Kind kind, const char* id, Rectangle<float> r, bool centredValue = false, const char* second = nullptr)
    {
        auto& c = add(parent, new Control(proc, kind, id, second));
        c.centred = centredValue, c.setBounds(r.toNearestInt());
        return c;
    }
    Control& dial(Component& parent, const char* id, const char* label, float x, float y, bool centredValue = false)
    {
        auto& c = control(parent, Control::Kind::dial, id, { x, y, 64, 76 }, centredValue);
        c.label = label;
        return c;
    }
    Key& key(Component& parent, const String& title, Rectangle<float> r, std::function<void(Graphics&, Key&)> draw,
             std::function<void(const ModifierKeys&)> click, Key::Role role = Key::Role::button)
    {
        auto& k = add(parent, new Key(title, role));
        k.draw = std::move(draw), k.onClick = std::move(click), k.setBounds(r.toNearestInt());
        return k;
    }
    static bool resetting(const ModifierKeys& m) { return m.isAltDown() || m.isCommandDown(); }
    bool dimOf(const Component& c) const
    {
        for (auto& column : columns)
            if (column.isParentOf(&c))
                return column.dim;
        return false;
    }
    Key& toggleKey(Component& parent, const char* id, const char* label, float x, float y)
    {
        auto& k = key(parent, String(label), { x, y, 16 + textWidth(label), 16 },
                      [this, id, label](Graphics& g, Key& self) {
                          toggle(g, 0, 0, on(id), label, dimOf(self));
                          if (self.isMouseOver() && !on(id))
                              box(g, { 0, 4, 8, 8 }, primary);
                      },
                      [this, id](const ModifierKeys& m) { setParam(id, resetting(m) ? defaultOf(id) : on(id) ? 0.0f : 1.0f); }, Key::Role::toggle);
        k.on = [this, id] { return on(id); };
        k.hint = String(label) + u(" \xc2\xb7 click \xc2\xb7 alt-click resets");
        return k;
    }
    // A segmented cell: the whole cell is the hit target; the chosen one is filled, outlined lighter and bold.
    Key& cell(Component& parent, const String& title, const String& label, Rectangle<float> r, std::function<bool()> chosen,
              std::function<void(const ModifierKeys&)> click, Colour activeText = accent)
    {
        auto& k = key(parent, title, r,
                      [this, label, chosen, activeText](Graphics& g, Key& self) {
                          const bool on = chosen(), d = dimOf(self);
                          const auto b = self.getLocalBounds().toFloat();
                          if (on)
                              g.setColour(tag), g.fillRect(b);
                          box(g, b, dimmed(on ? colours::dim : rule, d));
                          centred(g, label, 0, 3, b.getWidth(), dimmed(on ? activeText : self.isMouseOver() ? primary : muted, d), 11, on);
                      },
                      std::move(click), Key::Role::radio);
        k.on = std::move(chosen);
        return k;
    }
    // A parameter's choices first.. as equal cells from x0 to x1 at y; the arrow keys and the wheel step through them.
    void choiceCells(Component& parent, const char* id, const String& prefix, const StringArray& labels, int first, float x0, float x1, float y,
                     float gap = cellGapX, Colour activeText = accent)
    {
        const float w = (x1 - x0 - gap * (float) (labels.size() - 1)) / (float) labels.size();
        for (int i = 0; i < labels.size(); ++i)
        {
            const int index = first + i;
            auto& k = cell(parent, prefix + " " + labels[i], labels[i], { x0 + (float) i * (w + gap), y, w, cellH },
                           [this, id, index] { return roundToInt(raw(id)) == index; },
                           [this, id, index](const ModifierKeys& m) { setParam(id, resetting(m) ? defaultOf(id) : (float) index); }, activeText);
            k.hint = labels[i] + u(" \xc2\xb7 ") + spec(id).name;
            k.onStep = [this, id](int d) { stepChoice(id, d, false); };
        }
    }
    void stepChoice(const char* id, int d, bool wrap)
    {
        const auto& s = spec(id);
        const int n = roundToInt(s.hi - s.lo) + 1, now = roundToInt(raw(id) - s.lo), next = wrap ? ((now + d) % n + n) % n : jlimit(0, n - 1, now + d);
        if (next != now)
            setParam(id, s.lo + (float) next);
    }
    // The `< name >` box across a column at y (the name is the column's to paint); the arrows click, wheel and key-step round.
    void selector(Column& c, const char* id, const String& what, float y)
    {
        for (int d : { -1, 1 })
            key(c, (d < 0 ? "previous " : "next ") + what, { d < 0 ? 0.0f : CW - 20, y, 20, cellH },
                [this, d](Graphics& g, Key& k) { text(g, d < 0 ? "<" : ">", d < 0 ? 8.0f : 4.0f, 3, dimmed(k.isMouseOver() ? primary : muted, dimOf(k))); },
                [this, id, d](const ModifierKeys&) { stepChoice(id, d, true); })
                .onStep = [this, id](int s) { stepChoice(id, s, true); };
    }
    static void selectorBox(Graphics& g, const String& name, float y, bool d)
    {
        box(g, { 0, y, CW, cellH }, dimmed(rule, d));
        centred(g, name, 0, y + 3, CW, dimmed(primary, d), 12, true);
    }
    // A fader line (Layout.h): name at x 0, optional end labels, the fader between them.
    Control& line(Column& c, Control::Kind kind, const char* id, float y, const char* name, const char* endL, const char* endR, bool centredValue = false,
                  const char* second = nullptr)
    {
        c.caption(name, 0, y);
        if (endL != nullptr)
            c.caption(endL, faderX, y);
        if (endR != nullptr)
            c.caption(endR, faderRightX + 4, y, CW - faderRightX - 4);
        const float x0 = endL != nullptr ? faderEndX : faderX, x1 = endR != nullptr ? faderRightX : CW;
        return control(c, kind, id, { x0, y + 2, x1 - x0, 12 }, centredValue, second);
    }
    void targetLine(Column& c, const char* id)
    {
        line(c, Control::Kind::fader, id, bandTarget, "target", "trans", "sust", true).hint = u("target \xc2\xb7 left: transients only, right: sustain only");
    }
    void focusLine(Column& c, const char* lo, const char* hi)
    {
        line(c, Control::Kind::range, lo, bandFocus, "focus", nullptr, nullptr, false, hi).hint = u("focus \xc2\xb7 the band the module works on; drag either end");
    }
    // Toggles in the third slot of the detail band, 20 px apart, centred on the dials' arc centre.
    void slotToggles(Column& c, std::initializer_list<std::pair<const char*, const char*>> toggles)
    {
        float y = YD + 33 - 10 * (float) (toggles.size() - 1);
        for (auto [id, label] : toggles)
            toggleKey(c, id, label, slotX[2] + 4, y), y += 20;
    }
    static void readout(Graphics& g, bool d, const String& range) { right(g, range, faderX, bandReadout, CW - faderX, dimmed(muted, d), 11); }
    static void caption(Graphics& g, const String& s, bool d) { centred(g, s, 0, bandSelector + 28, CW, dimmed(colours::dim, d), 11); }
    // A meter slot: a dial slot's label and value; the caller draws the meter at (x + 29, y + 18).
    static void meterSlot(Graphics& g, float x, float y, const String& label, const String& value, bool d)
    {
        centred(g, label, x, y, 64, dimmed(muted, d), 11), centred(g, value, x, y + 58, 64, dimmed(primary, d), 12, true);
    }
    String focusText(const char* lo, const char* hi) const
    {
        return raw(lo) <= 20.5f && raw(hi) >= 19999 ? String("full range") : show(lo) + " - " + show(hi);
    }

    // ---- presets ----------------------------------------------------------------------------------------------
    String currentKey() const { return proc.apvts.state.getProperty("preset").toString(); }
    void load(const Preset& preset) // the name joins the load's undo step, so undo / redo keep it in step with the sound
    {
        if (PresetStore::load(proc, preset))
            proc.apvts.state.setProperty("preset", preset.key(), &proc.undoManager);
    }
    void step(int direction)
    {
        const auto& all = store.presets();
        if (all.empty())
            return;
        const auto at = std::find_if(all.begin(), all.end(), [&](auto& p) { return p.key() == currentKey(); });
        const int i = at == all.end() ? (direction > 0 ? -1 : 0) : (int) (at - all.begin());
        load(all[(size_t) ((i + direction + (int) all.size()) % (int) all.size())]);
    }
    void randomPreset()
    {
        auto list = store.list({});
        std::erase_if(list, [&](auto* p) { return p->key() == currentKey(); });
        if (!list.empty())
            load(*list[(size_t) Random::getSystemRandom().nextInt((int) list.size())]);
    }
    void savePreset()
    {
        // The dialog belongs to the editor: closing the editor closes it, and its callback checks the editor still exists.
        saveDialog = std::make_unique<AlertWindow>("save preset", "name", MessageBoxIconType::NoIcon, this);
        const auto* current = store.find(currentKey());
        saveDialog->addTextEditor("name", current != nullptr && !current->factory ? current->name : String());
        saveDialog->addButton("save", 1, KeyPress(KeyPress::returnKey)), saveDialog->addButton("cancel", 0, KeyPress(KeyPress::escapeKey));
        saveDialog->enterModalState(true, ModalCallbackFunction::create([self = SafePointer<Editor>(this), tags = current ? current->tags : StringArray()](int r) {
                                        if (!self || !self->saveDialog)
                                            return;
                                        const auto name = self->saveDialog->getTextEditorContents("name").trim().removeCharacters("/\\:*?\"<>|");
                                        if (r == 1 && name.isNotEmpty())
                                            if (const auto* saved = self->store.save(self->proc, name, tags))
                                                self->proc.apvts.state.setProperty("preset", saved->key(), nullptr), self->say("saved " + name);
                                        MessageManager::callAsync([self] {
                                            if (self)
                                                self->saveDialog.reset();
                                        });
                                    }),
                                    false);
    }
    void say(const String& s) { notice = s, noticeUntil = Time::getMillisecondCounter() + 3000; }

    // ---- top bar --------------------------------------------------------------------------------------------------
    Key *nameKey = nullptr, *prevKey = nullptr, *nextKey = nullptr, *diceKey = nullptr, *saveKey = nullptr, *undoKey = nullptr, *redoKey = nullptr;
    String presetName() const
    {
        const auto* p = store.find(currentKey());
        return p != nullptr ? p->name : String("init");
    }
    void buildTopBar()
    {
        key(layer, "about snap", { 24, 16, 48, 22 }, [](Graphics& g, Key&) { text(g, "snap", 0, 2, primary, 15, true); },
            [this](const ModifierKeys&) { say(u("snap " JucePlugin_VersionString " \xc2\xb7 drum multi-fx \xc2\xb7 daveydidit")); })
            .hint = "about snap";
        prevKey = &key(layer, "previous preset", { 88, 18, 16, 18 }, [](Graphics& g, Key& k) { text(g, "<", 4, 2, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { step(-1); });
        nameKey = &key(layer, "preset browser", { 104, 16, 80, 22 },
                       [this](Graphics& g, Key& k) {
                           if (browser.isVisible())
                               g.setColour(tag), g.fillRect(k.getLocalBounds().toFloat().withTrimmedLeft(-20).withTrimmedRight(-20));
                           auto name = presetName();
                           if (const int fits = (int) ((float) k.getWidth() / textWidth("x")); name.length() > fits)
                               name = name.substring(0, fits - 2) + "..";
                           centred(g, name, 0, 4, (float) k.getWidth(), primary, 12, true);
                       },
                       [this](const ModifierKeys&) { toggleBrowser(); });
        nextKey = &key(layer, "next preset", { 184, 18, 16, 18 }, [](Graphics& g, Key& k) { text(g, ">", 4, 2, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { step(1); });
        diceKey = &key(layer, "random preset", { 216, 20, 14, 14 }, [](Graphics& g, Key& k) { dice(g, 0, 0, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { randomPreset(); });
        saveKey = &key(layer, "save preset", { 242, 18, 40, 18 }, [](Graphics& g, Key& k) { text(g, "save", 4, 2, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { savePreset(); });
        undoKey = &key(layer, "undo", { 287, 18, 40, 18 }, [this](Graphics& g, Key& k) { text(g, "undo", 4, 2, proc.undoManager.canUndo() && k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { proc.undo(); });
        redoKey = &key(layer, "redo", { 332, 18, 40, 18 }, [this](Graphics& g, Key& k) { text(g, "redo", 4, 2, proc.undoManager.canRedo() && k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { proc.redo(); });
        nameKey->hint = "open the preset browser", prevKey->hint = "previous preset", nextKey->hint = "next preset";
        diceKey->hint = "load a random preset", saveKey->hint = "save the current sound as a user preset";
        undoKey->hint = "undo the last change", redoKey->hint = "redo";
        control(layer, Control::Kind::fader, "magnitude", { 945, 22, 300, 12 }).hint = defaultTip;
        key(layer, "menu", { 1296, 18, 22, 20 }, [](Graphics& g, Key& k) { menuIcon(g, 4, 5, k.isMouseOver() ? primary : muted); },
            [this](const ModifierKeys&) { showMenu(); })
            .hint = "low latency mode, oversampling, scale, about";
        layoutTopBar();
    }
    // The name box is as wide as the longest factory preset name (the name centred in it), so < and > never move; a longer
    // user preset name is cut to fit. Save, undo and redo follow 16 px apart.
    float nameWidth() const
    {
        int longest = 4; // "init"
        for (const auto& p : store.presets())
            if (p.factory)
                longest = jmax(longest, p.name.length());
        return textWidth(String::repeatedString("x", longest)) + 8;
    }
    void layoutTopBar()
    {
        const float w = nameWidth();
        nameKey->setBounds(Rectangle<float>(104, 16, w, 22).toNearestInt());
        nextKey->setTopLeftPosition(roundToInt(104 + w), 18);
        diceKey->setTopLeftPosition(jmax(216, roundToInt(104 + w + 28)), 20);
        float x = (float) diceKey->getRight() + 12;
        for (auto* k : { saveKey, undoKey, redoKey })
            k->setTopLeftPosition(roundToInt(x), 18), x += textWidth("save") + 16;
    }
    void toggleBrowser()
    {
        if (browser.isVisible())
            return browser.finish(false); // as ok: the auditioned preset stays
        browser.open(currentKey()), browser.setVisible(true), browser.toFront(true), repaintAll();
    }
    void showMenu()
    {
        PopupMenu menu, scales;
        const double scale = (double) getWidth() / W;
        for (int percent : { 75, 100, 125, 150, 200 })
            scales.addItem(100 + percent, String(percent) + "%", true, std::abs(scale * 100 - percent) < 1);
        menu.addItem(1, "low latency mode (64 samples)", true, on("low_latency"));
        menu.addItem(2, "oversampling", true, on("oversampling"));
        menu.addSubMenu("scale", scales);
        menu.addSeparator();
        menu.addItem(3, "about snap");
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), [self = SafePointer<Editor>(this)](int r) {
            if (!self)
                return;
            if (r == 1)
                self->setParam("low_latency", self->on("low_latency") ? 0.0f : 1.0f);
            else if (r == 2)
                self->setParam("oversampling", self->on("oversampling") ? 0.0f : 1.0f);
            else if (r == 3)
                self->say(u("snap " JucePlugin_VersionString " \xc2\xb7 drum multi-fx \xc2\xb7 daveydidit"));
            else if (r > 100)
                self->setSize(W * (r - 100) / 100, H * (r - 100) / 100);
        });
    }

    // ---- module columns -------------------------------------------------------------------------------------------
    const char* subOrHiss(const char* sub, const char* hiss) const { return tab == 0 ? sub : hiss; }
    void buildColumn(Module m)
    {
        auto& c = columns[(size_t) m];
        c.module = m;
        c.setTitle(String(modules[(size_t) m].name.data()));
        layer.addAndMakeVisible(c);
        c.viz = std::make_unique<Visualizer>(proc, m);
        c.addAndMakeVisible(*c.viz), c.viz->setBounds(0, (int) bandVisual, (int) CW, 104), c.viz->setInterceptsMouseClicks(false, false);
        if (m != Module::subHiss && m != Module::push)
        {
            auto& grip = add(c, new Grip());
            grip.setBounds(0, 4, roundToInt(30 + textWidth(String(modules[(size_t) m].name.data()), 13)), 20);
            grip.drag = [this, m](float dx) { dragColumn(m, dx); };
            grip.drop = [this] { dropColumn(); };
        }
        switch (m)
        {
            case Module::subHiss: buildSubHiss(c); break;
            case Module::shift: buildShift(c); break;
            case Module::space: buildSpace(c); break;
            case Module::press: buildPress(c); break;
            case Module::drive: buildDrive(c); break;
            case Module::push: buildPush(c); break;
        }
    }
    // sub/hiss shows one layer's controls at a time; the other tab's widgets are hidden.
    std::array<std::vector<Component*>, 2> layerParts;
    void buildSubHiss(Column& c)
    {
        const float rowB = bandSelector + cellH + cellGapY, tabW = (CW - cellGapX) / 2;
        for (int t = 0; t < 2; ++t)
            cell(c, t == 0 ? "sub tab" : "hiss tab", t == 0 ? "sub" : "hiss", { (float) t * (tabW + cellGapX), bandSelector, tabW, cellH },
                 [this, t] { return tab == t; }, [this, t](const ModifierKeys&) { setTab(t); })
                .onStep = [this](int d) { setTab(jlimit(0, 1, tab + d)); };
        for (int t = 0; t < 2; ++t)
        {
            const auto first = c.parts.size();
            const bool hiss = t == 1;
            dial(c, hiss ? "hiss_pitch" : "sub_pitch", "pitch", slotX[0], YK, hiss);
            dial(c, hiss ? "hiss_decay" : "sub_decay", "decay", slotX[1], YK);
            dial(c, hiss ? "hiss_duck" : "sub_duck", "duck", slotX[2], YK);
            if (hiss)
            {
                dial(c, "hiss_tone", "tone", slotX[0], YD, true), dial(c, "hiss_env", "env", slotX[1], YD);
                toggleKey(c, "hiss_solo", "solo", slotX[2] + 4, YD + 13), toggleKey(c, "hiss_post", "post", slotX[2] + 4, YD + 33); // res is the third row
                control(c, Control::Kind::fader, "hiss_res", { faderRightX, YD + 55, CW - faderRightX, 12 }).hint = "hiss resonance";
                selector(c, "hiss_type", "hiss type", rowB);
            }
            else
            {
                dial(c, "sub_stereo", "stereo", slotX[0], YD), dial(c, "sub_color", "color", slotX[1], YD);
                slotToggles(c, { { "sub_acid", "acid" }, { "sub_solo", "solo" }, { "sub_post", "post" } });
            }
            line(c, Control::Kind::fader, hiss ? "hiss_sens" : "sub_sens", bandTarget, "sens", nullptr, nullptr).hint
                = u("sensitivity \xc2\xb7 higher triggers on quieter hits");
            line(c, Control::Kind::range, hiss ? "hiss_det_lo" : "sub_det_lo", bandFocus, "detect", nullptr, nullptr, false, hiss ? "hiss_det_hi" : "sub_det_hi").hint
                = u("detect \xc2\xb7 the band the trigger listens to");
            for (int i = first; i < c.parts.size(); ++i)
                layerParts[(size_t) t].push_back(c.parts[i]);
        }
        c.body = [this, &c, rowB](Graphics& g) {
            const bool d = c.dim;
            if (tab == 0)
                caption(g, "generated layers", d);
            else
                selectorBox(g, show("hiss_type"), rowB, d), text(g, "res", slotX[2] + 4, YD + 53, dimmed(muted, d), 11);
            readout(g, d, show(subOrHiss("sub_det_lo", "hiss_det_lo")) + " - " + show(subOrHiss("sub_det_hi", "hiss_det_hi")));
        };
        setTab(0);
    }
    void setTab(int t)
    {
        tab = t;
        for (int i = 0; i < 2; ++i)
            for (auto* part : layerParts[(size_t) i])
                part->setVisible(i == t);
        columns[0].viz->tab = t;
        repaintAll();
    }
    void buildShift(Column& c)
    {
        line(c, Control::Kind::fader, "shift_balance", bandSelector + 4, "freq", nullptr, "form", true).hint = u("balance \xc2\xb7 left: pitch only, right: formant only");
        dial(c, "shift_stereo", "stereo", slotX[0], YK, true), dial(c, "shift_stretch", "stretch", slotX[1], YK, true), dial(c, "shift_fmix", "mix", slotX[2], YK);
        slotToggles(c, { { "shift_invert", "invert" } });
        targetLine(c, "shift_target"), focusLine(c, "shift_focus_lo", "shift_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            caption(g, "balance " + show("shift_balance"), d);
            const float a = raw("shift_amt") * raw("magnitude") * 1.0e-4f, b = raw("shift_balance");
            const float semis = shiftConstants::maxSemitones * a * std::abs(a), sign = on("shift_invert") ? -1.0f : 1.0f;
            const float freq = semis * sign * (1 - jmax(0.0f, b)), form = semis * (1 + jmin(0.0f, b));
            const auto st = [](float v) { return format(spec("hiss_pitch"), std::abs(v) < 0.05f ? 0.0f : v); };
            meterSlot(g, slotX[0], YD, "freq", st(freq), d), vbipolar(g, slotX[0] + 29, YD + 18, freq / shiftConstants::maxSemitones, accent, d);
            meterSlot(g, slotX[1], YD, "form", st(form), d), vbipolar(g, slotX[1] + 29, YD + 18, form / shiftConstants::maxSemitones, accent, d);
            readout(g, d, focusText("shift_focus_lo", "shift_focus_hi"));
        };
    }
    void buildSpace(Column& c)
    {
        selector(c, "space_type", "space type", bandSelector);
        dial(c, "space_decay", "decay", slotX[0], YK), dial(c, "space_stretch", "stretch", slotX[1], YK, true), dial(c, "space_predelay", "predelay", slotX[2], YK);
        dial(c, "space_duck", "duck", slotX[0], YD);
        auto& pad = add(c, new Pad(proc));
        pad.setBounds(Rectangle<float>(slotX[1] + 2, YD + 16, 137, 40).toNearestInt());
        focusLine(c, "space_focus_lo", "space_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            selectorBox(g, show("space_type"), bandSelector, d);
            caption(g, String(roundToInt(raw("space_type")) + 1).paddedLeft('0', 2) + " / 20", d);
            centred(g, "pan/width", slotX[1], YD, CW - slotX[1], dimmed(muted, d), 11);
            const int pan = roundToInt(raw("space_pan"));
            centred(g, (pan == 0 ? String("c") : (pan < 0 ? "l" : "r") + String(std::abs(pan))) + u(" \xc2\xb7 ") + show("space_width"), slotX[1], YD + 58,
                    CW - slotX[1], dimmed(primary, d), 12, true);
            readout(g, d, focusText("space_focus_lo", "space_focus_hi"));
        };
    }
    void modeGrid(Column& c, const char* id, const StringArray& labels) // 3 x 2 equal cells in the selector band
    {
        choiceCells(c, id, id, { labels[0], labels[1], labels[2] }, 0, 0, CW, bandSelector);
        choiceCells(c, id, id, { labels[3], labels[4], labels[5] }, 3, 0, CW, bandSelector + cellH + cellGapY);
    }
    void buildPress(Column& c)
    {
        modeGrid(c, "press_mode", { "punch", "glue", "slam", "warm", "smooth", "multi" });
        dial(c, "press_mix", "mix", slotX[0], YK);
        line(c, Control::Kind::fader, "press_downup", YD + 18, "down", nullptr, "up", true).hint = u("down/up \xc2\xb7 downward to upward compression");
        line(c, Control::Kind::fader, "press_bands", YD + 46, "bands", "i", "iii").hint = u("bands \xc2\xb7 single band to multiband");
        line(c, Control::Kind::fader, "press_attack", bandTarget, "attack", "slow", "fast", true).hint
            = u("attack \xc2\xb7 slow lets each hit through before press clamps, fast holds it down");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            const float ms = Press::attackMs(roundToInt(raw("press_mode")), raw("press_attack"));
            readout(g, d, "attack " + (ms < 10 ? String(ms, 1) : String(roundToInt(ms))) + " ms");
            const float down = proc.display.pressDownDb.load(), up = proc.display.pressUpDb.load();
            meterSlot(g, slotX[1], YK, "down", format(spec("in_gain"), down), d), vmeter(g, slotX[1] + 29, YK + 18, 10, -down / 12, true, accent, d);
            meterSlot(g, slotX[2], YK, "up", format(spec("in_gain"), up), d), vmeter(g, slotX[2] + 29, YK + 18, 10, up / 6, false, primary, d);
        };
    }
    void buildDrive(Column& c)
    {
        modeGrid(c, "drive_mode", { "desk", "cassette", "tape", "tube", "bits", "ring" });
        dial(c, "drive_flavor", "flavor", slot2X[0], YK, true), dial(c, "drive_mix", "mix", slot2X[1], YK);
        line(c, Control::Kind::fader, "drive_flavor", YD + 32, "dark", nullptr, "open", true).hint = u("flavor \xc2\xb7 dark to open");
        targetLine(c, "drive_target"), focusLine(c, "drive_focus_lo", "drive_focus_hi");
        c.body = [this, &c](Graphics& g) { readout(g, c.dim, focusText("drive_focus_lo", "drive_focus_hi")); };
    }
    void buildPush(Column& c)
    {
        c.caption("stages", 0, bandSelector + 15);
        choiceCells(c, "push_mult", "push_mult", { "1x", "2x", "4x" }, 0, faderX, CW, bandSelector + 12);
        dial(c, "push_tone", "tone", slot2X[0], YK, true), dial(c, "push_mix", "mix", slot2X[1], YK);
        c.caption("limit", 0, YD + 32);
        targetLine(c, "push_target"), focusLine(c, "push_focus_lo", "push_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            const float limit = proc.display.pushLimitDb.load();
            fader(g, faderX, YD + 34, 104, FaderKind::meter, -limit / 12, 0, accent, d);
            led(g, { 162, YD + 35, 6, 6 }, dimmed(limit < -0.1f ? accent : colours::dim, d));
            right(g, format(spec("in_gain"), limit), 168, YD + 32, CW - 168, dimmed(primary, d), 11, true);
            readout(g, d, focusText("push_focus_lo", "push_focus_hi"));
        };
    }

    // ---- reorder --------------------------------------------------------------------------------------------------
    std::array<int, 6> slots() const // module -> slot
    {
        std::array<int, 6> s { 0, 1, 2, 3, 4, 5 };
        const auto o = proc.order();
        for (int i = 0; i < 4; ++i)
            s[(size_t) o[(size_t) i]] = i + 1;
        return s;
    }
    std::array<int, 6> shown = slots();
    void place()
    {
        for (int m = 0; m < 6; ++m)
        {
            columns[(size_t) m].setBounds(Rectangle<float>(colX(shown[(size_t) m]), columnTop, CW, columnH).toNearestInt());
            cells[(size_t) m].setBounds(Rectangle<float>(colX(shown[(size_t) m]), amountTop, CW, 126).toNearestInt());
        }
    }
    void dragColumn(Module m, float dx)
    {
        dragging = (int) m;
        const auto s = slots();
        const float x = jlimit(colX(1), colX(4), colX(s[(size_t) m]) + dx);
        const int target = jlimit(1, 4, roundToInt((x - 24) / 235));
        std::vector<int> others; // the other middle modules in their current order
        for (auto o : proc.order())
            if (o != m)
                others.push_back((int) o);
        others.insert(others.begin() + (target - 1), (int) m);
        for (int i = 0; i < 4; ++i)
            shown[(size_t) others[(size_t) i]] = i + 1;
        place();
        columns[(size_t) m].setTopLeftPosition(roundToInt(x), (int) columnTop), columns[(size_t) m].toFront(false);
        if (browser.isVisible())
            browser.toFront(false);
        layer.repaint();
    }
    void dropColumn()
    {
        if (dragging < 0)
            return;
        dragging = -1;
        ChainOrder o {};
        for (int m = 1; m < 5; ++m)
            o[(size_t) shown[(size_t) m] - 1] = (Module) m;
        if (o != proc.order())
            proc.setOrder(o);
        shown = slots(), place(), repaintAll();
    }

    // ---- amount row -----------------------------------------------------------------------------------------------
    void buildAmount(Module m)
    {
        auto& cell = cells[(size_t) m];
        const auto& info = modules[(size_t) m];
        cell.setTitle(String(info.name.data()) + " amount");
        cell.draw = [this, m](Graphics& g) {
            const bool d = columns[(size_t) m].dim;
            centred(g, amountValue(m), 0, 88, CW, dimmed(primary, d), 15, true);
            centred(g, amountLabel(m), 0, 108, CW, d ? colours::dim : muted, 11);
        };
        layer.addAndMakeVisible(cell);
        const char* onId = info.onId.data();
        auto& power = key(cell, String(info.name.data()) + " on", { 0, 4, 28, 16 },
                          [this, m, onId](Graphics& g, Key&) {
                              square(g, 0, 4, on(onId)), moduleGlyph(g, m, 14, -1, on(onId) ? accent : colours::dim);
                          },
                          [this, onId](const ModifierKeys& mods) { setParam(onId, resetting(mods) ? 1.0f : on(onId) ? 0.0f : 1.0f); },
                          Key::Role::toggle);
        power.on = [this, onId] { return on(onId); };
        power.hint = String(info.name.data()) + u(" on/off \xc2\xb7 off passes the module's input through");
        const bool two = m == Module::subHiss;
        auto& amount = control(cell, Control::Kind::amount, info.amountIds[0].data(), { 64, 6, 88, arcHeight(88) }, m == Module::shift,
                               two ? info.amountIds[1].data() : nullptr);
        amount.hint = two ? u("sub/hiss amounts \xc2\xb7 inner ring sub, outer ring hiss; click a ring to pick it")
                          : String(info.name.data()) + u(" amount \xc2\xb7 the big one");
        if (two)
            amount.onPick = [this](int ring) { setTab(ring); };
        auto& lock = key(cell, String(info.name.data()) + " lock", { 149, 2, 68, 20 },
                         [this, m, &cell](Graphics& g, Key& k) {
                             if (proc.locked((int) m))
                                 g.setColour(tag), g.fillRect(0, 0, 68, 20), padlock(g, 7, 4, accent), text(g, "locked", 23, 2, accent, 11);
                             else if (cell.isMouseOverOrDragging(true) || k.hasKeyboardFocus(false))
                                 padlock(g, 37, 4, k.isMouseOver() ? secondary : muted);
                         },
                         [this, m](const ModifierKeys&) { proc.setLocked((int) m, !proc.locked((int) m)), repaintAll(); }, Key::Role::toggle);
        lock.on = [this, m] { return proc.locked((int) m); };
        lock.hint = u("lock \xc2\xb7 a locked module keeps its settings and slot when presets change");
    }
    String amountValue(Module m) const
    {
        if (m == Module::subHiss)
            return (tab == 0 ? "sub " : "hiss ") + show(tab == 0 ? "sub_amt" : "hiss_amt");
        if (m == Module::shift)
        {
            const float a = raw("shift_amt") * 0.01f, st = shiftConstants::maxSemitones * a * std::abs(a);
            return format(spec("hiss_pitch"), std::abs(st) < 0.05f ? 0.0f : st).replace(".0 st", " st");
        }
        return show(modules[(size_t) m].amountIds[0].data());
    }
    String amountLabel(Module m) const
    {
        const float shift = raw("shift_amt");
        const String names[] { (tab == 0 ? "hiss " : "sub ") + show(tab == 0 ? "hiss_amt" : "sub_amt"), shift > 0 ? "up" : shift < 0 ? "down" : "pitch",
                               "wet", "amount", "drive", "push" };
        return names[(size_t) m] + (on(modules[(size_t) m].onId.data()) ? String() : u(" \xc2\xb7 off"));
    }

    // ---- master ---------------------------------------------------------------------------------------------------
    // Seven groups on one centre line, 28 px apart with a rule in each gutter: input | eq / cut | tone / shape | air |
    // attack, sustain | clip | output, match and meters.
    static constexpr float groupX[] { 24, 132, 460, 668, 760, 928, 1016 };
    Control& bigDial(const char* id, const char* label, float x)
    {
        auto& c = dial(layer, id, label, x, Yc - 54, true);
        c.knob = bigKnob, c.setBounds(Rectangle<float>(x, Yc - 54, 80, 100).toNearestInt());
        return c;
    }
    void buildMaster()
    {
        bigDial("in_gain", "in", groupX[0]);
        toggleKey(layer, "eq_on", "eq", groupX[1], Yc - 6);
        auto shapeKey = [&](const char* id, int value, Slope s, float x) {
            key(layer, String(id) + (value ? " steep" : " gentle"), { x, Yc - 8, 14, 16 },
                [this, id, value, s](Graphics& g, Key& k) { slope(g, s, 0, 3, roundToInt(raw(id)) == value || k.isMouseOver() ? primary : colours::dim); },
                [this, id, value](const ModifierKeys&) { setParam(id, (float) value); }, Key::Role::radio)
                .on = [this, id, value] { return roundToInt(raw(id)) == value; };
        };
        shapeKey("lc_shape", 0, Slope::lowGentle, 176), shapeKey("lc_shape", 1, Slope::lowSteep, 194);
        shapeKey("hc_shape", 0, Slope::highGentle, 400), shapeKey("hc_shape", 1, Slope::highSteep, 418);
        control(layer, Control::Kind::range, "cut_lo", { 216, Yc - 6, 176, 12 }, false, "cut_hi").hint = u("cut \xc2\xb7 low and high cut; drag either end");
        dial(layer, "tone", "tone", groupX[2], Yc - 40, true);
        choiceCells(layer, "tone_shape", "tone shape", { "tilt", "bump" }, 0, groupX[2] + 80, groupX[2] + 180, Yc - 22, 4, primary);
        choiceCells(layer, "tone_shape", "tone shape", { "mid", "focus" }, 2, groupX[2] + 80, groupX[2] + 180, Yc + 2, 4, primary);
        dial(layer, "air", "air", groupX[3], Yc - 40);
        dial(layer, "attack", "attack", groupX[4], Yc - 40, true), dial(layer, "sustain", "sustain", groupX[4] + 76, Yc - 40, true);
        toggleKey(layer, "clip_on", "clip", groupX[5] + 4, Yc - 6);
        bigDial("out_gain", "out", groupX[6]);
        key(layer, "loudness match", { groupX[6] + 100, Yc - 10, 72, 20 },
            [](Graphics& g, Key& k) {
                g.setColour(tag), g.fillRect(k.getLocalBounds());
                swapArrows(g, 8, 2, accent), text(g, "match", 24, 2, k.isMouseOver() ? primary : muted);
            },
            [this](const ModifierKeys&) { proc.beginUndoStep(), proc.matchLoudness(); })
            .hint = u("match \xc2\xb7 sets out so the output is as loud as the input (bs.1770)");
        key(layer, "master lock", { 1402, masterTop, 18, 16 }, [this](Graphics& g, Key& k) { padlock(g, 4, 2, proc.locked(masterLock) ? accent : k.isMouseOver() ? secondary : muted); },
            [this](const ModifierKeys&) { proc.setLocked(masterLock, !proc.locked(masterLock)), repaintAll(); }, Key::Role::toggle)
            .on = [this] { return proc.locked(masterLock); };
    }
    static String lufs(float v) { return v <= LoudnessMeter::silence ? u("\xe2\x88\x92inf") : String(v, 1).replaceCharacter('-', (juce_wchar) 0x2212); }
    void paintMaster(Graphics& g)
    {
        text(g, "master", 24, masterTop, muted), hline(g, 84, masterTop + 8, 1310);
        for (int i = 1; i < 7; ++i)
            vline(g, groupX[i] - masterGutter / 2, Yc - 44, 88);
        centred(g, "cut", 216, Yc - 40, 176, muted, 11);
        text(g, show("cut_lo"), 216, Yc + 12, muted, 11), right(g, show("cut_hi"), 292, Yc + 12, 100, muted, 11);
        centred(g, "shape", groupX[2] + 80, Yc - 40, 100, muted, 11);
        led(g, { groupX[5] + 50, Yc - 3.5f, 7, 7 }, proc.clipping() && on("clip_on") ? accent : colours::dim);
        paintMeters(g);
    }
    static constexpr float metersX = 1208; // the meters run flush to the right margin
    Rectangle<int> metersArea() const { return { (int) metersX - 4, (int) Yc - 44, 1420 - (int) metersX, 84 }; }
    void paintMeters(Graphics& g)
    {
        const float x = metersX, valueX = 1416 - 44;
        text(g, "in/out", x, Yc - 40, muted, 11);
        text(g, "in", x, Yc - 20, muted, 11), fader(g, x + 28, Yc - 18, 128, FaderKind::meter, meterIn, 0, secondary);
        right(g, lufs(lufsIn), valueX, Yc - 20, 44, primary, 11);
        text(g, "out", x, Yc - 2, muted, 11), fader(g, x + 28, Yc, 128, FaderKind::meter, meterOut, 0, accent);
        right(g, lufs(lufsOut), valueX, Yc - 2, 44, primary, 11);
        text(g, "diff", x, Yc + 18, muted, 11);
        const bool both = lufsIn > LoudnessMeter::silence && lufsOut > LoudnessMeter::silence;
        right(g, both ? format(spec("in_gain"), lufsOut - lufsIn) : String("-"), 1316, Yc + 17, 100, primary, 12, true);
    }

    // ---- bottom bar -----------------------------------------------------------------------------------------------
    void buildBottomBar()
    {
        const float y = bottomTop + 26;
        for (int i = 0; i < 5; ++i)
        {
            const String label = i < 4 ? String::charToString((juce_wchar) ('A' + i)) : String("+");
            key(layer, i < 4 ? "snapshot " + label : String("store snapshot"), { 1150.0f + 24 * (float) i, y - 2, 18, 18 },
                [this, i, label](Graphics& g, Key& k) {
                    const bool current = i == snapshot;
                    if (current)
                        riso::use(g, primary), g.fillRect(k.getLocalBounds());
                    const auto c = current ? bg : k.isMouseOver() ? primary : i < 4 && !proc.hasSnapshot(i) ? colours::dim : muted;
                    centred(g, label, 0, 2, 18, c, 12, current);
                },
                [this, i](const ModifierKeys& m) {
                    if (i == 4 || m.isPopupMenu() || !proc.hasSnapshot(i))
                        proc.storeSnapshot(i == 4 ? snapshot : i), say("stored snapshot " + String::charToString((juce_wchar) ('A' + (i == 4 ? snapshot : i))));
                    else
                        proc.recallSnapshot(i);
                    if (i < 4)
                        snapshot = i;
                    repaintAll();
                },
                i < 4 ? Key::Role::radio : Key::Role::button)
                .hint = i < 4 ? u("snapshot \xc2\xb7 click recalls (stores when empty), right-click stores") : String("store the current sound in the selected snapshot");
        }
        toggleKey(layer, "bypass", "bypass", 1336, y);
    }
    void paintBottomBar(Graphics& g)
    {
        hline(g, 24, bottomTop + 8, 1392);
        text(g, "?", 24, bottomTop + 26, accent, 12, true);
        const auto line = Time::getMillisecondCounter() < noticeUntil ? notice : tip;
        text(g, line.substring(0, 150), 42, bottomTop + 26, muted, 12, false, 1100);
    }
    Rectangle<int> tipArea() const { return { 20, (int) bottomTop + 20, 1126, 28 }; }

    // ---- paint, timer -------------------------------------------------------------------------------------------
    void paint(Graphics& g) override { g.fillAll(bg); }
    void paintLayer(Graphics& g)
    {
        g.setImageResamplingQuality(Graphics::lowResamplingQuality), g.drawImageAt(riso::backdrop(), 0, 0);
        text(g, "magnitude", 869, 20, muted), text(g, show("magnitude"), 1255, 20, primary, 12, true);
        text(g, "daveydidit", 1344, 20, colours::dim), hline(g, 24, 50, 1392);
        for (int i = 1; i < 6; ++i)
            arrow(g, colX(i) - 14, 70), vline(g, colX(i) - 9, 100, 388);
        hline(g, 24, 496, 1392);
        paintMaster(g), paintBottomBar(g);
    }
    void resized() override
    {
        const float s = (float) getWidth() / W;
        layer.setTransform(AffineTransform::scale(s, (float) getHeight() / H));
        proc.apvts.state.setProperty("ui_scale", s, nullptr);
    }
    void repaintAll() { layer.repaint(); }
    void timerCallback() override
    {
        auto& d = proc.display;
        // No audio flowing (the host stopped, or silence): every visualizer settles and stops.
        const unsigned blocks = d.blocks.load(std::memory_order_relaxed);
        const bool flowing = blocks != lastBlocks;
        lastBlocks = blocks;
        Levels levels { d.inPeak.exchange(0), d.outPeak.exchange(0), d.spaceWet.exchange(0), d.driveIn.exchange(0), d.driveAdded.exchange(0),
                        d.pushIn.exchange(0), d.pushAdded.exchange(0), flowing };
        if (!flowing)
            levels = { .flowing = false };
        for (auto& c : columns)
            c.viz->update(levels);

        // Meters: BS.1770 short-term loudness of the input and output.
        const float outDb = applyMagnitude(spec("out_gain"), raw("out_gain"), raw("magnitude") * 0.01f);
        const float in = proc.inputLufs(), pre = proc.preOutputLufs(), out = pre <= LoudnessMeter::silence ? pre : pre + outDb;
        const auto norm = [](float v) { return jlimit(0.0f, 1.0f, (v + 60) / 60); };
        if (std::abs(in - lufsIn) >= 0.05f || std::abs(out - lufsOut) >= 0.05f)
            lufsIn = in, lufsOut = out, meterIn = norm(in), meterOut = norm(out), layer.repaint(metersArea());

        // Help line: the hovered control's tip, or a notice.
        auto* under = Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
        if (under != nullptr && !layer.isParentOf(under))
            under = nullptr;
        String line = defaultTip;
        for (auto* c = under; c != nullptr && c != &layer; c = c->getParentComponent())
        {
            if (auto* control = dynamic_cast<Control*>(c))
            {
                auto& q = control->param(control->active);
                line = control->hint.isNotEmpty() ? control->hint : control->getTitle();
                line << u(" \xc2\xb7 ") << format(spec(q.paramID.toRawUTF8()), q.convertFrom0to1(q.getValue()))
                     << u(" \xc2\xb7 drag, shift fine, alt-click reset, double-click types");
                break;
            }
            if (auto* k = dynamic_cast<Key*>(c))
            {
                line = k->hint.isNotEmpty() ? k->hint : k->getTitle();
                break;
            }
        }
        if (line != tip || (noticeUntil != 0 && Time::getMillisecondCounter() >= noticeUntil))
        {
            if (Time::getMillisecondCounter() >= noticeUntil)
                noticeUntil = 0;
            tip = line, setHelpText(line), layer.repaint(tipArea());
        }
        if (noticeUntil != 0)
            layer.repaint(tipArea());

        // Everything else repaints when what it shows changes.
        String state;
        state << proc.apvts.state.getProperty("order").toString() << currentKey() << (int) proc.clipping() << (int) browser.isVisible()
              << (proc.undoManager.canUndo() ? "u" : "") << (proc.undoManager.canRedo() ? "r" : "");
        for (int i = 0; i < lockCount; ++i)
            state << (int) proc.locked(i);
        for (int i = 0; i < 4; ++i)
            state << (int) proc.hasSnapshot(i);
        state << roundToInt(d.pressDownDb.load() * 10) << roundToInt(d.pressUpDb.load() * 10) << roundToInt(d.pushLimitDb.load() * 10);
        if (hovered.getComponent() != under)
        {
            hovered = under;
            for (auto& cell : cells)
                cell.repaint();
        }
        bool changed = state != drawnState;
        const auto& params = proc.getParameters();
        drawnParams.resize((size_t) params.size(), -1);
        for (int i = 0; i < params.size(); ++i)
            if (const float v = params[i]->getValue(); v != drawnParams[(size_t) i])
                drawnParams[(size_t) i] = v, changed = true;
        if (!changed)
            return;
        drawnState = state;
        if (dragging < 0 && shown != slots())
            shown = slots(), place();
        for (int m = 0; m < 6; ++m)
        {
            auto& c = columns[(size_t) m];
            c.dim = !on(modules[(size_t) m].onId.data()) || on("bypass");
            c.viz->dim = c.dim;
            for (auto* part : c.parts)
                if (auto* control = dynamic_cast<Control*>(part))
                    control->dim = c.dim;
                else if (auto* pad = dynamic_cast<Pad*>(part))
                    pad->dim = c.dim;
            for (auto* part : cells[(size_t) m].getChildren())
                if (auto* control = dynamic_cast<Control*>(part))
                    control->dim = c.dim;
        }
        layoutTopBar();
        repaintAll();
    }
};

} // namespace
} // namespace snap::ui

AudioProcessorEditor* SnapProcessor::createEditor() { return new snap::ui::Editor(*this); }
