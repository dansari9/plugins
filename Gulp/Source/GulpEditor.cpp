// gulp editor: ASCII design option 2b, "tide" (sidechain-design/ascii/option_2b.*, made by src/grid.py + opt2b.py).
// One 120 x 34 cell grid scaled with the window (50-200 %, fixed aspect), as in crumb. Text is Geist Mono in its cells;
// braille, box, block, shade and geometric glyphs are vectors per cell, so they join. At 30 Hz, when anything it shows
// changed, the grid is rebuilt; only cells that changed are redrawn into a device-pixel image and repainted, so an idle
// editor costs nothing. Controls are invisible Sliders and Buttons on their hit cells in a 1024 x 576 layer scaled with
// the window: drag, double-click = default and click the value to type, exactly as crumb.
#include "BinaryData.h"
#include "Gulp.h"
#include <cstring>
#include <numeric>

using namespace juce;

namespace
{
constexpr int C = 120, R = 34, W = 1024, H = 576, L = 22, AX = 25, AW = 70, AT = 5, AH = 11, RX = 98, CY = 21, B = 24;
constexpr int rateX = AX + 18, syncX = rateX + 17, rateCount = 3, wordsY = B + 2, secondaryY = B + 4, readoutY = B + 5;
constexpr float cw = (float) W / C, ch = (float) H / R;
const Colour page { 0xff0d0d0d }, white { 0xffececec }, grey { 0xff7a7a7a }, rule { 0xff2c2c2c }, dark { 0xff454545 }, red { 0xffff3d1f }, kick { 0xff5a5a5a };
String u(const char* s) { return String::fromUTF8(s); }
String minus(const String& s) { return s.replace("-", u("−")); }
Rectangle<int> cells(int x, int y, int w, int h) // edges rounded, so neighbours share an edge and never overlap
{
    return Rectangle<int>::leftTopRightBottom(roundToInt(cw * (float) x), roundToInt(ch * (float) y), roundToInt(cw * (float) (x + w)), roundToInt(ch * (float) (y + h)));
}
constexpr uint8 brailleBit(int dx, int dy) { return (uint8) (dy == 3 ? 0x40 << dx : 1 << (dy + 3 * dx)); }
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size));
}
const StringArray triggerWords { "sync", "midi", "audio" }, bandWords { "full", "lows", "highs" }, stereoWords { "both", "mid", "side" };
const StringArray curveWords(gulp::curveNames, gulp::numCurves), colourWords(gulp::colorNames, gulp::numColors);

struct Cell
{
    juce_wchar c = ' ';
    Colour fg, bg; // bg transparent: the page
    bool bold = false;
    juce_wchar left = 0; // nonzero: half-cell centring, with the previous glyph's right half clipped to this cell
    bool operator!=(const Cell& o) const { return c != o.c || fg != o.fg || bg != o.bg || bold != o.bold || left != o.left; }
};
enum Mode { merge, under, over }; // braille onto braille: or the dots, keep the old cell, replace it (grid.py)
using Dots = std::vector<Point<int>>;
struct Grid
{
    Cell at[R][C];
    void put(int x, int y, const String& s, Colour fg, bool bold = false, Colour bg = {})
    {
        for (auto p = s.getCharPointer(); !p.isEmpty(); ++x)
            if (const auto c = p.getAndAdvance(); isPositiveAndBelow(x, C) && isPositiveAndBelow(y, R))
                at[y][x] = { c, fg, bg, bold };
    }
    void centre(int x, int w, int y, String s, Colour fg, bool bold = false)
    {
        s = s.substring(0, w), x += (w - s.length()) / 2;
        put(x, y, s, fg, bold);
        if ((w - s.length()) % 2 != 0)
            for (int k = 0; k <= s.length(); ++k)
                if (isPositiveAndBelow(x + k, C) && isPositiveAndBelow(y, R))
                    at[y][x + k] = { k < s.length() ? s[k] : ' ', fg, {}, bold, k > 0 ? s[k - 1] : ' ' };
    }
    void right(int x, int y, const String& s, Colour fg) { put(x - s.length() + 1, y, s, fg); }
    void braille(int x, int y, int w, int h, const Dots& dots, Colour fg, Mode mode = merge) // dots in a 2w x 4h space
    {
        std::vector<uint8> bits((size_t) (w * h));
        for (auto d : dots)
            if (isPositiveAndBelow(d.x, 2 * w) && isPositiveAndBelow(d.y, 4 * h))
                bits[(size_t) (d.y / 4 * w + d.x / 2)] |= brailleBit(d.x % 2, d.y % 4);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                if (int v = bits[(size_t) (j * w + i)]; v != 0 && isPositiveAndBelow(x + i, C) && isPositiveAndBelow(y + j, R))
                {
                    auto& cell = at[y + j][x + i];
                    const bool old = cell.c >= 0x2800 && cell.c <= 0x28ff;
                    if (old && mode == under) continue;
                    cell = { (juce_wchar) (0x2800 + (v | (old && mode == merge ? (int) cell.c - 0x2800 : 0))), fg, {}, false };
                }
    }
    int words(int x, int y, const StringArray& items, int sel) // the selected word red and underlined; returns the end
    {
        for (int k = 0; k < items.size(); x += items[k].length() + 1, ++k)
        {
            put(x, y, items[k], k == sel ? red : grey, k == sel);
            if (k == sel) put(x, y + 1, String::repeatedString(u("▔"), items[k].length()), red);
        }
        return x;
    }
    void toggle(int x, int y, const String& label, bool on, bool dim = false)
    {
        put(x, y, u(on ? "■" : "□"), dim ? dark : on ? red : grey), put(x + 2, y, label, dim ? dark : on ? white : grey, on && !dim);
    }
};

