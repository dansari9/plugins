// crumb editor: design option 5 (crusher-design/option_5.html is the spec). One 960 x 540 plate in mockup pixels,
// scaled as a whole (50-200 %, fixed aspect). Theme and widget drawing follow Rootnote's Faceplate (ui/Theme, ui/Widgets),
// interaction follows Snap: double-click a knob = default, click the value under it to type. Every key is bound to its
// parameter through an attachment, so host automation, presets, A/B and state loads always show on the keys.
#include "BinaryData.h"
#include "Crumb.h"

using namespace juce;

namespace
{
const Colour face { 0xffd9d8d2 }, key { 0xffecebe6 }, darkKey { 0xff2a2a27 }, ink { 0xff1a1a18 }, sub { 0xff6e6d68 },
    orange { 0xffff5a1f }, blue { 0xff2f6bff }, grey { 0xff9a9993 }, white { 0xfff6f5f0 }, screen { 0xff0f100f },
    screenText { 0xffa5a59e }, legend { 0xff7a7a74 }, axis { 0xff6b6b66 }, lcd { 0xffc9c8c1 }, panel { 0xffcdccc5 },
    ledOff { 0xffb9b8b1 }, trace { 0xff5a5a55 };
const String dot = String::fromUTF8(" \xc2\xb7 "), arrow = String::fromUTF8(" \xe2\x86\x92 ");

// Geist Mono at a CSS font size in px with CSS letter-spacing in em (as Rootnote's ui::mono).
Font mono(float px, int weight = 0, float trackingEm = 0.02f)
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
Font label() { return mono(9, 0, 0.08f); } // the small uppercase labels

// Text on a baseline; returns the x after it. Right-aligned when right is set (x is then the right edge).
float text(Graphics& g, const String& s, const Font& f, Colour c, float x, float baseline, bool right = false)
{
    const float w = GlyphArrangement::getStringWidth(f, s);
    g.setFont(f), g.setColour(c), g.drawSingleLineText(s, roundToInt(right ? x - w : x), roundToInt(baseline));
    return x + w;
}

// A key cap as in Rootnote: drop line, darker bottom edge. dark/orange keys fill when on, led keys light their LED.
struct Key : Button
{
    enum Kind { plain, dark, orangeFill, led } kind;
    int arrowDir = 0; // -1 / +1: a step triangle instead of text
    Key(const String& t, Kind k, const String& id = {}) : Button(t), kind(k) { setComponentID(id.isEmpty() ? t : id); }
    void paintButton(Graphics& g, bool over, bool down) override
    {
        const auto box = getLocalBounds().toFloat().withTrimmedBottom(1);
        const bool on = getToggleState() && kind != plain && kind != led;
        Colour fill = on ? (kind == dark ? darkKey : orange) : key, textColour = on ? white : ink;
        fill = down ? fill.interpolatedWith(Colours::black, 0.08f) : over ? fill.interpolatedWith(on ? Colours::white : Colours::black, 0.03f) : fill;
        g.setColour(Colours::black.withAlpha(0.18f)), g.fillRoundedRectangle(box.translated(0, 1), 8);
        g.setColour(fill.interpolatedWith(Colours::black, 0.06f)), g.fillRoundedRectangle(box, 8);
        g.setColour(fill), g.fillRoundedRectangle(box.withTrimmedBottom(2), 8);
        g.setColour(textColour);
        if (arrowDir != 0)
        {
            const auto c = box.getCentre();
            const float s = (float) arrowDir;
            Path p;
            p.addTriangle(c.x - 2.5f * s, c.y - 3, c.x - 2.5f * s, c.y + 3, c.x + 2.5f * s, c.y);
            g.fillPath(p);
        }
        g.setFont(mono(10, 0, 0.06f)), g.drawText(getButtonText(), box.withTrimmedBottom(1), Justification::centred, false);
        if (kind == led)
            g.setColour(getToggleState() ? orange : ledOff), g.fillEllipse(box.getRight() - 11, box.getY() + 3, 5, 5);
    }
};

// Encoder: light ring with a soft shadow, coloured cap, ink dot (Snap/Rootnote); the slider's own text box is the value.
struct Look : LookAndFeel_V4
{
    Font getLabelFont(Label&) override { return mono(12); }
    void drawRotarySlider(Graphics& g, int x, int y, int w, int, float pos, float a0, float a1, Slider& s) override
    {
        const auto r = Rectangle<float>((float) x + (float) w * 0.5f - 26, (float) y + 2, 52, 52);
        g.setColour(Colours::black.withAlpha(0.06f)), g.fillEllipse(r.translated(0, 4).expanded(1.5f));
        g.setColour(Colours::black.withAlpha(0.12f)), g.fillEllipse(r.translated(0, 3));
        g.setColour(key), g.fillEllipse(r);
        g.setColour(s.findColour(Slider::thumbColourId)), g.fillEllipse(r.reduced(10.4f));
        g.setColour(ink), g.fillEllipse(Rectangle<float>(8, 8).withCentre(r.getCentre().getPointOnCircumference(18.7f, a0 + pos * (a1 - a0))));
    }
};

struct Editor : AudioProcessorEditor, Timer
{
    static constexpr int W = 960, H = 540;
    static constexpr const char* knobIds[] = { "rate", "jitter", "approxDev", "imagesShift", "preamp", "dither", "cutoff", "reso", "mix", "output" };
    static constexpr const char* knobNames[] = { "FREQ", "JITTER", "DEVIATE", "SHIFT", "PREAMP", "DITHER", "CUTOFF", "RESO", "FX", "OUTPUT" };

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
    Key prev { "", Key::plain, "prev" }, next { "", Key::plain, "next" }, save { "save", Key::plain }, slotA { "A", Key::dark }, slotB { "B", Key::dark };
    Key minus { String::fromUTF8("\xe2\x88\x92"), Key::plain, "minus" }, plus { "+", Key::plain, "plus" };
    Key dcShift { "dc shift", Key::led, "dcShift" }, approx { "approx filter", Key::orangeFill, "approxOn" }, images { "images filter", Key::led, "imagesOn" };
    Key types[5] { { "off", Key::dark }, { "lp", Key::dark }, { "bp", Key::dark }, { "hp", Key::dark }, { "br", Key::dark } };
    Key pre { "pre", Key::dark }, post { "post", Key::dark };
    // Attachments after the components they drive, so they are destroyed first.
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> knobAtt[10];
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> toggleAtt[3];
    std::unique_ptr<ParameterAttachment> typeAtt, orderAtt, bitsAtt;
    std::vector<float> in = std::vector<float>(crumb::Scope::size), out = std::vector<float>(crumb::Scope::size);
    float meter[2] {}, scopePeak = 0.1f;
    String shown; // the parameter text on the plate: repaint all when it changes

