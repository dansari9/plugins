// snap's one-screen editor (approved v1 layout, 1440 x 840 logical, scaled 75-200 %). Coordinates are the mockup's:
// top bar, six module columns in chain order, the amount row, the master strip and the bottom bar; the preset browser
// overlays the columns only. Controls draw themselves; each zone paints its own labels and readouts. A 30 Hz timer feeds
// the visualizers and meters and repaints the static layer only when something it shows has changed.
#include "../PluginProcessor.h"
#include "../dsp/modules/ShiftConstants.h"
#include "PresetBrowser.h"
#include "Visualizers.h"
#include "Widgets.h"

using namespace juce;

namespace snap::ui
{
namespace
{
using namespace colours;
constexpr int W = 1440, H = 840;
constexpr float CW = 217, R1 = 194, R2 = 280, columnTop = 64, amountTop = 502, masterTop = 636, bottomTop = 776;
float colX(int slot) { return 24.0f + (float) slot * 235; }
const String defaultTip = u("magnitude scales all six modules and master at once \xc2\xb7 alt-click any dial to reset, double-click to type");

// A component that paints through a lambda: the fixed zones and the amount cells.
struct Canvas : Component
{
    std::function<void(Graphics&)> draw;
    void paint(Graphics& g) override
    {
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
    std::function<void(Graphics&)> body; // labels and readouts
    bool dim = false;
    float headerX() const { return module == Module::subHiss || module == Module::push ? 0.0f : 14.0f; }
    void paint(Graphics& g) override
    {
        const float hx = headerX();
        const auto name = String(modules[(size_t) module].name.data());
        if (hx > 0)
            handle(g, 0, 10, dimmed(muted, dim));
        moduleGlyph(g, module, hx, 7, dimmed(accent, dim));
        text(g, name, hx + 16, 7, dimmed(primary, dim), 13, true);
        const float rx = std::ceil(hx + 16 + textWidth(name, 13) + 10);
        hline(g, rx, 16, CW - rx);
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
        for (float x = 3; x < 141; x += 6)
            for (float y = 3; y < 40; y += 6)
                dot(g, x, y, 0.7f, dimmed(colours::dim, dim));
        vline(g, 69, 0, 40), hline(g, 0, 20, 137);
        dot(g, 3 + 132 * pan.getValue(), 37 - 32 * width.getValue(), 3, dimmed(primary, dim));
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
                              square(g, 0, 4, false, accent, false), g.setColour(primary), g.drawRect(0.0f, 4.0f, 8.0f, 8.0f, 1.0f);
                      },
                      [this, id](const ModifierKeys& m) { setParam(id, resetting(m) ? defaultOf(id) : on(id) ? 0.0f : 1.0f); }, Key::Role::toggle);
        k.on = [this, id] { return on(id); };
        k.hint = String(label) + u(" \xc2\xb7 click \xc2\xb7 alt-click resets");
        return k;
    }
    // A row of choice words; the chosen one accent and bold.
    void words(Component& parent, const char* id, const StringArray& labels, int first, float x, float y)
    {
        for (int i = 0; i < labels.size(); ++i)
        {
            const int index = first + i;
            const auto label = labels[i];
            auto& k = key(parent, String(id) + " " + label, { x, y, textWidth(label), 16 },
                          [this, id, index, label](Graphics& g, Key& self) {
                              const bool chosen = roundToInt(raw(id)) == index;
                              text(g, label, 0, 0, dimmed(chosen ? accent : self.isMouseOver() ? primary : muted, dimOf(self)), 12, chosen);
                          },
                          [this, id, index](const ModifierKeys& m) { setParam(id, resetting(m) ? defaultOf(id) : (float) index); }, Key::Role::radio);
            k.on = [this, id, index] { return roundToInt(raw(id)) == index; };
            k.hint = label + u(" \xc2\xb7 ") + spec(id).name;
            x += textWidth(label) + 12;
        }
    }
    void targetRow(Column& c, const char* id)
    {
        control(c, Control::Kind::fader, id, { 98, 368, 78, 12 }, true).hint = u("target \xc2\xb7 left: transients only, right: sustain only");
    }
    void focusRow(Column& c, const char* lo, const char* hi)
    {
        control(c, Control::Kind::range, lo, { 56, 390, 161, 12 }, false, hi).hint = u("focus \xc2\xb7 the band the module works on; drag either end");
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
    Key *nameKey = nullptr, *prevKey = nullptr, *nextKey = nullptr, *diceKey = nullptr, *saveKey = nullptr, *magLabel = nullptr;
    Control* magnitudeFader = nullptr;
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
                           text(g, presetName(), 4, 4, primary, 12, true);
                       },
                       [this](const ModifierKeys&) { toggleBrowser(); });
        nextKey = &key(layer, "next preset", { 184, 18, 16, 18 }, [](Graphics& g, Key& k) { text(g, ">", 4, 2, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { step(1); });
        diceKey = &key(layer, "random preset", { 216, 20, 14, 14 }, [](Graphics& g, Key& k) { dice(g, 0, 0, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { randomPreset(); });
        saveKey = &key(layer, "save preset", { 242, 18, 40, 18 }, [](Graphics& g, Key& k) { text(g, "save", 4, 2, k.isMouseOver() ? primary : muted); },
                       [this](const ModifierKeys&) { savePreset(); });
        nameKey->hint = "open the preset browser", prevKey->hint = "previous preset", nextKey->hint = "next preset";
        diceKey->hint = "load a random preset", saveKey->hint = "save the current sound as a user preset";
        magnitudeFader = &control(layer, Control::Kind::fader, "magnitude", { 406, 22, 300, 12 });
        magnitudeFader->hint = defaultTip;
        key(layer, "menu", { 1296, 18, 22, 20 }, [](Graphics& g, Key& k) { menuIcon(g, 4, 5, k.isMouseOver() ? primary : muted); },
            [this](const ModifierKeys&) { showMenu(); })
            .hint = "low latency mode, oversampling, scale, about";
        layoutTopBar();
    }
    void layoutTopBar() // the name sets where the rest of the left group sits
    {
        const float w = textWidth(presetName()) + 8;
        nameKey->setBounds(Rectangle<float>(104, 16, w, 22).toNearestInt());
        nextKey->setTopLeftPosition(roundToInt(104 + w), 18);
        diceKey->setTopLeftPosition(jmax(216, roundToInt(104 + w + 28)), 20);
        saveKey->setTopLeftPosition(diceKey->getRight() + 12, 18);
        const int shift = jmax(0, saveKey->getRight() + 48 - 330);
        magnitudeFader->setTopLeftPosition(406 + shift, 22);
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
        c.addAndMakeVisible(*c.viz), c.viz->setBounds(0, 34, 217, 104), c.viz->setInterceptsMouseClicks(false, false);
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
        for (int t = 0; t < 2; ++t)
            key(c, t == 0 ? "sub tab" : "hiss tab", { t == 0 ? 0.0f : 40.0f, 150, t == 0 ? 24.0f : 32.0f, 20 },
                [this, t](Graphics& g, Key& self) {
                    text(g, t == 0 ? "sub" : "hiss", 0, 2, dimmed(tab == t ? primary : self.isMouseOver() ? secondary : muted, dimOf(self)), 12, tab == t);
                    if (tab == t)
                        hline(g, 0, 19, textWidth(t == 0 ? "sub" : "hiss"), dimmed(accent, dimOf(self)));
                },
                [this, t](const ModifierKeys&) { setTab(t); }, Key::Role::radio)
                .on = [this, t] { return tab == t; };
        for (int t = 0; t < 2; ++t)
        {
            const auto first = c.parts.size();
            const bool hiss = t == 1;
            dial(c, hiss ? "hiss_pitch" : "sub_pitch", "pitch", 0, R1, hiss);
            dial(c, hiss ? "hiss_decay" : "sub_decay", "decay", 76, R1);
            dial(c, hiss ? "hiss_duck" : "sub_duck", "duck", 153, R1);
            if (hiss)
            {
                dial(c, "hiss_tone", "tone", 0, R2, true), dial(c, "hiss_env", "env", 76, R2);
                toggleKey(c, "hiss_solo", "solo", 160, R2 + 6), toggleKey(c, "hiss_post", "post", 160, R2 + 28);
                control(c, Control::Kind::fader, "hiss_res", { 186, R2 + 54, 30, 12 }).hint = "hiss resonance";
                for (int d : { -1, 1 })
                    key(c, d < 0 ? "previous hiss type" : "next hiss type", { d < 0 ? 105.0f : 205.0f, 150, 12, 20 },
                        [d](Graphics& g, Key& k) { text(g, d < 0 ? "<" : ">", 0, 2, k.isMouseOver() ? primary : muted); },
                        [this, d](const ModifierKeys&) { setParam("hiss_type", (float) ((roundToInt(raw("hiss_type")) + d + 18) % 18)); });
            }
            else
            {
                dial(c, "sub_stereo", "stereo", 0, R2), dial(c, "sub_color", "color", 76, R2);
                toggleKey(c, "sub_acid", "acid", 160, R2 + 6), toggleKey(c, "sub_solo", "solo", 160, R2 + 28);
                toggleKey(c, "sub_post", "post", 160, R2 + 50);
            }
            control(c, Control::Kind::fader, hiss ? "hiss_sens" : "sub_sens", { 56, 368, 161, 12 }).hint
                = u("sensitivity \xc2\xb7 higher triggers on quieter hits");
            control(c, Control::Kind::range, hiss ? "hiss_det_lo" : "sub_det_lo", { 56, 390, 161, 12 }, false, hiss ? "hiss_det_hi" : "sub_det_hi").hint
                = u("detect \xc2\xb7 the band the trigger listens to");
            for (int i = first; i < c.parts.size(); ++i)
                layerParts[(size_t) t].push_back(c.parts[i]);
        }
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            if (tab == 0)
                right(g, "generated layers", 89, 152, 128, dimmed(colours::dim, d), 11);
            else
                text(g, show("hiss_type"), 117, 152, dimmed(primary, d), 12, true), text(g, "res", 160, R2 + 52, dimmed(muted, d), 11);
            text(g, "sens", 0, 366, dimmed(muted, d), 11), text(g, "detect", 0, 388, dimmed(muted, d), 11);
            const char* lo = subOrHiss("sub_det_lo", "hiss_det_lo");
            right(g, show(lo) + " - " + show(subOrHiss("sub_det_hi", "hiss_det_hi")), 56, 404, 161, dimmed(muted, d), 11);
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
        control(c, Control::Kind::fader, "shift_balance", { 40, 154, 136, 12 }, true).hint = u("balance \xc2\xb7 left: pitch only, right: formant only");
        dial(c, "shift_stereo", "stereo", 0, R1, true), dial(c, "shift_stretch", "stretch", 76, R1, true), dial(c, "shift_fmix", "mix", 153, R1);
        toggleKey(c, "shift_invert", "invert", 0, R2 + 6);
        targetRow(c, "shift_target"), focusRow(c, "shift_focus_lo", "shift_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            text(g, "freq", 0, 152, dimmed(muted, d), 11), text(g, "form", 187, 152, dimmed(muted, d), 11);
            const float a = raw("shift_amt") * raw("magnitude") * 1.0e-4f, b = raw("shift_balance");
            const float semis = shiftConstants::maxSemitones * a * std::abs(a), sign = on("shift_invert") ? -1.0f : 1.0f;
            const auto st = [](float v) { return format(spec("hiss_pitch"), std::abs(v) < 0.05f ? 0.0f : v); };
            text(g, "freq", 0, R2 + 32, dimmed(muted, d), 11), text(g, st(semis * sign * (1 - jmax(0.0f, b))), 56, R2 + 31, dimmed(primary, d), 12, true);
            text(g, "form", 0, R2 + 52, dimmed(muted, d), 11), text(g, st(semis * (1 + jmin(0.0f, b))), 56, R2 + 51, dimmed(primary, d), 12, true);
            targetLabels(g, d), focusLabels(g, d, focusText("shift_focus_lo", "shift_focus_hi"));
        };
    }
    void targetLabels(Graphics& g, bool d)
    {
        text(g, "target", 0, 366, dimmed(muted, d), 11), text(g, "trans", 56, 366, dimmed(muted, d), 11), text(g, "sust", 187, 366, dimmed(muted, d), 11);
    }
    void focusLabels(Graphics& g, bool d, const String& range)
    {
        text(g, "focus", 0, 388, dimmed(muted, d), 11), right(g, range, 56, 404, 161, dimmed(muted, d), 11);
    }
    void buildSpace(Column& c)
    {
        for (int d : { -1, 1 })
            key(c, d < 0 ? "previous space type" : "next space type", { d < 0 ? 0.0f : 0.0f, 150, 12, 20 },
                [d](Graphics& g, Key& k) { text(g, d < 0 ? "<" : ">", 0, 2, k.isMouseOver() ? primary : muted); },
                [this, d](const ModifierKeys&) { setParam("space_type", (float) ((roundToInt(raw("space_type")) + d + 20) % 20)); });
        dial(c, "space_decay", "decay", 0, R1), dial(c, "space_stretch", "stretch", 76, R1, true), dial(c, "space_predelay", "predelay", 153, R1);
        dial(c, "space_duck", "duck", 0, R2);
        auto& pad = add(c, new Pad(proc));
        pad.setBounds(80, (int) R2 + 17, 137, 40);
        focusRow(c, "space_focus_lo", "space_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            const auto name = show("space_type");
            text(g, name, 16, 152, dimmed(primary, d), 12, true);
            right(g, String(roundToInt(raw("space_type")) + 1).paddedLeft('0', 2) + "/20", 157, 152, 60, dimmed(muted, d), 11);
            text(g, "pan/width", 80, R2, dimmed(muted, d), 11);
            const int pan = roundToInt(raw("space_pan"));
            text(g, (pan == 0 ? String("c") : (pan < 0 ? "l" : "r") + String(std::abs(pan))) + u(" \xc2\xb7 ") + show("space_width"), 80, R2 + 60,
                 dimmed(primary, d), 12, true);
            focusLabels(g, d, focusText("space_focus_lo", "space_focus_hi"));
        };
    }
    void layoutSpaceArrows() // '>' follows the type name
    {
        auto& c = columns[(size_t) Module::space];
        for (auto* part : c.parts)
            if (part->getTitle() == "next space type")
                part->setTopLeftPosition(roundToInt(24 + textWidth(show("space_type"))), 150);
    }
    void buildPress(Column& c)
    {
        words(c, "press_mode", { "punch", "glue", "slam" }, 0, 0, 152), words(c, "press_mode", { "warm", "smooth", "multi" }, 3, 0, 170);
        dial(c, "press_mix", "mix", 0, R1);
        control(c, Control::Kind::fader, "press_downup", { 40, R2 + 10, 136, 12 }, true).hint = u("down/up \xc2\xb7 downward to upward compression");
        control(c, Control::Kind::fader, "press_bands", { 56, R2 + 38, 120, 12 }).hint = u("bands \xc2\xb7 single band to multiband");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            const float down = proc.display.pressDownDb.load(), up = proc.display.pressUpDb.load();
            centred(g, "down", 84, R1, 52, dimmed(muted, d), 11), centred(g, "up", 150, R1, 56, dimmed(muted, d), 11);
            vmeter(g, 107, R1 + 18, 10, -down / 12, true, accent, d), vmeter(g, 175, R1 + 18, 10, up / 6, false, primary, d);
            centred(g, format(spec("in_gain"), down), 80, R1 + 60, 58, dimmed(primary, d), 11, true);
            centred(g, format(spec("in_gain"), up), 148, R1 + 60, 60, dimmed(primary, d), 11, true);
            text(g, "down", 0, R2 + 8, dimmed(muted, d), 11), text(g, "up", 190, R2 + 8, dimmed(muted, d), 11);
            text(g, "bands", 0, R2 + 36, dimmed(muted, d), 11), text(g, "i", 46, R2 + 36, dimmed(muted, d), 11);
            text(g, "iii", 187, R2 + 36, dimmed(muted, d), 11);
        };
    }
    void buildDrive(Column& c)
    {
        words(c, "drive_mode", { "desk", "cassette", "tape" }, 0, 0, 152), words(c, "drive_mode", { "tube", "bits", "ring" }, 3, 0, 170);
        dial(c, "drive_flavor", "flavor", 0, R1, true), dial(c, "drive_mix", "mix", 76, R1);
        control(c, Control::Kind::fader, "drive_flavor", { 40, R2 + 10, 136, 12 }, true).hint = u("flavor \xc2\xb7 dark to open");
        targetRow(c, "drive_target"), focusRow(c, "drive_focus_lo", "drive_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            text(g, "dark", 0, R2 + 8, dimmed(muted, d), 11), text(g, "open", 187, R2 + 8, dimmed(muted, d), 11);
            targetLabels(g, d), focusLabels(g, d, focusText("drive_focus_lo", "drive_focus_hi"));
        };
    }
    void buildPush(Column& c)
    {
        words(c, "push_mult", { "1x", "2x", "4x" }, 0, 56, 152);
        dial(c, "push_tone", "tone", 0, R1, true), dial(c, "push_mix", "mix", 76, R1);
        targetRow(c, "push_target"), focusRow(c, "push_focus_lo", "push_focus_hi");
        c.body = [this, &c](Graphics& g) {
            const bool d = c.dim;
            const float limit = proc.display.pushLimitDb.load();
            text(g, "stages", 0, 152, dimmed(muted, d), 11), text(g, "limit", 0, R2 + 8, dimmed(muted, d), 11);
            g.setColour(dimmed(limit < -0.1f ? accent : colours::dim, d)), g.fillEllipse(42, R2 + 12, 6, 6);
            text(g, format(spec("in_gain"), limit), 56, R2 + 7, dimmed(primary, d), 12, true);
            targetLabels(g, d), focusLabels(g, d, focusText("push_focus_lo", "push_focus_hi"));
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
            columns[(size_t) m].setBounds(Rectangle<float>(colX(shown[(size_t) m]), columnTop, CW, 424).toNearestInt());
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
    void buildMaster()
    {
        const float y = masterTop + 32, my = masterTop + 68;
        dial(layer, "in_gain", "in", 24, y, true);
        toggleKey(layer, "eq_on", "eq", 108, my - 2);
        auto shapeKey = [&](const char* id, int value, Slope s, float x) {
            key(layer, String(id) + (value ? " steep" : " gentle"), { x, my - 3, 14, 16 },
                [this, id, value, s](Graphics& g, Key& k) { slope(g, s, 0, 3, roundToInt(raw(id)) == value || k.isMouseOver() ? primary : colours::dim); },
                [this, id, value](const ModifierKeys&) { setParam(id, (float) value); }, Key::Role::radio)
                .on = [this, id, value] { return roundToInt(raw(id)) == value; };
        };
        shapeKey("lc_shape", 0, Slope::lowGentle, 164), shapeKey("lc_shape", 1, Slope::lowSteep, 182);
        shapeKey("hc_shape", 0, Slope::highGentle, 380), shapeKey("hc_shape", 1, Slope::highSteep, 398);
        control(layer, Control::Kind::range, "cut_lo", { 204, my, 168, 12 }, false, "cut_hi").hint = u("cut \xc2\xb7 low and high cut; drag either end");
        dial(layer, "tone", "tone", 440, y, true);
        const std::pair<const char*, Point<float>> shapes[] { { "tilt", { 520, y + 18 } }, { "bump", { 572, y + 18 } }, { "mid", { 520, y + 42 } },
                                                               { "focus", { 572, y + 42 } } };
        for (int i = 0; i < 4; ++i)
        {
            const auto [label, at] = shapes[i];
            key(layer, String("tone shape ") + label, { at.x, at.y, 48, 20 },
                [this, i, label](Graphics& g, Key& k) {
                    const bool chosen = roundToInt(raw("tone_shape")) == i;
                    if (chosen)
                        g.setColour(tag), g.fillRect(k.getLocalBounds());
                    centred(g, label, 0, 2, 48, chosen || k.isMouseOver() ? primary : muted, 12, chosen);
                },
                [this, i](const ModifierKeys&) { setParam("tone_shape", (float) i); }, Key::Role::radio)
                .on = [this, i] { return roundToInt(raw("tone_shape")) == i; };
        }
        dial(layer, "air", "air", 656, y), dial(layer, "attack", "attack", 744, y, true), dial(layer, "sustain", "sustain", 820, y, true);
        toggleKey(layer, "clip_on", "clip", 916, my - 2);
        dial(layer, "out_gain", "out", 1000, y, true);
        key(layer, "loudness match", { 1072, my - 4, 72, 20 },
            [](Graphics& g, Key& k) {
                g.setColour(tag), g.fillRect(k.getLocalBounds());
                swapArrows(g, 6, 2, accent), text(g, "match", 22, 2, k.isMouseOver() ? primary : muted);
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
        const float y = masterTop + 32, my = masterTop + 68;
        text(g, "master", 24, masterTop, muted), hline(g, 84, masterTop + 8, 1310);
        centred(g, "cut", 204, y, 168, muted, 11);
        text(g, show("cut_lo"), 204, my + 20, muted, 11), right(g, show("cut_hi"), 272, my + 20, 100, muted, 11);
        centred(g, "shape", 520, y, 100, muted, 11);
        g.setColour(proc.clipping() && on("clip_on") ? accent : colours::dim), g.fillEllipse(962, my + 3, 7, 7);
        paintMeters(g);
    }
    Rectangle<int> metersArea() const { return { 1176, (int) masterTop + 28, 244, 80 }; }
    void paintMeters(Graphics& g)
    {
        const float y = masterTop + 32;
        text(g, "in/out", 1180, y, muted, 11);
        text(g, "in", 1180, y + 18, muted, 11), fader(g, 1212, y + 20, 152, FaderKind::meter, meterIn, 0, secondary);
        right(g, lufs(lufsIn), 1372, y + 18, 44, primary, 11);
        text(g, "out", 1180, y + 36, muted, 11), fader(g, 1212, y + 38, 152, FaderKind::meter, meterOut, 0, accent);
        right(g, lufs(lufsOut), 1372, y + 36, 44, primary, 11);
        text(g, "diff", 1180, y + 58, muted, 11);
        const bool both = lufsIn > LoudnessMeter::silence && lufsOut > LoudnessMeter::silence;
        right(g, both ? format(spec("in_gain"), lufsOut - lufsIn) : String("-"), 1316, y + 56, 100, primary, 12, true);
    }

    // ---- bottom bar -----------------------------------------------------------------------------------------------
    void buildBottomBar()
    {
        const float y = bottomTop + 26;
        key(layer, "undo", { 24, y - 2, 36, 20 }, [this](Graphics& g, Key& k) { text(g, "undo", 0, 2, proc.undoManager.canUndo() && k.isMouseOver() ? primary : muted); },
            [this](const ModifierKeys&) { proc.undo(); })
            .hint = "undo the last change";
        key(layer, "redo", { 68, y - 2, 36, 20 }, [this](Graphics& g, Key& k) { text(g, "redo", 0, 2, proc.undoManager.canRedo() && k.isMouseOver() ? primary : muted); },
            [this](const ModifierKeys&) { proc.redo(); })
            .hint = "redo";
        for (int i = 0; i < 5; ++i)
        {
            const String label = i < 4 ? String::charToString((juce_wchar) ('A' + i)) : String("+");
            key(layer, i < 4 ? "snapshot " + label : String("store snapshot"), { 1150.0f + 24 * (float) i, y - 2, 18, 18 },
                [this, i, label](Graphics& g, Key& k) {
                    const bool current = i == snapshot;
                    if (current)
                        g.setColour(primary), g.fillRect(k.getLocalBounds());
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
        text(g, "?", 150, bottomTop + 26, accent, 12, true);
        const auto line = Time::getMillisecondCounter() < noticeUntil ? notice : tip;
        text(g, line.substring(0, 132), 168, bottomTop + 26, muted, 12, false, 970);
    }
    Rectangle<int> tipArea() const { return { 146, (int) bottomTop + 20, 1000, 28 }; }

    // ---- paint, timer -------------------------------------------------------------------------------------------
    void paint(Graphics& g) override { g.fillAll(bg); }
    void paintLayer(Graphics& g)
    {
        const float mx = (float) magnitudeFader->getX();
        text(g, "magnitude", mx - 76, 20, muted), text(g, show("magnitude"), mx + 310, 20, primary, 12, true);
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
        const Levels levels { d.inPeak.exchange(0), d.outPeak.exchange(0) };
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
            c.dim = !on(modules[(size_t) m].onId.data());
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
        layoutTopBar(), layoutSpaceArrows();
        repaintAll();
    }
};

} // namespace
} // namespace snap::ui

AudioProcessorEditor* SnapProcessor::createEditor() { return new snap::ui::Editor(*this); }