// Controls draw nothing: the grid shows them. The value box only appears while a value is typed.
struct Look : LookAndFeel_V4
{
    Font getLabelFont(Label&) override { return mono(cw / 0.6f, false); }
    void drawLabel(Graphics&, Label&) override {}
    void drawRotarySlider(Graphics&, int, int, int, int, float, float, float, Slider&) override {}
    Label* createSliderTextBox(Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox(s);
        l->setColour(Label::backgroundWhenEditingColourId, page), l->setColour(Label::textWhenEditingColourId, white);
        l->setColour(Label::outlineWhenEditingColourId, {}), l->setColour(TextEditor::highlightColourId, red.withAlpha(0.4f));
        l->setColour(TextEditor::highlightedTextColourId, white), l->setColour(CaretComponent::caretColourId, red);
        return l;
    }
    Slider::SliderLayout getSliderLayout(Slider& s) override // the slope's value box follows its label on the curve
    {
        auto layout = LookAndFeel_V4::getSliderLayout(s);
        if (s.getProperties().contains("box"))
            layout.sliderBounds = s.getLocalBounds(), layout.textBoxBounds = Rectangle<int>::fromString(s.getProperties()["box"].toString());
        return layout;
    }
};
struct Key : Button
{
    explicit Key(const String& id) : Button(id) { setComponentID(id), setRepaintsOnMouseActivity(false); }
    void paintButton(Graphics&, bool, bool) override {}
};

struct KnobSpec { const char* id; int x, y; bool centred; };
const KnobSpec knobSpecs[] = { { "shift", AX + 42, B + 1, true }, { "thresh", AX + 52, B + 1, false }, { "tone", AX + 62, B + 1, false },
                               { "depth", RX + 7, 4, false },     { "split", RX + 7, 14, false },     { "output", RX + 7, 24, false } };

struct Editor : AudioProcessorEditor, Timer
{
    GulpProcessor& proc;
    Look look;
    Component layer; // the controls, in design pixels
    Slider knobs[6], slope, freeHz;
    Key prev { "prev" }, next { "next" }, save { "save" }, slotA { "A" }, slotB { "B" }, invert { "invert" }, loop { "loop" }, free { "free" }, sync { "sync" }, down { "rate-" }, up { "rate+" };
    OwnedArray<Key> curves, colours, stereos, triggers, bands, rates;
    // Attachments after the components they drive, so they are destroyed first.
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment>> sliderAtt;
    std::vector<std::unique_ptr<ParameterAttachment>> keyAtt;
    ParameterAttachment* rateAtt = nullptr;
    int windowStart = 0; // the first of the three rate words shown
    Grid shown;
    std::vector<float> drawnFrom; // what the shown grid was built from: the same again means idle
    String drawnName;
    Image image; // the shown grid in device pixels
    Font fonts[2] { mono(1, false), mono(1, true) };

    explicit Editor(GulpProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setOpaque(true), setLookAndFeel(&look);
        addAndMakeVisible(layer);
        layer.setBounds(0, 0, W, H);
        const auto setup = [&](Slider& k, const char* id, Rectangle<int> bounds) {
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(Slider::TextBoxBelow, false, bounds.getWidth(), roundToInt(ch));
            k.setComponentID(id), k.setRepaintsOnMouseActivity(false);
            sliderAtt.push_back(std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts, id, k));
            k.textFromValueFunction = [f = k.textFromValueFunction](double v) { return minus(f(v)); };
            k.updateText();
            auto* param = p.apvts.getParameter(id);
            k.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
            k.setBounds(bounds);
            layer.addAndMakeVisible(k);
        };
        for (int i = 0; i < 6; ++i)
            setup(knobs[i], knobSpecs[i].id, cells(knobSpecs[i].x - 1, knobSpecs[i].y - 1, 10, 6));
        slope.getProperties().set("box", Rectangle<int>().toString());
        setup(slope, "slope", cells(AX + 2, AT, AW - 4, AH));
        setup(freeHz, "freeHz", cells(rateX, B, 10, 3)); // drag the rows above the value, click the value to type