    explicit Editor(CrumbProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setLookAndFeel(&look);
        addAndMakeVisible(plate);
        plate.setBounds(0, 0, W, H);
        const Colour caps[] = { orange, blue, blue, blue, grey, grey, grey, grey, white, white };
        for (int i = 0; i < 10; ++i)
        {
            auto& k = knobs[i];
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(Slider::TextBoxBelow, false, 86, 18);
            k.setRotaryParameters(MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
            k.setColour(Slider::thumbColourId, caps[i]), k.setColour(Slider::textBoxTextColourId, ink);
            k.setColour(Slider::textBoxOutlineColourId, {}), k.setColour(Slider::textBoxHighlightColourId, orange.withAlpha(0.3f));
            k.setColour(TextEditor::focusedOutlineColourId, {}), k.setTitle(knobNames[i]), k.setComponentID(knobIds[i]);
            knobAtt[i] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts, knobIds[i], k);
            auto* param = p.apvts.getParameter(knobIds[i]);
            k.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
            place(k, 609.0f + 86.0f * (float) (i % 4), 64.0f + 126.0f * (float) (i / 4), 86, 80);
        }
        prev.arrowDir = -1, next.arrowDir = 1;
        place(prev, 556, 9, 26, 26), place(next, 586, 9, 26, 26), place(save, 620, 9, 44, 26), place(slotA, 670, 9, 26, 26), place(slotB, 700, 9, 26, 26);
        prev.onClick = [this] { proc.stepPreset(-1); };
        next.onClick = [this] { proc.stepPreset(1); };
        save.onClick = [this] { proc.saveUserPreset(); };
        slotA.onClick = [this] { proc.selectAb(0); };
        slotB.onClick = [this] { proc.selectAb(1); };
        place(minus, 24, 328, 26, 40), place(plus, 148, 328, 26, 40);
        place(dcShift, 180, 328, 100, 40), place(approx, 300, 328, 138, 40), place(images, 446, 328, 138, 40);
        Key* toggles[] = { &dcShift, &approx, &images };
        for (int i = 0; i < 3; ++i)
        {
            toggles[i]->setClickingTogglesState(true);
            toggleAtt[i] = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment>(p.apvts, toggles[i]->getComponentID(), *toggles[i]);
        }
        // Radio keys: the attachment sets every key from the parameter; a click only asks for the value (no local toggling).
        auto radio = [&](const char* id, std::vector<Key*> keys) {
            auto att = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(id), [keys](float v) {
                for (size_t k = 0; k < keys.size(); ++k)
                    keys[k]->setToggleState((int) k == roundToInt(v), dontSendNotification);
            });
            for (size_t k = 0; k < keys.size(); ++k)
                keys[k]->onClick = [a = att.get(), k] { a->setValueAsCompleteGesture((float) k); };
            att->sendInitialUpdate();
            return att;
        };
        for (int k = 0; k < 5; ++k)
            place(types[k], 24 + 60.8f * (float) k, 400, 56.8f, 36);
        place(pre, 334, 400, 120, 36), place(post, 464, 400, 120, 36);
        typeAtt = radio("fType", { &types[0], &types[1], &types[2], &types[3], &types[4] });
        orderAtt = radio("fOrder", { &pre, &post });
        bitsAtt = std::make_unique<ParameterAttachment>(*p.apvts.getParameter("bits"), [this](float) { plate.repaint(); });
        minus.onClick = [this] { bitsAtt->setValueAsCompleteGesture((float) jmax(0, bits() - 1)); };
        plus.onClick = [this] { bitsAtt->setValueAsCompleteGesture((float) jmin(24, bits() + 1)); };

