// crumb editor: design option 7b (crusher-design/v3/option_7b.html is the spec, coordinates in its 1024 x 576 px).
// One plate, scaled as a whole (50-200 %, fixed aspect). Knobs: drag, double-click = default, click the value to type.
// Every key shows its parameter (attachments), so automation, presets, A/B and state loads always show on the keys.
#include "BinaryData.h"
#include "Crumb.h"

using namespace juce;

namespace
{
const Colour face { 0xffd9d8d2 }, key { 0xffecebe6 }, darkKey { 0xff2a2a27 }, ink { 0xff1a1a18 }, muted { 0xff7a7a74 },
    muted2 { 0xff9a9993 }, orange { 0xffff5a1f }, blue { 0xff2f6bff }, white { 0xfff6f5f0 }, screen { 0xff0f100f },
    screenText { 0xffa5a59e }, axis { 0xff6b6b66 }, pill { 0xffc9c8c1 }, panel { 0xffcdccc5 }, filterPanel { 0xffc5c4bd },
    line { 0xffb0afa8 }, railDot { 0xff5a5a56 }, trace { 0xff5a5a55 }, tick { 0xffc8c7c0 }, numeral { 0xff8a8a84 }, ledOff { 0xff6a6a64 };
const String dot = String::fromUTF8(" \xc2\xb7 ");
String minus(const String& s) { return s.replace("-", String::fromUTF8("\xe2\x88\x92")); } // typographic minus, as the spec

// Geist Mono at a CSS font size in px with CSS letter-spacing in em. Weights: 0 regular, 1 medium (500), 2 semibold (600).
Font mono(float px, float trackingEm = 0.0f, int weight = 0)
{
    static const Typeface::Ptr faces[] = {
        Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
        Typeface::createSystemTypefaceFor(BinaryData::GeistMonoMedium_ttf, BinaryData::GeistMonoMedium_ttfSize),
        Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize),
    };
    Font font { FontOptions(faces[weight]).withPointHeight(px) };
    font.setExtraKerningFactor(trackingEm * font.getHeightToPointsFactor());
    return font;
}
Font label() { return mono(9, 0.08f); }
// Where the spec's renderer (Chrome) puts the baseline of a px-size Geist Mono line: in a line box of the given height,
// or (centred) a block of normal line height centred in a box. Fitted to Chrome: rounded ascent .862 em, descent .24 em.
float baseline(float top, float lineHeight, float px, bool centred = false)
{
    const float a = std::round(0.862f * px), d = std::round(0.24f * px), lead = (lineHeight - a - d) / 2;
    return top + (centred ? std::ceil(lead) : std::floor(lead)) + a;
}

// Text on a float baseline at x; align -1 left, 0 centred on x, 1 right-aligned at x. Returns its width.
float shapeText(GlyphArrangement& a, const String& s, const Font& f, float x, float base, int align)
{
    a.clear(), a.addLineOfText(f, s, 0, 0);
    const float w = a.getBoundingBox(0, a.getNumGlyphs(), true).getWidth();
    a.moveRangeOfGlyphs(0, -1, x - w * (float) (align + 1) / 2, base);
    return w;
}
float print(Graphics& g, const String& s, const Font& f, Colour c, float x, float base, int align = -1)
{
    GlyphArrangement a;
    const float w = shapeText(a, s, f, x, base, align);
    g.setColour(c), a.draw(g);
    return w;
}

// Key cap: 1 px drop line, darker 2 px bottom edge, radius 8. Dark / orange keys fill when on, an LED key lights its LED.
struct Key : Button
{
    enum Kind { dark, orangeFill, led } kind;
    Key(const String& t, Kind k = dark, const String& id = {}) : Button(t), kind(k) { setComponentID(id.isEmpty() ? t : id); }
    void paintButton(Graphics& g, bool over, bool down) override
    {
        const auto box = getLocalBounds().toFloat().withTrimmedBottom(1); // the component is one px taller: the drop line
        const bool on = getToggleState() && kind != led;
        Colour fill = on ? (kind == dark ? darkKey : orange) : key;
        fill = down ? fill.interpolatedWith(Colours::black, 0.08f) : over ? fill.interpolatedWith(on ? Colours::white : Colours::black, 0.03f) : fill;
        g.setColour(Colours::black.withAlpha(0.18f)), g.fillRoundedRectangle(box.translated(0, 1), 8);
        g.setColour(fill.interpolatedWith(Colours::black, 0.06f)), g.fillRoundedRectangle(box, 8);
        g.setColour(fill), g.fillRoundedRectangle(box.withTrimmedBottom(2), 8);
        print(g, getButtonText(), mono(10, 0.08f), on ? (kind == dark ? white : Colours::white) : ink, box.getCentreX(), baseline(0, box.getHeight(), 10, true), 0);
        if (kind == led && getToggleState())
            g.setColour(orange), g.fillEllipse(box.getRight() - 11, 5, 5, 5);
    }
};