        prev.onClick = [this] { proc.stepPreset(-1); };
        next.onClick = [this] { proc.stepPreset(1); };
        save.onClick = [this] { proc.saveUserPreset(); };
        slotA.onClick = [this] { proc.selectAb(0); };
        slotB.onClick = [this] { proc.selectAb(1); };
        for (auto [key, x, w] : { std::tuple { &prev, 30, 2 }, { &next, 62, 2 }, { &save, 67, 4 }, { &slotA, 74, 1 }, { &slotB, 76, 1 } })
            layer.addAndMakeVisible(*key), key->setBounds(cells(x, 1, w, 1));
        // Keys never toggle themselves: the attachment sets them from the parameter, a click only asks for a value.
        const auto bind = [&](const char* id, std::vector<Key*> keys) {
            auto att = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(id), [keys](float v) {
                for (size_t k = 0; k < keys.size(); ++k)
                    keys[k]->setToggleState(keys.size() == 1 ? v > 0.5f : (int) k == roundToInt(v), dontSendNotification);
            });
            for (size_t k = 0; k < keys.size(); ++k)
                keys[k]->onClick = [a = att.get(), k, single = keys.size() == 1, key = keys[k]] { a->setValueAsCompleteGesture(single ? (key->getToggleState() ? 0.0f : 1.0f) : (float) k); };
            att->sendInitialUpdate();
            keyAtt.push_back(std::move(att));
            return keyAtt.back().get();
        };
        const auto row = [&](OwnedArray<Key>& keys, const char* id, const StringArray& items, int x, int y, bool vertical = false) {
            std::vector<Key*> list;
            for (int k = 0; k < items.size(); ++k)
            {
                auto* key = keys.add(new Key(String(id) + " " + items[k]));
                key->setBounds(vertical ? cells(2, 5 + 2 * k, 19, 2) : cells(x, y, items[k].length(), 2));
                layer.addAndMakeVisible(key), list.push_back(key), x += items[k].length() + 1;
            }
            bind(id, list);
        };
        row(curves, "curve", curveWords, 0, 0, true);
        row(colours, "color", colourWords, AX + 6, CY), row(stereos, "stereo", stereoWords, AX + 57, CY);
        row(triggers, "trigger", triggerWords, AX, wordsY), row(bands, "band", bandWords, RX + 3, 11);
        bind("invert", { &invert }), bind("loop", { &loop });
        bind("tempoSync", { &free, &sync }); // explicit choices: free = 0, sync = 1, including repeated clicks
        keyAtt.push_back(std::make_unique<ParameterAttachment>(*p.apvts.getParameter("rate"), [](float) {}));
        rateAtt = keyAtt.back().get();
        for (int k = 0; k < rateCount; ++k)
        {
            auto* key = rates.add(new Key("rate " + String(k)));
            key->onClick = [this, k] { rateAtt->setValueAsCompleteGesture((float) (windowStart + k)); };
            layer.addChildComponent(key);
        }
        down.onClick = [this] { rateAtt->setValueAsCompleteGesture((float) jmax(0, rate() - 1)); };
        up.onClick = [this] { rateAtt->setValueAsCompleteGesture((float) jmin(gulp::numRates - 1, rate() + 1)); };
        for (auto [key, r] : { std::pair { &invert, cells(12, 3, 8, 1) }, { &loop, cells(AX, secondaryY, 6, 1) },
                               { &sync, cells(syncX, wordsY, 4, 2) }, { &free, cells(syncX, secondaryY, 4, 2) },
                               { &down, cells(rateX, secondaryY, 1, 1) }, { &up, cells(rateX + 7, secondaryY, 1, 1) } })
            layer.addAndMakeVisible(*key), key->setBounds(r);
        windowStart = jlimit(0, gulp::numRates - rateCount, rate() - 1);

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

    float value(const char* id) const { return proc.apvts.getRawParameterValue(id)->load(); }
    int index(const char* id) const { return roundToInt(value(id)); }
    int rate() const { return index("rate"); }
    String text(const char* id) const { return minus(proc.apvts.getParameter(id)->getCurrentValueAsText()); }
    void resized() override
    {
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }

    void timerCallback() override
    {
        std::vector<float> now;
        for (auto* param : proc.getParameters())
            now.push_back(param->getValue());
        const auto& f = proc.feed;
        now.insert(now.end(), { f.phase.load(), f.duck.load(), f.bpm.load(), f.scLevel.load(), f.outLevel.load(), (float) proc.preset.load(), (float) proc.abSlot() });
        for (size_t k = 0; k < gulp::Feed::bins; ++k)
            now.insert(now.end(), { f.lo[k].load(), f.hi[k].load(), f.outLo[k].load(), f.outHi[k].load() });
        if (now == drawnFrom && proc.presetName() == drawnName)
            return;
        drawnFrom = std::move(now), drawnName = proc.presetName();
        refresh();
    }

    // A round braille dial, 8 x 4 cells = 16 x 16 dots: a dotted 270 degree track, the value arc (centred: from the top)
    // and a two-dot needle, layered as common.py's knob: track grey, arc red (merged), needle white (over).
    static void knob(Grid& g, int x, int y, float norm, const String& name, const String& valueText, bool centred, bool dim)
    {
        Dots track, arc, needle;
        const double c = 7.5, r = 7.2, v = 270.0 * norm;
        for (int t = 0; t <= 270; t += 3)
        {
            const double a = degreesToRadians(225.0 + t);
            ((centred ? jmin(135.0, v) <= t && t <= jmax(135.0, v) : t <= v) ? arc : track).push_back({ (int) (c + r * std::sin(a)), (int) (c - r * std::cos(a)) });
        }
        const double a = degreesToRadians(225 + v), sn = std::sin(a), cs = std::cos(a);
        for (auto [x0, y0, len] : { std::tuple { c, c, r - 2 }, { c + 0.5, c + 0.5, r - 2.5 } }) // common.py's dotline, Python rounding
        {
            const int steps = (int) jmax(std::abs(len * sn), std::abs(len * cs)) + 1;
            for (int k = 0; k < steps; ++k)
                needle.push_back({ (int) std::nearbyint(x0 + len * sn * k / jmax(1, steps - 1)), (int) std::nearbyint(y0 - len * cs * k / jmax(1, steps - 1)) });
        }
        g.braille(x, y, 8, 4, track, dim ? rule : dark), g.braille(x, y, 8, 4, arc, dim ? dark : red), g.braille(x, y, 8, 4, needle, dim ? grey : white, over);
        g.centre(x - 3, 14, y - 1, name, dim ? dark : grey);
        g.centre(x - 3, 14, y + 4, valueText, dim ? grey : white, !dim);
    }

    void refresh()
    {
        Grid g;
        const int curve = index("curve"), trigger = index("trigger"), band = index("band"), colour = index("color"), stereo = index("stereo"), r = rate();
        const bool isFree = value("tempoSync") < 0.5f, inverted = value("invert") > 0.5f;
        const double s = std::exp2(value("slope") / 100.0), depth = value("depth") / 100.0, bpm = proc.feed.bpm.load();
        const auto gain = [&](double p) { const double d = gulp::duck(curve, p, s); return 1 - depth * (inverted ? 1 - d : d); };
        // Header: name, preset number and name with prev / next, save, A / B, maker.
        g.put(2, 1, "gulp", white, true);
        g.put(30, 1, u("‹"), grey), g.put(63, 1, u("›"), grey), g.put(67, 1, "save", grey);
        g.put(33, 1, proc.preset >= 0 ? String(proc.preset.load()).paddedLeft('0', 2) : String("--"), grey);
        g.centre(36, 26, 1, proc.presetName(), white, true);
        for (int k = 0; k < 2; ++k)
            g.put(74 + 2 * k, 1, k == 0 ? "A" : "B", proc.abSlot() == k ? page : grey, proc.abSlot() == k, proc.abSlot() == k ? white : Colour());
        g.right(C - 3, 1, "daveydidit", dark);
        g.put(2, 2, String::repeatedString(u("─"), C - 4), rule), g.put(2, R - 3, String::repeatedString(u("─"), C - 4), rule);
        for (int j = 3; j < R - 3; ++j)
            g.put(L, j, u("│"), rule), g.put(RX - 1, j, u("│"), rule);

        // Curve list: number, thumbnail, name, ◀ on the selected one; invert.
        g.put(2, 3, "curve", grey), g.toggle(12, 3, "invert", inverted);
        for (int i = 0; i < gulp::numCurves; ++i)
        {
            const int y = 5 + 2 * i;
            const bool sel = i == curve;
            Dots thumb;
            for (int k = 0; k < 11; ++k)
            {
                const double a = gulp::duck(i, k / 12.0, 1) * 3, b = gulp::duck(i, (k + 1) / 12.0, 1) * 3;
                for (int t = (int) std::nearbyint(jmin(a, b)); t <= (int) std::nearbyint(jmax(a, b)); ++t)
                    thumb.push_back({ k, t });
            }
            g.put(2, y, String(i + 1).paddedLeft('0', 2), sel ? red : dark, sel);
            g.braille(5, y, 6, 1, thumb, sel ? red : grey);
            g.put(12, y, curveWords[i], sel ? white : grey, sel);
            if (sel) g.put(20, y, u("◀"), red);
        }

        // The tide: one cycle of gain, the water (audio that passes) under it, the sidechain's kick as a grey ghost.
        const float duck = proc.feed.duck.load();
        g.put(AX, AT - 2, "duck", grey);
        g.put(AX + 5, AT - 2, duck > 0.999f ? u("−inf dB") : minus(String(Decibels::gainToDecibels(1.0f - duck), 1)) + " dB", red, true);
        g.right(AX + AW - 1, AT - 2, "one cycle = " + (isFree ? minus(text("freeHz")) : String(gulp::rateNames[r])), dark);
        for (int i = 0; i < AW; ++i)
            for (int j = AT + (int) std::nearbyint((1 - gain((i + 0.5) / AW)) * AH) + 1; j < AT + AH; ++j)
                g.put(AX + i, j, u("░"), rule);
        Dots line, ghost;
        for (int k = 0; k < 2 * AW; ++k)
        {
            const double y = (1 - gain(k / (2.0 * AW))) * (4 * AH - 1);
            line.push_back({ k, (int) y }), line.push_back({ k, (int) (y + 1) });
            if (k > 0)
            {
                const double py = (1 - gain((k - 1) / (2.0 * AW))) * (4 * AH - 1);
                for (int t = (int) jmin(py, y); t <= (int) jmax(py, y); ++t)
                    line.push_back({ k, t });
            }
        }
        float peak = 0;
        for (int k = 0; k < gulp::Feed::bins; ++k)
            peak = jmax(peak, std::abs(proc.feed.lo[(size_t) k].load()), std::abs(proc.feed.hi[(size_t) k].load()));
        const auto row = [peak](float v) { return (int) std::nearbyint((1 - (jlimit(-1.0f, 1.0f, v / jmax(0.05f, peak) * 0.6f) + 1) / 2) * (4 * AH - 1)); };
        for (int k = 0; k < gulp::Feed::bins; ++k)
            if (const float lo = proc.feed.lo[(size_t) k].load(), hi = proc.feed.hi[(size_t) k].load(); jmax(-lo, hi) > 0.03f * peak)
                for (int t = row(hi); t <= row(lo); ++t)
                    ghost.push_back({ k, t });
        g.braille(AX, AT, AW, AH, line, red), g.braille(AX, AT, AW, AH, ghost, kick, under);
        // Slope handle on the release (moves with the slope), its label is the value to type.
        const double hp = jlimit(0.05, 0.9, 0.3 * s);
        const int hx = AX + (int) (AW * hp), hy = jlimit(AT, AT + AH - 2, AT + (int) ((1 - gain(hp)) * AH));
        const String label = "slope " + text("slope");
        const int lx = hx + 2 + label.length() <= AX + AW ? hx + 2 : hx - 1 - label.length(), ly = hy > AT ? hy - 1 : hy + 1;
        g.put(hx, hy, u("◆"), white), g.put(hx - 3, hy + 1, u("◀ ▶"), grey), g.put(lx, ly, label, white, true);
        if (const auto box = (cells(lx, ly, label.length(), 1) - cells(AX + 2, AT, 0, 0).getPosition()).toString(); slope.getProperties()["box"].toString() != box)
            slope.getProperties().set("box", box), slope.resized();
        // Playhead and ruler in fractions of the rate (or time when free).
        const int ph = AX + jmin(AW - 1, (int) (AW * proc.feed.phase.load()));
        for (int j = AT; j < AT + AH; ++j)
            if (g.at[j][ph].c == ' ' || g.at[j][ph].c == 0x2591) g.put(ph, j, u("┊"), red);
        for (int i = 0; i <= 16; ++i)
            g.put(AX + (int) std::nearbyint(i * (AW - 1) / 16.0), AT + AH, u(i % 4 == 0 ? "┴" : i % 2 == 0 ? "┬" : "·"), i % 4 == 0 ? grey : dark);
        for (int i = 0; i <= 4; ++i)
        {
            String t = "0";
            if (i > 0 && isFree)
                t = (i / (4.0 * value("freeHz")) < 1 ? String(roundToInt(250 * i / value("freeHz"))) + " ms" : String(i / (4.0 * value("freeHz")), 2) + " s");
            else if (i > 0)
            {
                const int num = i * gulp::rateNum[r], den = 4 * gulp::rateDen[r], gcd = std::gcd(num, den);
                t = String(num / gcd) + (den == gcd ? String() : "/" + String(den / gcd));
            }
            g.centre(AX + (int) std::nearbyint(i * (AW - 1) / 4.0) - 3, 7, AT + AH + 1, t, dark);
        }
        g.put(ph, AT + AH, u("▲"), red);
        // The output scope: the post-trim output over the same cycle, full scale = the strip's height.
        Dots scope;
        const auto level = [](float v) { return (int) std::nearbyint((1 - (jlimit(-1.0f, 1.0f, v) + 1) / 2) * 7); };
        for (size_t k = 0; k < gulp::Feed::bins; ++k)
            for (int t = level(proc.feed.outHi[k].load()); t <= level(proc.feed.outLo[k].load()); ++t)
                scope.push_back({ (int) k, t });
        g.braille(AX, AT + AH + 2, AW, 2, scope, grey);

        // Colour and stereo row; the bottom strip: trigger, rate, shift / thresh / tone.
        g.put(AX, CY, "color", grey), g.words(AX + 6, CY, colourWords, colour), g.words(AX + 57, CY, stereoWords, stereo);
        g.put(L + 1, B - 1, String::repeatedString(u("─"), RX - L - 2), rule);
        g.put(AX, B, "trigger", grey), g.words(AX, wordsY, triggerWords, trigger), g.toggle(AX, secondaryY, "loop", value("loop") > 0.5f, trigger == 0);
        g.put(rateX, B, "rate", grey);
        g.words(syncX, wordsY, { "sync" }, isFree ? -1 : 0), g.words(syncX, secondaryY, { "free" }, isFree ? 0 : -1);
        windowStart = jlimit(r - rateCount + 1, r, windowStart);
        StringArray window;
        for (int k = 0; k < rateCount; ++k)
            window.add(gulp::rateNames[windowStart + k]);
        // Share all spare cells between the three word gaps, with at most one cell of rounding; sync stays fixed.
        const int gaps = syncX - rateX - window.joinIntoString("").length();
        for (int k = 0, used = 0; k < rateCount; used += window[k++].length())
        {
            const int x = rateX + used + gaps * k / rateCount;
            rates[k]->setBounds(cells(x, wordsY, window[k].length(), 2)), rates[k]->setVisible(!isFree), rates[k]->setToggleState(windowStart + k == r, dontSendNotification);
            if (!isFree) g.words(x, wordsY, { window[k] }, windowStart + k == r ? 0 : -1);
        }
        freeHz.setVisible(isFree), down.setVisible(!isFree), up.setVisible(!isFree);
        if (isFree)
            g.centre(rateX, 10, wordsY, text("freeHz"), red, true), g.centre(rateX, 10, wordsY + 1, String::repeatedString(u("▔"), text("freeHz").length()), red);
        else
            g.put(rateX, secondaryY, u("◁"), grey), g.put(rateX + 2, secondaryY, "more", dark), g.put(rateX + 7, secondaryY, u("▷"), grey);
        for (const auto& k : knobSpecs)
            knob(g, k.x, k.y, proc.apvts.getParameter(k.id)->getValue(), k.id, text(k.id), k.centred, std::strcmp(k.id, "thresh") == 0 && trigger != 2);
        g.centre(RX + 4, 14, 10, "band", grey), g.words(RX + 3, 11, bandWords, band);
        // Dotted meters, two steps per cell: the sidechain follower against the threshold (a full-height tick, white
        // when the audio trigger listens; the bar red above it) under the trigger, -60..0 dB; the output peak under its
        // trim, -54..0 dBFS, red at 0. Unlit steps are the dark track.
        const auto bar = [&g](int x, int y, int cells, float db, float floorDb, Colour on) {
            const int halves = roundToInt(jlimit(0.0f, 1.0f, (db - floorDb) / -floorDb) * (float) (2 * cells));
            Dots lit, track;
            for (int k = 0; k < 2 * cells; ++k)
                for (int t : { 1, 2 }) (k < halves ? lit : track).push_back({ k, t });
            g.braille(x, y, cells, 1, track, rule), g.braille(x, y, cells, 1, lit, on, over);
        };
        const float sc = proc.feed.scLevel.load(), thresh = value("thresh"), out = proc.feed.outLevel.load();
        const bool listening = trigger == 2;
        g.put(AX, readoutY, "sc", grey);
        bar(AX + 3, readoutY, 12, Decibels::gainToDecibels(sc, -100.0f), -60, Decibels::gainToDecibels(sc) > thresh && listening ? red : grey);
        const int tick = jlimit(0, 23, roundToInt((thresh + 60) * 23 / 60));
        g.braille(AX + 3, readoutY, 12, 1, { { tick, 0 }, { tick, 1 }, { tick, 2 }, { tick, 3 } }, listening ? white : dark);
        g.put(RX + 3, readoutY, "out", grey);
        bar(RX + 7, readoutY, 11, Decibels::gainToDecibels(out, -100.0f), -54, out >= 1 ? red : white);

        // Footer: rate, sync | free, depth, the summary, gain reduction bar.
        int at = 2;
        const auto word = [&](const String& t, Colour c, bool bold = false) { g.put(at, R - 2, t, c, bold), at += t.length() + 1; };
        word(isFree ? minus(text("freeHz")) : String(gulp::rateNames[r]), white, true), word(isFree ? "free" : "sync", grey);
        word(String(roundToInt(depth * 100)), white, true), word("%", grey);
        const double cycle = isFree ? 1 / value("freeHz") : 240.0 * gulp::rateNum[r] / gulp::rateDen[r] / bpm;
        const double shiftMs = 10 * cycle * (trigger == 0 ? value("shift") : jmax(0.0f, value("shift"))); // a trigger delay is never negative
        const String dot = u(" · ");
        g.put(at + 1, R - 2, (curveWords[curve] + dot + colourWords[colour] + dot + stereoWords[stereo] + dot
                                 + (band == 0 ? String("full") : bandWords[band] + (band == 1 ? " < " : " > ") + text("split")) + dot
                                 + String(roundToInt(bpm)) + " bpm" + dot + minus(String(roundToInt(shiftMs))) + " ms").substring(0, RX - at - 2), dark);
        const int halves = roundToInt(jlimit(0.0f, 1.0f, -Decibels::gainToDecibels(1.0f - duck, -100.0f) / 40) * 18);
        g.put(RX + 3, R - 2, "gr", grey);
        for (int k = 0; k < 9; ++k)
            g.put(RX + 6 + k, R - 2, u(halves >= 2 * k + 2 ? "█" : halves == 2 * k + 1 ? "▌" : "·"), halves > 2 * k ? red : dark);
        show(g);
    }

    // Cells that differ from the shown grid are redrawn into the image; runs of them on a row are repainted.
    void show(const Grid& g)
    {
        std::optional<Graphics> context;
        if (image.isValid()) context.emplace(image);
        for (int j = 0; j < R; ++j)
            for (int i = 0, first = -1, last = -1; i <= C; ++i)
            {
                if (i < C && g.at[j][i] != shown.at[j][i])
                {
                    shown.at[j][i] = g.at[j][i], first = first < 0 ? i : first, last = i;
                    if (context) drawCell(*context, i, j);
                }
                else if (first >= 0 && (i == C || i - last > 8))
                {
                    const float sx = (float) getWidth() / C, sy = (float) getHeight() / R;
                    repaint(Rectangle<float>(sx * (float) first, sy * (float) j, sx * (float) (last + 1 - first), sy).getSmallestIntegerContainer());
                    first = -1;
                }
            }
    }
    void paint(Graphics& g) override
    {
        const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
        const int iw = jmax(1, roundToInt((float) getWidth() * scale)), ih = jmax(1, roundToInt((float) getHeight() * scale));
        if (image.getWidth() != iw || image.getHeight() != ih)
        {
            image = Image(Image::RGB, iw, ih, false);
            fonts[0] = mono((float) iw / C / 0.6f, false), fonts[1] = mono((float) iw / C / 0.6f, true);
            Graphics ig(image);
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < C; ++i)
                    drawCell(ig, i, j);
        }
        g.drawImageTransformed(image, AffineTransform::scale((float) getWidth() / (float) iw, (float) getHeight() / (float) ih));
    }
    // One cell in device pixels, snapped to whole pixels so lines and blocks join; vector glyphs as grid.py draws them.
    void drawCell(Graphics& g, int i, int j)
    {
        const auto& c = shown.at[j][i];
        const auto px = [this](int k) { return roundToInt((float) k * (float) image.getWidth() / C); };
        const auto py = [this](int k) { return roundToInt((float) k * (float) image.getHeight() / R); };
        const auto r = Rectangle<int>::leftTopRightBottom(px(i), py(j), px(i + 1), py(j + 1));
        g.setColour(c.bg.isTransparent() ? page : c.bg), g.fillRect(r);
        if (c.c == ' ' && c.left <= ' ') return;
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r), g.setColour(c.fg); // nothing spills into a neighbour, which may be redrawn alone
        const float w = (float) image.getWidth() / C, h = (float) image.getHeight() / R, x = (float) r.getX(), y = (float) r.getY();
        const float cx = x + (float) r.getWidth() / 2, cy = y + (float) r.getHeight() / 2, lw = jmax(1.0f, w * 0.12f);
        const int t = jmax(1, roundToInt(lw)), hx = roundToInt(cx - (float) t / 2), hy = roundToInt(cy - (float) t / 2);
        const auto block = [&](float l, float tp, float rt, float bt) {
            g.fillRect(Rectangle<int>::leftTopRightBottom(roundToInt(x + l * (float) r.getWidth()), roundToInt(y + tp * (float) r.getHeight()),
                                                          roundToInt(x + rt * (float) r.getWidth()), roundToInt(y + bt * (float) r.getHeight())));
        };
        const auto glyph = [&](juce_wchar code, float offset) {
            const float gx = (c.left != 0 ? (float) i * w : x + ((float) r.getWidth() - w) / 2) + offset;
            if (code == 0x2594) g.fillRect(Rectangle<float>(gx, y, w, h * 0.125f));
            else if (const auto id = fonts[c.bold ? 1 : 0].getTypefacePtr()->getNominalGlyphForCodepoint(code))
            {
                const float size = w / 0.6f; // Geist Mono: advance 0.6 em, ascent 1.005 + descent 0.295 em
                const uint16_t index = (uint16_t) *id;
                const Point<float> at { gx, y + (h - 1.3f * size) / 2 + 1.005f * size };
                g.setFont(fonts[c.bold ? 1 : 0]);
                g.getInternalContext().drawGlyphs({ &index, 1 }, { &at, 1 }, {});
            }
        };
        if (c.left != 0) { glyph(c.left, -w / 2), glyph(c.c, w / 2); return; }
        const int arms = c.c == 0x2500 ? 10 : c.c == 0x2502 ? 5 : c.c == 0x2534 ? 11 : c.c == 0x252c ? 14 : 0; // ─ │ ┴ ┬: up 1 right 2 down 4 left 8
        if (arms != 0)
        {
            if (arms & 8) g.fillRect(r.getX(), hy, hx + t - r.getX(), t);
            if (arms & 2) g.fillRect(hx, hy, r.getRight() - hx, t);
            if (arms & 1) g.fillRect(hx, r.getY(), t, hy + t - r.getY());
            if (arms & 4) g.fillRect(hx, hy, t, r.getBottom() - hy);
        }
        else if (c.c == 0x250a) // ┊
            for (int k = 0; k < 4; ++k) g.fillRect(Rectangle<float>((float) hx, y + h * ((float) k + 0.2f) / 4, (float) t, h * 0.15f));
        else if (c.c == 0x2591) // ░: a CRT dither of 4 dots
            for (auto [di, dj] : { std::pair { 0, 0 }, { 0, 2 }, { 1, 1 }, { 1, 3 } })
                g.fillRect(Rectangle<float>(0.22f * w, 0.22f * w).withCentre({ x + w * ((float) di * 0.5f + 0.25f), y + h * ((float) dj * 0.25f + 0.125f) }));
        else if (c.c >= 0x2581 && c.c <= 0x2588) block(0, 1 - (float) (c.c - 0x2580) / 8, 1, 1); // ▁ .. █
        else if (c.c == 0x258c) block(0, 0, 0.5f, 1); // ▌
        else if (c.c == 0x2594) block(0, 0, 1, 0.125f); // ▔
        else if (c.c >= 0x2800 && c.c <= 0x28ff) // braille
        {
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                    if ((c.c - 0x2800) & brailleBit(dx, dy))
                        g.fillEllipse(Rectangle<float>(0.3f * w, 0.3f * w).withCentre({ x + (float) r.getWidth() * (0.3f + 0.4f * (float) dx), y + (float) r.getHeight() * (0.125f + 0.25f * (float) dy) }));
        }
        else if (c.c == 0x25a0) g.fillRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy })); // ■
        else if (c.c == 0x25a1) g.drawRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy }), lw); // □
        else if (c.c == 0x25b2 || c.c == 0x25b6 || c.c == 0x25b7 || c.c == 0x25c0 || c.c == 0x25c1 || c.c == 0x25c6) // ▲ ▶ ▷ ◀ ◁ ◆
        {
            const float rr = 0.36f * w, s = 0.95f * rr, d = c.c == 0x25c0 || c.c == 0x25c1 ? -1.0f : 1.0f;
            Path p;
            if (c.c == 0x25c6) p.addQuadrilateral(cx, cy - 1.1f * rr, cx + rr, cy, cx, cy + 1.1f * rr, cx - rr, cy);
            else if (c.c == 0x25b2) p.addTriangle(cx, cy - s, cx + s, cy + 0.8f * s, cx - s, cy + 0.8f * s);
            else p.addTriangle(cx + d * s, cy, cx - d * 0.8f * s, cy - s, cx - d * 0.8f * s, cy + s);
            if (c.c != 0x25b7 && c.c != 0x25c1) g.fillPath(p);
            g.strokePath(p, PathStrokeType(c.c == 0x25c6 ? lw : 0.8f * lw, PathStrokeType::curved, PathStrokeType::rounded));
        }
        else if (c.c == 0x2039 || c.c == 0x203a) // ‹ › (not in Geist Mono)
        {
            const float d = c.c == 0x2039 ? 1.0f : -1.0f;
            Path p;
            p.startNewSubPath(cx + d * 0.14f * w, cy - 0.14f * h), p.lineTo(cx - d * 0.14f * w, cy + 0.02f * h), p.lineTo(cx + d * 0.14f * w, cy + 0.18f * h);
            g.strokePath(p, PathStrokeType(lw));
        }
        else glyph(c.c, 0);
    }
};
} // namespace

AudioProcessorEditor* GulpProcessor::createEditor() { return new Editor(*this); }