        const int stored = proc.editorWidth; // read first: setting the limits already resizes
        setResizable(true, false);
        setResizeLimits(W / 2, H / 2, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio((double) W / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, stored) : W;
        setSize(width, width * H / W);
        timerCallback();
        startTimerHz(30);
    }
    ~Editor() override { setLookAndFeel(nullptr); }

    void place(Component& c, float x, float y, float w, float h)
    {
        plate.addAndMakeVisible(c);
        c.setBounds(Rectangle<float>(x, y, w, h + (dynamic_cast<Key*>(&c) ? 1.0f : 0.0f)).toNearestInt()); // keys: + drop line
    }
    void resized() override
    {
        plate.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }
    float value(const char* id) const { return proc.apvts.getRawParameterValue(id)->load(); }
    String display(const char* id) const { auto* p = proc.apvts.getParameter(id); return p->getText(p->getValue(), 0); }
    int bits() const { return roundToInt(value("bits")); }

    void timerCallback() override
    {
        slotA.setToggleState(proc.abSlot() == 0, dontSendNotification), slotB.setToggleState(proc.abSlot() == 1, dontSendNotification);
        String now = proc.presetName() + String(proc.preset.load());
        for (auto* id : { "preamp", "bits", "dcShift", "dither", "rate", "jitter", "approxOn", "approxDev", "imagesOn", "imagesShift", "fType", "fOrder", "cutoff", "reso" })
            now << display(id) << "|";
        if (now != shown)
            shown = now, plate.repaint();
        for (int c = 0; c < 2; ++c) // peak hold with ~20 dB/s fall
            meter[c] = jmax(proc.scope.peak[c].exchange(0.0f), meter[c] * 0.93f);
        plate.repaint(24, 52, 560, 250), plate.repaint(830, 316, 106, 104);
    }

    void paintPlate(Graphics& g)
    {
        g.fillAll(face);
        // Header: wordmark, preset LCD, maker and version.
        text(g, "crumb", mono(13, 2), ink, 24, 28);
        text(g, String::fromUTF8("CR-1 \xc2\xb7 SAMPLER CRUSHER"), label(), sub, 80, 26);
        text(g, "DAVEYDIDIT" + dot + "V" JucePlugin_VersionString, label(), sub, 936, 26, true);
        g.setColour(lcd), g.fillRoundedRectangle(330, 9, 220, 26, 6);
        text(g, proc.preset >= 0 ? String(proc.preset.load()).paddedLeft('0', 2) : String("--"), mono(11), sub, 340, 26);
        g.setFont(mono(11)), g.setColour(ink), g.drawText(proc.presetName(), Rectangle<float>(366, 9, 158, 26), Justification::centred, true);

        // Screen: rate and resolution, status lines, live scope.
        g.setColour(screen), g.fillRoundedRectangle(24, 52, 560, 250, 12);
        const float rate = value("rate");
        const String big = rate >= 1000 ? String(rate / 1000, 1) : String(roundToInt(rate)), b = bits() > 0 ? String(bits()) + " bit" : "off";
        text(g, (rate >= 1000 ? " khz" : " hz") + dot + b, mono(16), screenText, text(g, big, mono(34, 1), white, 44, 100), 100);
        const String status[] = {
            String(value("dcShift") > 0.5f ? "mid-tread" : "mid-riser") + dot + "dither " + display("dither"),
            (value("approxOn") > 0.5f ? "approx " + display("approxDev").trimCharactersAtEnd("%") : String("approx off")) + dot
                + (value("imagesOn") > 0.5f ? "images +" + display("imagesShift") : String("images off")),
            filterText() + dot + "jitter " + display("jitter"),
        };
        for (int l = 0; l < 3; ++l)
            text(g, status[l], mono(10), screenText, 564, 82 + 16.0f * (float) l, true);
        text(g, String::fromUTF8("\xe2\x97\x8f"), mono(10), bits() > 0 ? orange : axis, 557 - GlyphArrangement::getStringWidth(mono(10), status[0]) - 6, 82, true);
        paintScope(g);
        text(g, "IN", mono(9, 0, 0.11f), legend, 44, 287), text(g, "OUT", mono(9, 0, 0.11f), orange, 68, 287);
        text(g, "20 MS", mono(9), axis, 564, 287, true);

        // Quantizer bits display, section labels, knob names, meter.
        for (auto [s, x, y] : { std::tuple { "QUANTIZER", 24, 320 }, { "RESAMPLER", 300, 320 }, { "FILTER", 24, 392 } })
            text(g, s, label(), sub, (float) x, (float) y);
        g.setColour(screen), g.fillRoundedRectangle(54, 328, 90, 40, 6);
        const auto f = mono(24, 1);
        const String n = bits() > 0 ? String(bits()) : String("off"), unit = bits() > 0 ? "BIT" : "";
        const float wn = GlyphArrangement::getStringWidth(f, n), wu = unit.isEmpty() ? 0 : 6 + GlyphArrangement::getStringWidth(label(), unit);
        text(g, unit, label(), legend, text(g, n, f, orange, 99 - (wn + wu) / 2, 357) + 6, 351);
        for (int i = 0; i < 10; ++i)
        {
            g.setFont(label()), g.setColour(sub);
            g.drawText(knobNames[i], Rectangle<float>(602.0f + 86.0f * (float) (i % 4), 146.0f + 126.0f * (float) (i / 4), 100, 11), Justification::centred);
        }
        g.setColour(panel), g.fillRoundedRectangle(830, 316, 106, 104, 8);
        text(g, "OUT", label(), sub, 842, 334);
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 14; ++s) // 14 segments, -48..0 dBFS
            {
                const bool lit = Decibels::gainToDecibels(meter[c], -100.0f) > -48.0f + 48.0f * (float) s / 14.0f;
                g.setColour(lit ? ink : ledOff), g.fillRoundedRectangle(880.0f + 10.0f * (float) c, 406.857f - 5.142857f * (float) s, 6, 3.1f, 1);
            }