// Knob: 56 px light ring with a soft shadow, 32 px coloured cap, ink dot; the slider's text box is the value.
struct Look : LookAndFeel_V4
{
    Font getLabelFont(Label&) override { return mono(11, 0.02f); }
    void drawLabel(Graphics& g, Label& l) override // the value under a knob, on the spec's baseline in its 16 px row
    {
        if (!l.isBeingEdited())
            print(g, l.getText(), getLabelFont(l), l.findColour(Label::textColourId), (float) l.getWidth() / 2, baseline(0, 16, 11), 0);
    }
    void drawRotarySlider(Graphics& g, int x, int y, int w, int, float pos, float a0, float a1, Slider& s) override
    {
        const auto r = Rectangle<float>(56, 56).withCentre({ (float) x + (float) w * 0.5f, (float) y + 28 });
        Path ring;
        ring.addEllipse(r);
        DropShadow(Colours::black.withAlpha(0.08f), 10, { 0, 6 }).drawForPath(g, ring);
        g.setColour(Colours::black.withAlpha(0.12f)), g.fillEllipse(r.translated(0, 3));
        g.setColour(key), g.fillPath(ring);
        g.setColour(s.findColour(Slider::thumbColourId)), g.fillEllipse(r.reduced(12));
        g.setColour(ink), g.fillEllipse(Rectangle<float>(8, 8).withCentre(r.getCentre().getPointOnCircumference(20.16f, a0 + pos * (a1 - a0))));
    }
};

// The module row. Post order: in | pre dock | quantize | approx | resample | images | filter | mix. Filter order pre swaps
// the filter and the empty dock. Children sit at offsets from their module's left edge; rows are the spec's centrelines.
enum { In, Dock, Quant, Approx, Resample, Images, Filter, Mix };
constexpr float MY = 296, top = MY + 68, bottom = MY + 180;
constexpr float widths[] = { 96, 40, 152, 96, 128, 96, 208, 104 };
struct KnobSpec { const char* id; const char* name; int module; float dx, cy, labelWidth; Colour cap; };
const KnobSpec knobSpecs[] = { { "preamp", "preamp", In, 48, top, 88, muted2 },        { "dither", "dither", In, 48, bottom, 88, muted2 },
                               { "approxDev", "deviate", Approx, 48, bottom, 88, blue }, { "rate", "freq", Resample, 64, top, 120, orange },
                               { "jitter", "jitter", Resample, 64, bottom, 120, blue },  { "imagesShift", "shift", Images, 48, bottom, 88, blue },
                               { "cutoff", "cutoff", Filter, 60, bottom, 80, muted2 },   { "reso", "reso", Filter, 148, bottom, 80, muted2 },
                               { "mix", "fx", Mix, 52, top, 96, white },                  { "output", "output", Mix, 52, bottom, 96, white } };

const char* const displayIds[] = { "preamp", "bits", "dcShift", "dither", "rate", "jitter", "approxOn", "approxDev", "imagesOn", "imagesShift", "fType", "fOrder", "cutoff", "reso", "output" };
const Identifier vuId[] { "vu0", "vu1" }, clipId[] { "clip0", "clip1" };

struct Editor : AudioProcessorEditor, Timer
{
    static constexpr int W = 1024, H = 576;
    struct Plate : Component
    {
        Editor& e;
        explicit Plate(Editor& ed) : e(ed) { setOpaque(true); }
        void paint(Graphics& g) override { e.paintPlate(g); }
    };

    CrumbProcessor& proc;
    Look look;
    Plate plate { *this };
    Slider knobs[10];
    Key prev { String::fromUTF8("\xe2\x97\x80"), Key::dark, "prev" }, next { String::fromUTF8("\xe2\x96\xb6"), Key::dark, "next" }, save { "save" },
        slotA { "A" }, slotB { "B" };
    Key minusKey { String::fromUTF8("\xe2\x88\x92"), Key::dark, "minus" }, plus { "+", Key::dark, "plus" }, dcShift { "dc shift", Key::dark, "dcShift" },
        approx { "approx", Key::orangeFill, "approxOn" }, images { "images", Key::led, "imagesOn" };
    Key pre { "pre" }, post { "post" }, types[5] { { "off" }, { "lp" }, { "bp" }, { "hp" }, { "br" } };
    // Attachments after the components they drive, so they are destroyed first.
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> knobAtt[10];
    std::vector<std::unique_ptr<ParameterAttachment>> keyAtt;
    std::vector<float> in = std::vector<float>(crumb::Scope::size), out = std::vector<float>(crumb::Scope::size);
    float scopePeak = 0.1f, needle[2] {}, speed[2] {}, level[2] {};
    crumb::Scope::Meter seen;   // the meter totals last read: opening the editor starts from the current position
    double stepAt = 0, heard = 0; // ms: needle time integrated up to, last new audio
    bool clip[2] {};
    std::array<std::atomic<float>*, 15> raw {};
    std::array<RangedAudioParameter*, 15> parameters {};
    std::array<float, 15> rawSeen {};
    String displayed[15], presetSeen, rateText, unitText, status[3];
    int presetIndexSeen = -2, slotSeen = -1;
    bool valuesSeen = false;
    uint32_t scopeSeen = 0;
    float paintedNeedle[2] {};
    Path waveform, shadow, hand;
    struct VuFace { Path arcs[2], ticks[7]; GlyphArrangement numbers[7]; bool built = false; } vuFace[2];

    explicit Editor(CrumbProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        for (size_t i = 0; i < raw.size(); ++i)
            raw[i] = p.apvts.getRawParameterValue(displayIds[i]), parameters[i] = p.apvts.getParameter(displayIds[i]);
        waveform.preallocateSpace(crumb::Scope::size * 3);
        setLookAndFeel(&look);
        addAndMakeVisible(plate);
        plate.setBounds(0, 0, W, H);
        for (int i = 0; i < 10; ++i)
        {
            auto& k = knobs[i];
            const auto& s = knobSpecs[i];
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(Slider::TextBoxBelow, false, (int) s.labelWidth, 16);
            k.setRotaryParameters(MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            k.setColour(Slider::thumbColourId, s.cap), k.setColour(Slider::textBoxTextColourId, ink), k.setColour(Slider::textBoxBackgroundColourId, {});
            k.setColour(Slider::textBoxOutlineColourId, {}), k.setColour(Slider::textBoxHighlightColourId, orange.withAlpha(0.3f));
            k.setColour(TextEditor::focusedOutlineColourId, {}), k.setTitle(s.name), k.setComponentID(s.id);
            knobAtt[i] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts, s.id, k);
            k.textFromValueFunction = [f = k.textFromValueFunction](double v) { return minus(f(v)); };
            k.updateText();
            auto* param = p.apvts.getParameter(s.id);
            k.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
            plate.addAndMakeVisible(k);
        }
        prev.onClick = [this] { proc.stepPreset(-1); };
        next.onClick = [this] { proc.stepPreset(1); };
        save.onClick = [this] { proc.saveUserPreset(); };
        slotA.onClick = [this] { proc.selectAb(0); };
        slotB.onClick = [this] { proc.selectAb(1); };
        place(prev, 552, 24, 24, 24), place(next, 584, 24, 24, 24), place(save, 616, 24, 48, 24), place(slotA, 672, 24, 24, 24), place(slotB, 704, 24, 24, 24);
        // Keys never toggle themselves: the attachment sets them from the parameter, a click only asks for a value.
        auto bind = [&](const char* id, std::vector<Key*> keys) {
            auto att = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(id), [keys](float v) {
                for (size_t k = 0; k < keys.size(); ++k)
                    keys[k]->setToggleState(keys.size() == 1 ? v > 0.5f : (int) k == roundToInt(v), dontSendNotification);
            });
            for (size_t k = 0; k < keys.size(); ++k)
                keys[k]->onClick = [a = att.get(), k, single = keys.size() == 1, key = keys[k]] { a->setValueAsCompleteGesture(single ? (key->getToggleState() ? 0.0f : 1.0f) : (float) k); };
            att->sendInitialUpdate();
            keyAtt.push_back(std::move(att));
        };
        bind("dcShift", { &dcShift }), bind("approxOn", { &approx }), bind("imagesOn", { &images });
        bind("fType", { &types[0], &types[1], &types[2], &types[3], &types[4] }), bind("fOrder", { &pre, &post });
        keyAtt.push_back(std::make_unique<ParameterAttachment>(*p.apvts.getParameter("bits"), [this](float) { plate.repaint(); }));
        auto* bitsAtt = keyAtt.back().get();
        minusKey.onClick = [this, bitsAtt] { bitsAtt->setValueAsCompleteGesture((float) jmax(0, bits() - 1)); };
        plus.onClick = [this, bitsAtt] { bitsAtt->setValueAsCompleteGesture((float) jmin(24, bits() + 1)); };
        layoutModules();