        // Signal path: the chain as it runs now. Stages that do nothing are grey, the filter is bold at its position.
        g.setColour(panel), g.fillRoundedRectangle(24, 450, 912, 48, 8);
        text(g, "SIGNAL PATH", label(), sub, 36, 469);
        const bool filterOn = value("fType") > 0.5f, preFilter = value("fOrder") < 0.5f;
        const String filter = "filter (" + String(preFilter ? "pre" : "post") + ")";
        std::vector<std::pair<String, bool>> path { { "preamp", value("preamp") > 0 } };
        if (preFilter)
            path.push_back({ filter, filterOn });
        path.insert(path.end(), { { "dither", value("dither") > 0 && bits() > 0 }, { "quantize " + b, bits() > 0 }, { "approx lp", value("approxOn") > 0.5f },
                                  { "resample " + String(rate / 1000, rate >= 1000 ? 1 : 2) + " khz" + (value("jitter") > 0 ? String::fromUTF8(" \xc2\xb1 jitter") : ""), true },
                                  { "images lp", value("imagesOn") > 0.5f } });
        if (!preFilter)
            path.push_back({ filter, filterOn });
        path.insert(path.end(), { { "fx", true }, { "out", true } });
        float x = 36;
        for (auto& [s, on] : path)
        {
            x = text(g, s, mono(11, s == filter && on ? 2 : 0), on ? ink : sub, x, 488);
            if (&s != &path.back().first)
                x = text(g, arrow, mono(11), ink, x, 488);
        }
        text(g, "daveydidit", mono(9), sub, 24, 522);
        text(g, "DOUBLE-CLICK = RESET" + dot + "CLICK VALUE TO TYPE", label(), sub, 936, 522, true);
    }

    String filterText() const
    {
        const int type = roundToInt(value("fType"));
        if (type == 0)
            return "filter off";
        return String(StringArray { "", "lp", "bp", "hp", "br" }[type]) + " " + display("cutoff") + dot + (value("fOrder") < 0.5f ? "pre" : "post");
    }

    // 20 ms of input (grey) and output (orange), started on a rising zero crossing of the input so the picture stands still.
    void paintScope(Graphics& g)
    {
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
            Path p;
            for (int i = 0; i < n; ++i)
            {
                const float px = 44 + 520 * (float) i / (float) (n - 1), py = 185 - 64 * jlimit(-1.1f, 1.1f, data[start + i] / scopePeak);
                i == 0 ? p.startNewSubPath(px, py) : p.lineTo(px, py);
            }
            g.setColour(colour), g.strokePath(p, PathStrokeType(width, PathStrokeType::mitered, PathStrokeType::square));
        }
    }
};
} // namespace

AudioProcessorEditor* CrumbProcessor::createEditor() { return new Editor(*this); }