        const int stored = proc.editorWidth; // read first: setting the limits already resizes
        setResizable(true, false);
        setResizeLimits(W / 2, H / 2, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio((double) W / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, stored) : W;
        setSize(width, width * H / W);
        proc.scope.meter(seen), stepAt = heard = proc.scope.clock();
        timerCallback();
        startTimerHz(30);
    }
    ~Editor() override { setLookAndFeel(nullptr); }

    void place(Key& c, float x, float y, float w, float h)
    {
        if (c.getParentComponent() != &plate)
            plate.addAndMakeVisible(c);
        c.setBounds(Rectangle<float>(x, y, w, h + 1).toNearestInt()); // keys: + drop line
    }
    bool preOrder() const { return value("fOrder") < 0.5f; }
    std::array<int, 8> order() const { return { In, preOrder() ? Filter : Dock, Quant, Approx, Resample, Images, preOrder() ? Dock : Filter, Mix }; }
    std::array<float, 8> moduleX() const // left edge of each module; 8 px gutters from the 24 px margin
    {
        std::array<float, 8> x {};
        float at = 24;
        for (auto m : order())
            x[(size_t) m] = at, at += widths[m] + 8;
        return x;
    }
    void layoutModules()
    {
        const auto x = moduleX();
        for (int i = 0; i < 10; ++i)
        {
            const auto& s = knobSpecs[i];
            knobs[i].setBounds(Rectangle<float>(x[(size_t) s.module] + s.dx - s.labelWidth / 2, s.cy - 28, s.labelWidth, 80).toNearestInt());
        }
        const float q = x[Quant], f = x[Filter];
        place(minusKey, q + 8, MY + 40, 24, 96), place(plus, q + 120, MY + 40, 24, 96), place(dcShift, q + 8, MY + 168, 136, 32);
        place(approx, x[Approx] + 8, MY + 40, 80, 40), place(images, x[Images] + 8, MY + 40, 80, 40);
        place(pre, f + 8, MY + 40, 92, 40), place(post, f + 108, MY + 40, 92, 40);
        for (int k = 0; k < 5; ++k)
            place(types[k], f + 8 + 40 * (float) k, MY + 88, 32, 32);
    }
    void resized() override
    {
        plate.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }
    float value(const char* id) const { return proc.apvts.getRawParameterValue(id)->load(); }
    String display(const char* id) const
    {
        for (size_t i = 0; i < raw.size(); ++i)
            if (std::string_view(id) == displayIds[i]) return displayed[i];
        return {};
    }
    int bits() const { return roundToInt(value("bits")); }
    // Stages that currently do nothing are dimmed: their knobs, their rail dots.
    bool active(int module) const
    {
        return module == Quant ? bits() > 0 : module == Approx ? value("approxOn") > 0.5f : module == Images ? value("imagesOn") > 0.5f
             : module == Filter ? value("fType") > 0.5f : module != Dock;
    }
    bool knobActive(const KnobSpec& s) const
    {
        const String id = s.id;
        return active(s.module) && !(id == "dither" && (value("dither") <= 0 || bits() == 0)) && !(id == "jitter" && value("jitter") <= 0);
    }

    void timerCallback() override
    {
        const int slot = proc.abSlot(), pr = proc.preset.load();
        const auto name = proc.presetName();
        bool changed = !valuesSeen, chrome = !valuesSeen || slot != slotSeen || pr != presetIndexSeen || name != presetSeen;
        bool orderChanged = false;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            const float v = raw[i]->load();
            if (!valuesSeen || !exactlyEqual(v, rawSeen[i]))
            {
                changed = true, orderChanged |= i == 11;
                chrome |= i == 1 || i == 2 || i == 3 || i == 5 || i == 6 || i == 8 || i == 10 || i == 11;
                rawSeen[i] = v, displayed[i] = minus(parameters[i]->getText(parameters[i]->convertTo0to1(v), 0));
            }
        }
        if (changed)
            updateDisplay();
        if (orderChanged && valuesSeen)
            layoutModules();
        if (chrome)
        {
            slotA.setToggleState(slot == 0, dontSendNotification), slotB.setToggleState(slot == 1, dontSendNotification);
            for (int i = 0; i < 10; ++i)
                knobs[i].setAlpha(knobActive(knobSpecs[i]) ? 1.0f : 0.4f);
            dcShift.setAlpha(bits() > 0 ? 1.0f : 0.4f);
            plate.repaint();
        }
        valuesSeen = true, presetSeen = name, presetIndexSeen = pr, slotSeen = slot;
        // VU: the RMS of the audio since the last new block (held between blocks, silence after 0.5 s without audio)
        // drives a 2nd-order needle (99 % in 300 ms, 1.5 % overshoot) in 1/240 s steps of elapsed time, at most 1 s of
        // catch-up; 0 VU = -12 dBFS RMS (K-12). The clip LED lights for 1 s after a sample over 0 dBFS.
        const double t = proc.scope.clock();
        if (crumb::Scope::Meter m; proc.scope.meter(m) && m.frames != seen.frames)
        {
            for (int c = 0; c < 2; ++c)
                level[c] = (float) std::sqrt((m.energy[c] - seen.energy[c]) / (double) (m.frames - seen.frames)) * Decibels::decibelsToGain(12.0f);
            seen = m, heard = t;
        }
        else if (t - heard > 500)
            level[0] = level[1] = 0;
        int k = 0;
        for (; stepAt + 1000.0 / 240 <= t && k < 240; ++k, stepAt += 1000.0 / 240)
            for (int c = 0; c < 2; ++c)
                speed[c] += (169 * (level[c] - needle[c]) - 20.8f * speed[c]) / 240, needle[c] += speed[c] / 240;
        stepAt = k < 240 ? stepAt : t;
        const auto write = proc.scope.write.load(std::memory_order_acquire);
        bool screens = changed || write != scopeSeen;
        scopeSeen = write;
        for (int c = 0; c < 2; ++c)
        {
            const bool lit = proc.scope.clipAt[c].load() + 1000 > t;
            screens |= lit != clip[c] || std::abs(needle[c] - paintedNeedle[c]) > 1e-6f;
            clip[c] = lit;
            if (!getProperties().contains(vuId[c]) || !exactlyEqual((float) getProperties()[vuId[c]], needle[c]))
                getProperties().set(vuId[c], needle[c]); // for tests
            if (!getProperties().contains(clipId[c]) || (bool) getProperties()[clipId[c]] != clip[c])
                getProperties().set(clipId[c], clip[c]);
        }
        if (screens || chrome)
        {
            std::copy(std::begin(needle), std::end(needle), paintedNeedle);
            if (!chrome) plate.repaint(24, 56, 976, 216);
        }
    }

    void paintPlate(Graphics& g)
    {
        g.fillAll(face);
        const auto clipBounds = g.getClipBounds();
        if (clipBounds.intersects(Rectangle<int> { 24, 23, 976, 26 }))
        {
            // Header: wordmark, centred preset pill (number + name) with its keys, maker and version.
            const float hb = baseline(24, 24, 15);
            print(g, "crumb", mono(15, 0.01f, 2), ink, 24, hb);
            print(g, "daveydidit" + dot + "v" JucePlugin_VersionString, label(), muted, 1000, hb, 1);
            g.setColour(pill), g.fillRoundedRectangle(296, 24, 248, 24, 8);
            print(g, proc.preset >= 0 ? String(proc.preset.load()).paddedLeft('0', 2) : String("--"), mono(11, 0.02f), muted, 304, baseline(24, 24, 11));
            g.setFont(mono(11, 0.02f)), g.setColour(ink), g.drawText(proc.presetName(), Rectangle<float>(328, 23, 184, 24), Justification::centred, true); // on the spec baseline
        }
        if (clipBounds.intersects(Rectangle<int> { 24, 56, 632, 216 })) paintScope(g);
        if (clipBounds.intersects(Rectangle<int> { 664, 56, 336, 216 })) paintVu(g);

        // Signal rail: a dot at each module's centre; the filter (orange) where it sits, the empty dock hollow.
        const auto x = moduleX();
        if (clipBounds.intersects(Rectangle<int> { 24, 268, 976, 24 }))
        {
            g.setColour(line), g.drawLine(24, 280, 1000, 280, 2);
            for (int m = 0; m < 8; ++m)
            {
                const Point<float> c { x[(size_t) m] + widths[m] / 2, 280 };
                if (m == Dock)
                    g.setColour(face), g.fillEllipse(Rectangle<float>(16, 16).withCentre(c)), g.setColour(line), g.drawEllipse(Rectangle<float>(14, 14).withCentre(c), 2);
                else if (m == Filter && active(m))
                    g.setColour(orange.withAlpha(0.22f)), g.fillEllipse(Rectangle<float>(24, 24).withCentre(c)), g.setColour(orange), g.fillEllipse(Rectangle<float>(16, 16).withCentre(c));
                else
                    g.setColour(active(m) ? railDot : line), g.fillEllipse(Rectangle<float>(8, 8).withCentre(c));
            }
        }
        if (!clipBounds.intersects(Rectangle<int> { 24, 296, 976, 256 })) return;
        // Modules: panels with their names, knob names, the dock, the bits display, the flow arrows in the gutters.
        const char* names[] = { "in", "", "quantize", "approx", "resample", "images", "filter", "mix" };
        for (int m = 0; m < 8; ++m)
        {
            const Rectangle<float> r { x[(size_t) m], MY, widths[m], 256 };
            if (m == Dock)
            {
                // CSS 2 px dashed border: solid corners, 6 / 4 dashes centred on each side
                g.setColour(line);
                for (int k = 0; k < 4; ++k)
                {
                    const float c = (float) k * MathConstants<float>::halfPi;
                    Path corner;
                    corner.addCentredArc(k == 1 || k == 2 ? r.getRight() - 8 : r.getX() + 8, k < 2 ? MY + 8 : MY + 248, 7, 7, 0, c - MathConstants<float>::halfPi, c, true);
                    g.strokePath(corner, PathStrokeType(2));
                    const auto a = Point<float>(k == 1 ? r.getRight() - 1 : r.getX() + (k == 3 ? 1 : 6), k == 2 ? MY + 255 : MY + (k == 0 ? 1 : 6));
                    const auto b = k == 0 ? Point<float>(r.getRight() - 6, MY + 1) : k == 2 ? Point<float>(r.getRight() - 6, MY + 255) : Point<float>(a.x, MY + 250);
                    const float len = a.getDistanceFrom(b), n = std::floor((len + 4) / 10), off = (len - (n * 10 - 4)) / 2;
                    for (float d = off; d < len; d += 10)
                        g.drawLine(Line<float>(a + (b - a) * d / len, a + (b - a) * (d + 6) / len), 2);
                }
                const Point<float> c { r.getCentreX(), MY + 60 };
                g.setColour(line), g.drawEllipse(Rectangle<float>(14, 14).withCentre(c), 2);
                print(g, preOrder() ? "post" : "pre", label(), muted2, c.x, baseline(MY + 76, 16, 9), 0);
                continue;
            }
            // inset 0 1px 0 white .35 (top edge), inset 0 -1px 0 black .05 (bottom edge): crescents of the shifted shape
            const auto fill = m == Filter ? filterPanel : panel;
            Path shape, down, up;
            shape.addRoundedRectangle(r, 8), down.addRoundedRectangle(r.translated(0, 1), 8), up.addRoundedRectangle(r.translated(0, -1), 8);
            Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(shape);
            g.setColour(fill.interpolatedWith(Colours::white, 0.35f)), g.fillPath(shape);
            g.setColour(fill.interpolatedWith(Colours::black, 0.05f)), g.fillPath(down);
            g.reduceClipRegion(down);
            g.setColour(fill), g.fillPath(up);
            print(g, names[m], label(), muted, r.getX() + 8, baseline(MY + 8, 16, 9));
        }
        for (const auto& s : knobSpecs)
            print(g, String(s.name).toUpperCase(), label(), muted.withMultipliedAlpha(knobActive(s) ? 1.0f : 0.4f), x[(size_t) s.module] + s.dx, baseline(s.cy + 52, 16, 9), 0);
        const Rectangle<float> lcd { x[Quant] + 40, MY + 40, 72, 96 };
        g.setColour(screen), g.fillRoundedRectangle(lcd, 8);
        const auto big = mono(22, 0, 1);
        const String n = bits() > 0 ? String(bits()) : String("off");
        const float wn = GlyphArrangement::getStringWidth(big, n), wu = bits() > 0 ? 6 + GlyphArrangement::getStringWidth(label(), "BIT") - 0.72f : 0;
        const float x0 = lcd.getCentreX() - (wn + wu) / 2;
        print(g, n, big, bits() > 0 ? orange : axis, x0, baseline(MY + 77, 24, 22));
        if (bits() > 0)
            print(g, "BIT", label(), muted, x0 + wn + 6, baseline(MY + 81, 16, 9));
        for (int i = 0; i < 7; ++i)
        {
            const int m = order()[(size_t) i];
            const float gx = x[(size_t) m] + widths[m];
            Path p;
            p.addTriangle(gx + 1, MY + 141, gx + 7, MY + 144, gx + 1, MY + 147);
            g.setColour(line), g.fillPath(p);
        }
    }

    void updateDisplay()
    {
        const float rate = value("rate");
        unitText = (rate >= 1000 ? "khz" : "hz") + dot + (bits() > 0 ? String(bits()) + " bit" : "off");
        rateText = rate >= 1000 ? String(rate / 1000, 1) : String(roundToInt(rate));
        const String nextStatus[] = {
            String(value("dcShift") > 0.5f ? " mid-tread" : " mid-riser") + dot + "dither " + display("dither"),
            (value("approxOn") > 0.5f ? "approx " + display("approxDev") : String("approx off")) + dot
                + (value("imagesOn") > 0.5f ? "images +" + display("imagesShift") : String("images off")),
            (value("fType") > 0.5f ? String(StringArray { "", "lp", "bp", "hp", "br" }[roundToInt(value("fType"))]) + " " + display("cutoff") + dot + (preOrder() ? "pre" : "post")
                                   : String("filter off")) + dot + "jitter " + display("jitter"),
        };
        std::copy(std::begin(nextStatus), std::end(nextStatus), status);
    }

    // Scope screen: rate and resolution, three status lines, 20 ms of input (grey) and output (orange) started on a rising
    // zero crossing of the input so the picture stands still.
    void paintScope(Graphics& g)
    {
        g.setColour(screen), g.fillRoundedRectangle(24, 56, 632, 216, 12);
        const float b = baseline(72, 32, 32);
        print(g, unitText, mono(15, 0.08f), screenText, 48 + print(g, rateText, mono(32, 0.02f, 1), white, 40, b), b);
        for (int l = 0; l < 3; ++l)
        {
            const float w = print(g, status[l], mono(10), screenText, 640, baseline(72 + 16 * (float) l, 16, 10), 1);
            if (l == 0)
                print(g, String::fromUTF8("\xe2\x97\x8f"), mono(10), bits() > 0 ? orange : axis, 640 - w, baseline(72, 16, 10), 1);
        }
        const int n = jlimit(16, crumb::Scope::size / 2, roundToInt(proc.sampleRate.load() * 0.02)), m = jmin(crumb::Scope::size, 2 * n);
        proc.scope.read(in.data(), out.data(), m);
        int start = m - n;
        for (int i = m - n; i > 1; --i)
            if (in[(size_t) i - 1] <= 0 && in[(size_t) i] > 0)
            {
                start = i;
                break;
            }
        float peak = 0;
        for (int i = start; i < start + n; ++i)
            peak = jmax(peak, std::abs(in[(size_t) i]), std::abs(out[(size_t) i]));
        scopePeak = jmax(0.05f, peak, scopePeak * 0.95f); // auto gain: fast up, slow down
        for (auto [data, colour, width] : { std::tuple { in.data(), trace, 2.5f }, { out.data(), orange, 2.0f } })
        {
            auto& p = waveform;
            p.clear();
            for (int i = 0; i < n; ++i)
            {
                const float px = 40 + 600 * (float) i / (float) (n - 1), py = 188 - 54 * jlimit(-1.1f, 1.1f, data[start + i] / scopePeak);
                i == 0 ? p.startNewSubPath(px, py) : p.lineTo(px, py);
            }
            g.setColour(colour), g.strokePath(p, PathStrokeType(width, PathStrokeType::mitered, PathStrokeType::butt));
        }
        print(g, "IN", label(), muted, 40, 256), print(g, "OUT", label(), orange, 64, 256);
        print(g, "20 MS", label(), axis, 640, 256, 1);
    }

    // VU screen: two needles on a 20 / 10 / 0 / +3 scale (red from 0), clip LEDs L / R, the output setting.
    void paintVu(Graphics& g)
    {
        g.setColour(screen), g.fillRoundedRectangle(664, 56, 336, 216, 12);
        print(g, "out", label(), muted, 680, baseline(72, 32, 9));
        print(g, display("output"), label(), muted, 984, baseline(72, 32, 9), 1);
        for (int c = 0; c < 2; ++c)
        {
            const Point<float> led { 824 + 16 * (float) c, 88 }, pivot { 754 + 156 * (float) c, 232 };
            if (clip[c])
                g.setColour(orange.withAlpha(0.25f)), g.fillEllipse(Rectangle<float>(10, 10).withCentre(led));
            g.setColour(clip[c] ? orange : ledOff), g.fillEllipse(Rectangle<float>(6, 6).withCentre(led));
            // Scale position 0..1 from dB (the spec's stops), angle -140..-40 degrees from the x axis.
            auto at = [&](float t, float radius) { const float a = degreesToRadians(-140 + 100 * t); return pivot + Point<float>(std::cos(a), std::sin(a)) * radius; };
            auto pos = [](float db) {
                const float stops[][2] = { { -20, 0 }, { -10, 0.38f }, { -7, 0.52f }, { -5, 0.64f }, { -3, 0.75f }, { 0, 0.88f }, { 3, 1 } };
                int k = 0;
                while (k < 5 && db > stops[k + 1][0])
                    ++k;
                return stops[k][1] + (stops[k + 1][1] - stops[k][1]) * (db - stops[k][0]) / (stops[k + 1][0] - stops[k][0]);
            };
            const float r = 71.04f;
            auto& cached = vuFace[c];
            int arcIndex = 0, tickIndex = 0;
            for (auto [t0, t1, colour, width] : { std::tuple { 0.0f, 0.88f, tick, 2.0f }, { 0.88f, 1.0f, orange, 2.4f } })
            {
                auto& arc = cached.arcs[arcIndex++];
                if (!cached.built) arc.addCentredArc(pivot.x, pivot.y, r - 1, r - 1, 0, degreesToRadians(-50 + 100 * t0), degreesToRadians(-50 + 100 * t1), true);
                g.setColour(colour), g.strokePath(arc, PathStrokeType(width, PathStrokeType::curved, PathStrokeType::rounded));
            }
            for (auto [db, name] : { std::pair { -20.0f, "20" }, { -10.0f, "10" }, { -7.0f, "" }, { -5.0f, "" }, { -3.0f, "" }, { 0.0f, "0" }, { 3.0f, "+3" } })
            {
                const Colour colour = db >= 0 ? orange : tick;
                auto& mark = cached.ticks[tickIndex];
                auto& number = cached.numbers[tickIndex++];
                if (!cached.built)
                {
                    mark.addLineSegment(Line<float>(at(pos(db), r - 1), at(pos(db), r - 8)), 1.6f);
                    const auto p = at(pos(db), r + 16);
                    shapeText(number, name, mono(9, 0.02f), p.x, p.y + 2.4f, 0);
                }
                g.setColour(colour), g.fillPath(mark);
                g.setColour(db >= 0 ? orange : numeral), number.draw(g);
            }
            cached.built = true;
            const float t = jlimit(-0.04f, 1.03f, pos(Decibels::gainToDecibels(needle[c], -40.0f)));
            const auto tip = at(t, r - 14), d = (tip - pivot) / pivot.getDistanceFrom(tip), n = Point<float>(-d.y, d.x);
            shadow.clear(), hand.clear();
            shadow.addTriangle(pivot + Point<float>(1, 1.6f) + n * 1.2f, pivot + Point<float>(1, 1.6f) - n * 1.2f, tip + Point<float>(1, 1.6f));
            hand.addTriangle(pivot + n * 2.1f, pivot - n * 2.1f, tip);
            g.setColour(Colours::black.withAlpha(0.3f)), g.fillPath(shadow);
            g.setColour(orange), g.fillPath(hand);
            g.setColour(key), g.fillEllipse(Rectangle<float>(11, 11).withCentre(pivot));
            g.setColour(ink), g.fillEllipse(Rectangle<float>(5.2f, 5.2f).withCentre(pivot));
            print(g, c == 0 ? "L" : "R", label(), muted, pivot.x + 0.36f, 253.3f, 0);
        }
    }
};
} // namespace

AudioProcessorEditor* CrumbProcessor::createEditor() { return new Editor(*this); }
