// gulp editor: ASCII design v2, "tide" (sidechain-design/ascii/v2/redesign2.*, made by src/redesign2.py), with the mode
// words spread evenly across the graph.
// One 120 x 34 cell grid scaled with the window (50-200 %, fixed aspect), as in crumb. Text is Geist Mono in its cells;
// braille, box, block, shade and geometric glyphs are vectors per cell, so they join. At 30 Hz, when anything it shows
// changed, the grid is rebuilt; only cells that changed are redrawn into a device-pixel image and repainted, so an idle
// editor costs nothing. Controls are invisible Sliders and Buttons on their hit cells in a 1024 x 576 layer scaled with
// the window: drag, double-click = default and click the value to type, exactly as crumb. The slope's ◆ drags sideways.
// The cells are drawn in grey, white and red as before; the riso "pump" skin (ascii3/family.py, gulp(ascii=True)) then
// re-inks every pixel they drew from plates built once per image size, so nothing moves.
#include "BinaryData.h"
#include "Gulp.h"
#include <cstring>
#include <numeric>

using namespace juce;

namespace
{
constexpr int C = 120, R = 34, W = 1024, H = 576, L = 22, AX = 25, AW = 70, AT = 5, AH = 14, RX = 98, CY = 22, B = 25;
constexpr int TY = AT - 2, stepX = AX + AW - 13, syncX = stepX + 3, SX = 44, wordsY = B + 2, secondaryY = B + 4, readoutY = B + 5;
constexpr int invertX = 56, midiX = AX + 5, audioX = AX + 10; // invert centred over the graph; the sc meter from "midi" to "audio"
constexpr int curveY[] { 5, 7, 9, 12, 14, 16, 19, 21, 23, 26, 28, 30 }; // groups of three fill the column
constexpr float cw = (float) W / C, ch = (float) H / R;
const Colour page { 0xff0d0d0d }, white { 0xffececec }, grey { 0xff7a7a7a }, rule { 0xff2c2c2c }, dark { 0xff454545 }, red { 0xffff3d1f }, kick { 0xff5a5a5a }, tag { 0xff1c1c1c };
const Colour cobalt { 0xff5b7cff }, lilac { 0xffa98bff }, sun { 0xffffc83d }, tang { 0xffff6a2f }, pink { 0xffff4f9a }, cream { 0xffefe6d2 }; // riso inks
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
const StringArray triggerWords { "host", "midi", "audio" }, stereoWords { "both", "mid", "side" };
const StringArray curveWords(gulp::curveNames, gulp::numCurves), colourWords(gulp::colorNames, gulp::numColors);
const int modeGap = (AW - colourWords.joinIntoString("").length()) / (gulp::numColors - 1); // first to last: the graph

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
    void words(int x, int y, const StringArray& items, int sel, int gap = 1) // the selected word red and underlined
    {
        for (int k = 0; k < items.size(); x += items[k].length() + gap, ++k)
        {
            put(x, y, items[k], k == sel ? red : grey, k == sel);
            if (k == sel) put(x, y + 1, String::repeatedString(u("▔"), items[k].length()), red);
        }
    }
    void toggle(int x, int y, const String& label, bool on, bool dim = false)
    {
        put(x, y, u(on ? "■" : "□"), dim ? dark : on ? red : grey), put(x + 2, y, label, dim ? dark : on ? white : grey, on && !dim);
    }
    // A dotted meter, two steps per cell, dot rows from..to: the lit fraction in `on` over the track.
    void meter(int x, int y, int w, int h, int from, int to, float fraction, Colour on)
    {
        const int lit = roundToInt(jlimit(0.0f, 1.0f, fraction) * (float) (2 * w));
        Dots dots[2];
        for (int k = 0; k < 2 * w; ++k)
            for (int t = from; t < to; ++t) dots[k < lit ? 1 : 0].push_back({ k, t });
        braille(x, y, w, h, dots[0], rule), braille(x, y, w, h, dots[1], on, over);
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
        l->setColour(Label::backgroundWhenEditingColourId, page), l->setColour(Label::textWhenEditingColourId, cream);
        l->setColour(Label::outlineWhenEditingColourId, {}), l->setColour(TextEditor::highlightColourId, pink.withAlpha(0.4f));
        l->setColour(TextEditor::highlightedTextColourId, cream), l->setColour(CaretComponent::caretColourId, pink);
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
// The free-rate value is also its drag surface; the label lets pointer events through until it is being edited.
struct RateValue : Slider
{
    void lookAndFeelChanged() override
    {
        Slider::lookAndFeelChanged();
        for (auto* child : getChildren())
            if (auto* label = dynamic_cast<Label*>(child)) label->setInterceptsMouseClicks(false, true);
    }
    void mouseUp(const MouseEvent& e) override
    {
        Slider::mouseUp(e);
        if (e.mouseWasClicked() && e.getNumberOfClicks() == 1 && !e.mods.isPopupMenu()) showTextBox();
    }
};
struct Key : Button
{
    explicit Key(const String& id) : Button(id) { setComponentID(id), setRepaintsOnMouseActivity(false); }
    void paintButton(Graphics&, bool, bool) override {}
};

struct KnobSpec { const char *id, *name; int x, y; bool centred; Colour glow; };
const KnobSpec knobSpecs[] = { { "shift", "nudge", 61, B + 1, true, pink }, { "thresh", "thresh", 74, B + 1, false, lilac }, { "soothe", "soothe", 87, B + 1, false, sun },
                               { "depth", "depth", RX + 7, 4, false, pink }, { "split", "split", RX + 7, 13, false, tang }, { "output", "output", RX + 7, 22, false, sun } };

// Riso plates, one byte per image pixel: the accent's ink (bits 0-1), the plot's shadow ink (bit 2), in the plot (bit 3)
// and the paper (bits 4-7): the page, its grain, the braille field's two inks, a dim glow per knob.
const Colour accents[] { pink, tang, sun }, shadows[] { cobalt.interpolatedWith(cream, 0.2f), lilac.interpolatedWith(cream, 0.2f) };
const auto papers = [] {
    std::array<Colour, 4 + std::size(knobSpecs)> p { page, Colour(0xff121212), page.interpolatedWith(lilac, 0.55f), page.interpolatedWith(cobalt, 0.55f) };
    for (size_t k = 0; k < std::size(knobSpecs); ++k) p[4 + k] = page.interpolatedWith(knobSpecs[k].glow, 0.12f);
    return p;
}();
// family.py's recolor: the red accent takes its plate's ink, bright neutrals cream, the plot's neutrals their cool shadow
// (a fifth cream), the bare page its paper. Anything else (antialiased edges of red) stays.
Colour reink(Colour c, uint8 plate)
{
    const int r = c.getRed(), g = c.getGreen(), b = c.getBlue();
    const float lum = (float) (r + g + b) / 3, t = jlimit(0.0f, 1.0f, (lum - 13) / 223);
    if (std::abs(r - 13) + std::abs(g - 13) + std::abs(b - 13) < 4) return papers[plate >> 4];
    if (r - g > 40 && r - b > 30) return page.interpolatedWith(accents[plate & 3], (float) (r - 13) / 242);
    if (jmax(r, g, b) - jmin(r, g, b) >= 12) return c;
    if ((plate & 8) != 0 && lum > 20 && lum < 200) return page.interpolatedWith(shadows[plate >> 2 & 1], t);
    return lum > 150 ? page.interpolatedWith(cream, t) : c;
}
// family.py's fbm: octaves of a seeded random lattice (3 * 2^o rows) smoothly stretched over h x w, normalised to 0..1.
std::vector<float> fbm(int h, int w, int octaves, int64 seed)
{
    std::vector<float> out((size_t) (h * w));
    Random random(seed);
    for (int o = 0; o < octaves; ++o)
    {
        const int rows = (3 << o) + 1, cols = (3 << o) * w / h + 2;
        std::vector<float> lattice((size_t) (rows * cols));
        for (auto& v : lattice) v = random.nextFloat();
        const auto at = [&](int j, int i) { return lattice[(size_t) (j * cols + i)]; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const float fy = ((float) y + 0.5f) * (float) rows / (float) h - 0.5f, fx = ((float) x + 0.5f) * (float) cols / (float) w - 0.5f;
                const int j = jlimit(0, rows - 2, (int) std::floor(fy)), i = jlimit(0, cols - 2, (int) std::floor(fx));
                const auto smooth = [](float t) { t = jlimit(0.0f, 1.0f, t); return t * t * (3 - 2 * t); };
                const float ty = smooth(fy - (float) j), tx = smooth(fx - (float) i);
                out[(size_t) (y * w + x)] += jmap(ty, jmap(tx, at(j, i), at(j, i + 1)), jmap(tx, at(j + 1, i), at(j + 1, i + 1))) / (float) (1 << o);
            }
    }
    const auto [lo, hi] = std::minmax_element(out.begin(), out.end());
    const float low = *lo, range = *hi - *lo + 1e-9f;
    for (auto& v : out) v = (v - low) / range;
    return out;
}

// The slope: only the ◆ and its value label are targets. A sideways drag moves the handle with the cursor, in
// design pixels from where it was grabbed, so the handle's own moves never feed back into the drag.
struct Handle : Slider
{
    Rectangle<int> grip;
    std::function<void(double dx)> drag;
    bool hitTest(int x, int y) override
    {
        return grip.contains(x, y) || Rectangle<int>::fromString(getProperties()["box"].toString()).contains(x, y);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        const auto m = e.getEventRelativeTo(getParentComponent());
        if (e.mouseWasDraggedSinceMouseDown()) drag(m.position.x - m.mouseDownPosition.x);
    }
};

struct Editor : AudioProcessorEditor, Timer
{
    GulpProcessor& proc;
    Look look;
    Component layer; // the controls, in design pixels
    Slider knobs[6];
    RateValue freeHz;
    Handle slope;
    Key prev { "prev" }, next { "next" }, save { "save" }, slotA { "A" }, slotB { "B" }, invert { "invert" }, loop { "loop" }, ahead { "lookahead" }, free { "free" }, sync { "sync" }, down { "rate-" }, up { "rate+" }, peak { "peak" };
    OwnedArray<Key> curves, colours, stereos, triggers;
    // Attachments after the components they drive, so they are destroyed first.
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment>> sliderAtt;
    std::vector<std::unique_ptr<ParameterAttachment>> keyAtt;
    double grabbed = 0; // the handle's place in the cycle when the drag began
    Grid shown;
    std::vector<float> drawnFrom; // what the shown grid was built from: the same again means idle
    String drawnName;
    Image image; // the shown grid in device pixels, re-inked
    std::vector<uint8> plates; // per pixel of the image: see reink()
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
        setup(slope, "slope", {}), setup(freeHz, "freeHz", {}); // placed by refresh(): their value box is all of them
        slope.onDragStart = [this] { grabbed = 0.3 * std::exp2(slope.getValue() / 100); };
        slope.drag = [this](double dx) { slope.setValue(100 * std::log2(jmax(0.01, grabbed + dx / (AW * cw)) / 0.3), sendNotificationSync); };

        prev.onClick = [this] { proc.stepPreset(-1); };
        next.onClick = [this] { proc.stepPreset(1); };
        save.onClick = [this] { proc.saveUserPreset(); };
        slotA.onClick = [this] { proc.selectAb(0); };
        slotB.onClick = [this] { proc.selectAb(1); };
        peak.onClick = [this] { proc.feed.held = 0; };
        for (auto [key, x, w] : { std::tuple { &prev, 30, 2 }, { &next, 62, 2 }, { &save, 67, 4 }, { &slotA, 76, 1 }, { &slotB, 78, 1 } })
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
        const auto row = [&](OwnedArray<Key>& keys, const char* id, const StringArray& items, int x, int y, int gap = 1) {
            std::vector<Key*> list;
            for (int k = 0; k < items.size(); ++k)
            {
                auto* key = keys.add(new Key(String(id) + " " + items[k]));
                key->setBounds(gap == 0 ? cells(2, curveY[k], 19, (k + 1 < items.size() ? curveY[k + 1] : R - 3) - curveY[k]) : cells(x, y, items[k].length(), 2));
                layer.addAndMakeVisible(key), list.push_back(key), x += items[k].length() + gap;
            }
            bind(id, list);
        };
        row(curves, "curve", curveWords, 0, 0, 0);
        row(colours, "color", colourWords, AX, CY, modeGap), row(stereos, "stereo", stereoWords, SX, wordsY);
        row(triggers, "trigger", triggerWords, AX, wordsY);
        bind("invert", { &invert }), bind("loop", { &loop }), bind("lookahead", { &ahead });
        bind("tempoSync", { &free, &sync }); // explicit choices: free = 0, sync = 1, including repeated clicks
        // ‹ › step the rate up to 2 bars (an old 4 bars still steps down), or in free a ladder of round frequencies.
        auto* rate = bind("rate", {});
        auto* hz = bind("freeHz", {});
        const auto step = [this, rate, hz](int by) {
            if (value("tempoSync") > 0.5f)
                return rate->setValueAsCompleteGesture((float) jlimit(0, jmax(gulp::maxRate, index("rate")), index("rate") + by));
            const float now = value("freeHz"), ladder[] { 0.1f, 0.2f, 0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20 };
            float to = now; // the nearest rung past the value
            for (size_t k = 0; k < std::size(ladder); ++k)
                if (const float f = ladder[by > 0 ? std::size(ladder) - 1 - k : k]; by > 0 ? f > now * 1.001f : f < now * 0.999f)
                    to = f;
            hz->setValueAsCompleteGesture(to);
        };
        down.onClick = [step] { step(-1); };
        up.onClick = [step] { step(1); };
        for (auto [key, r] : { std::pair { &invert, cells(invertX, TY, 8, 1) }, { &loop, cells(AX, secondaryY, 6, 1) }, { &ahead, cells(audioX, secondaryY, 11, 1) }, { &peak, cells(AX, TY, 20, 1) },
                               { &sync, cells(syncX, TY, 4, 2) }, { &free, cells(syncX + 6, TY, 4, 2) }, { &up, cells(stepX, TY, 1, 1) } })
            layer.addAndMakeVisible(*key), key->setBounds(r);
        layer.addAndMakeVisible(down); // placed by refresh(): it moves out for a long value

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
    String text(const char* id) const { return minus(proc.apvts.getParameter(id)->getCurrentValueAsText()); }
    void resized() override
    {
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }
    // A slider whose value box is the given cells within its area; moved only when either changes.
    static void place(Slider& s, Rectangle<int> area, Rectangle<int> box)
    {
        if (const auto b = (box - area.getPosition()).toString(); s.getBounds() != area || s.getProperties()["box"].toString() != b)
            s.getProperties().set("box", b), s.setBounds(area), s.resized();
    }

    void timerCallback() override
    {
        std::vector<float> now;
        for (auto* param : proc.getParameters())
            now.push_back(param->getValue());
        const auto& f = proc.feed;
        now.insert(now.end(), { f.phase.load(), f.duck.load(), f.held.load(), (float) f.live.load(), f.scLevel.load(), f.outLevel.load(),
                                f.bpm.load(), (float) proc.preset.load(), (float) proc.abSlot() });
        for (size_t k = 0; k < gulp::Feed::bins; ++k)
            now.insert(now.end(), { f.lo[k].load(), f.hi[k].load() });
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
        const auto& feed = proc.feed;
        const int curve = index("curve"), trigger = index("trigger"), colour = index("color"), stereo = index("stereo"), r = index("rate");
        const bool isFree = value("tempoSync") < 0.5f, inverted = value("invert") > 0.5f, live = feed.live.load();
        const double s = std::exp2(value("slope") / 100.0), depth = value("depth") / 100.0, soothe = value("soothe") / 100.0;
        // The graph is the time since the cycle began (the beat in sync): the nudge slides the curve along it in sync,
        // and a trigger's softened attack starts where the hit is.
        const double offset = trigger == 0 ? value("shift") / 100.0 : 0;
        gulp::Table table;
        gulp::shape(table, curve, value("slope"), inverted, soothe, trigger != 0);
        const auto gain = [&](double p) {
            p -= offset + std::floor(p - offset);
            const double x = p * 1024;
            const int j = jmin(1023, (int) x);
            return 1 - depth * (table[(size_t) j] + (x - j) * (table[(size_t) j + 1] - table[(size_t) j]));
        };
        // Header: name, preset number and name with prev / next, save, A / B, maker.
        g.put(2, 1, "gulp", white, true);
        g.put(30, 1, u("‹"), grey), g.put(63, 1, u("›"), grey), g.put(67, 1, "save", grey); // ‹ ›: also the rate's
        g.put(33, 1, proc.preset >= 0 ? String(proc.preset.load()).paddedLeft('0', 2) : String("--"), grey);
        g.centre(36, 26, 1, proc.presetName(), white, true);
        for (int k = 0; k < 2; ++k)
            g.put(76 + 2 * k, 1, k == 0 ? "A" : "B", proc.abSlot() == k ? page : grey, proc.abSlot() == k, proc.abSlot() == k ? white : Colour());
        g.right(C - 3, 1, "daveydidit", dark);
        g.put(2, 2, String::repeatedString(u("─"), C - 4), rule), g.put(2, R - 2, String::repeatedString(u("─"), C - 4), rule);
        for (int j = 3; j < R - 2; ++j)
            g.put(L, j, u("│"), rule), g.put(RX - 1, j, u("│"), rule);

        // Curve list: number, thumbnail, name, ◀ on the selected one.
        g.put(2, 3, "curve", grey);
        for (int i = 0; i < gulp::numCurves; ++i)
        {
            const int y = curveY[i];
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

        // Above the graph: gain reduction now (24 dB, dotted) and its held peak in whole dB; invert; the rate stepper
        // (or the free rate) and sync / free.
        const float duck = live ? feed.duck.load() : 0.0f, held = feed.held.load();
        g.put(AX, TY, "gr", grey), g.meter(AX + 3, TY, 10, 1, 1, 3, -Decibels::gainToDecibels(1.0f - duck, -100.0f) / 24, red);
        const float heldDb = Decibels::gainToDecibels(1.0f - held, -100.0f);
        g.put(AX + 14, TY, heldDb <= -100 ? u("−inf") : minus(String(roundToInt(heldDb))), heldDb < -0.5f ? red : grey, heldDb < -0.5f);
        g.toggle(invertX, TY, "invert", inverted);
        const String rateText = isFree ? text("freeHz") : gulp::rateDen[r] > 1 ? String(gulp::rateNames[r]) : String(gulp::rateNum[r]) + (r > 13 ? " bars" : " bar");
        const int field = jmax(7, rateText.length() + 4), downX = stepX - field - 1;
        g.put(downX, TY, u("‹"), isFree || r > 0 ? grey : dark), g.centre(downX + 1, field, TY, rateText, white, true);
        g.put(stepX, TY, u("›"), isFree || r < gulp::maxRate ? grey : dark);
        g.words(syncX, TY, { "sync", "free" }, isFree ? 1 : 0, 2);
        down.setBounds(cells(downX, TY, 1, 1));
        place(freeHz, cells(downX + 1, TY, field, 1), cells(downX + 1, TY, field, 1));
        freeHz.setVisible(isFree);

        // The tide: one cycle of gain, the water (audio that passes) under it, the sidechain's kick as a grey ghost.
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
        float loudest = 0;
        for (int k = 0; k < gulp::Feed::bins; ++k)
            loudest = jmax(loudest, std::abs(feed.lo[(size_t) k].load()), std::abs(feed.hi[(size_t) k].load()));
        const auto row = [loudest](float v) { return (int) std::nearbyint((1 - (jlimit(-1.0f, 1.0f, v / jmax(0.05f, loudest) * 0.6f) + 1) / 2) * (4 * AH - 1)); };
        for (int k = 0; k < gulp::Feed::bins; ++k)
            if (const float lo = feed.lo[(size_t) k].load(), hi = feed.hi[(size_t) k].load(); jmax(-lo, hi) > 0.03f * loudest)
                for (int t = row(hi); t <= row(lo); ++t)
                    ghost.push_back({ k, t });
        g.braille(AX, AT, AW, AH, line, red), g.braille(AX, AT, AW, AH, ghost, kick, under);
        // Playhead (only while the transport runs; under the slope's handle and label).
        const int ph = AX + jmin(AW - 1, (int) (AW * feed.phase.load()));
        for (int j = AT; j < AT + AH && live; ++j)
            if (g.at[j][ph].c == ' ' || g.at[j][ph].c == 0x2591) g.put(ph, j, u("┊"), red);
        // The slope handle rides the curve where the release is (it moves with the slope and the nudge). Its label, the
        // value to type, sits on a dark tag diagonally off the ◆ on the side the curve leaves open.
        const double position = 0.3 * s + offset + (trigger == 0 ? 0 : 3 * gulp::blur(soothe) / 1024.0);
        const double hp = position - std::floor(position);
        const int hx = AX + (int) (AW * hp), hy = jlimit(AT, AT + AH - 2, AT + (int) std::nearbyint((1 - gain(hp)) * AH));
        const String label = "slope " + text("slope");
        const bool right = hx + 3 + label.length() <= AX + AW, below = (gain(hp + 0.02) >= gain(hp)) == right || hy == AT;
        const int lx = right ? hx + 2 : hx - 1 - label.length(), ly = below ? hy + 1 : hy - 1;
        g.put(hx, hy, u("◆"), white), g.put(lx - 1, ly, " " + label + " ", white, true, tag);
        place(slope, cells(hx - 1, hy - 1, 3, 3).getUnion(cells(lx, ly, label.length(), 1)), cells(lx, ly, label.length(), 1));
        slope.grip = cells(hx - 1, hy, 3, 1).expanded(2) - slope.getPosition();
        // The ruler, in fractions of the rate (or time when free): a baseline, a tall tick on each labelled beat.
        g.put(AX, AT + AH, String::repeatedString(u("─"), AW), rule);
        for (int i = 0; i <= 16; i += 2)
            g.put(AX + (int) std::nearbyint(i * (AW - 1) / 16.0), AT + AH, u(i % 4 == 0 ? "┸" : "┴"), i % 4 == 0 ? grey : dark);
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
            if (i == 4) g.right(AX + AW, AT + AH + 1, t, dark); // long free-rate labels must clear the divider
            else g.centre(AX + (int) std::nearbyint(i * (AW - 1) / 4.0) - 3, 7, AT + AH + 1, t, dark);
        }
        if (live) g.put(ph, AT + AH, u("▲"), red);

        // The modes across the graph's width; the bottom strip: trigger, loop (dim in host sync, which never reads it),
        // lookahead under "audio" (dim unless the audio trigger, the only one it acts on), stereo, nudge / thresh / soothe.
        g.words(AX, CY, colourWords, colour, modeGap);
        g.put(L + 1, B - 1, String::repeatedString(u("─"), RX - L - 2), rule);
        g.put(AX, B, "trigger", grey), g.words(AX, wordsY, triggerWords, trigger), g.toggle(AX, secondaryY, "loop", value("loop") > 0.5f, trigger == 0);
        g.toggle(audioX, secondaryY, "lookahead", value("lookahead") > 0.5f, trigger != 2);
        g.put(SX, B, "stereo", grey), g.words(SX, wordsY, stereoWords, stereo);
        for (const auto& k : knobSpecs)
            knob(g, k.x, k.y, proc.apvts.getParameter(k.id)->getValue(), k.name, text(k.id), k.centred,
                 (std::strcmp(k.id, "thresh") == 0 && trigger != 2) || (std::strcmp(k.id, "split") == 0 && colour != 1 && colour != 2));
        // Dotted meters: the sidechain follower against the threshold (a full-height tick, white when the audio trigger
        // listens; the bar red above it) under midi .. audio, -60..0 dB; the output peak under its knob, 2 rows of a
        // 4-dot bar, -54..0 dBFS, red at 0.
        const float sc = feed.scLevel.load(), thresh = value("thresh"), out = feed.outLevel.load();
        const bool listening = trigger == 2;
        g.put(AX, readoutY, "sc", grey);
        g.meter(midiX, readoutY, 10, 1, 1, 3, (Decibels::gainToDecibels(sc, -100.0f) + 60) / 60, Decibels::gainToDecibels(sc) > thresh && listening ? red : grey);
        const int tick = jlimit(0, 19, roundToInt((thresh + 60) * 19 / 60));
        g.braille(midiX, readoutY, 10, 1, { { tick, 0 }, { tick, 1 }, { tick, 2 }, { tick, 3 } }, listening ? white : dark);
        g.meter(RX + 3, B + 4, 16, 2, 2, 6, (Decibels::gainToDecibels(out, -100.0f) + 54) / 54, out >= 1 ? red : white);
        // The nudge's ms follow the tempo and rate: keep its (hidden) value box current unless it is being typed in.
        if (auto* box = dynamic_cast<Label*>(knobs[0].getChildComponent(0)); box != nullptr && !box->isBeingEdited())
            knobs[0].updateText();
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
                    if (context) drawCell(*context, i, j), reinkArea(pixelsOf(i, j));
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
            image = Image(Image::RGB, iw, ih, false, SoftwareImageType()); // software: re-inked in place on every platform
            fonts[0] = mono((float) iw / C / 0.6f, false), fonts[1] = mono((float) iw / C / 0.6f, true);
            makePlates();
            Graphics ig(image);
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < C; ++i)
                    drawCell(ig, i, j);
            reinkArea(image.getBounds());
        }
        g.drawImageTransformed(image, AffineTransform::scale((float) getWidth() / (float) iw, (float) getHeight() / (float) ih));
    }
    // family.py's gulp() plates for this image size, in its pixels (the editor at 2x, 2048 x 1152): accents pink, the
    // plot's curve heating pink -> tang -> sun across the beat, the band row tang, the bottom row tang -> pink -> sun, the
    // right column pink / tang / sun; the plot's shadow lilac (top) -> cobalt; the paper's braille field (8 x 16 cells, 2 x 4
    // dots by a slow noise) behind the plot and a halftone glow behind each knob. Inks between neighbours are Bayer-dithered.
    void makePlates()
    {
        static const auto noise = fbm(144, 256, 5, 21), field = fbm(17, 78, 4, 918); // 918: the seed closest to the preview's field
        constexpr int bayer[4][4] { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
        constexpr int dots[8][2] { { 5, 4 }, { 11, 4 }, { 5, 12 }, { 11, 12 }, { 5, 20 }, { 11, 20 }, { 5, 28 }, { 11, 28 } }, order[8] { 0, 4, 6, 2, 5, 1, 3, 7 }, bottom[3] { 1, 0, 2 };
        const auto ramp = [](float u, int stops, int at) { // an ink index; at = the Bayer rank, 0..15
            const float v = jlimit(0.0f, 1.0f, u) * (float) (stops - 1);
            const int k = jmin((int) v, stops - 1);
            return jmin(stops - 1, k + (jlimit(0.0f, 1.0f, (v - (float) k - 0.3f) / 0.4f) > (float) at / 16 ? 1 : 0));
        };
        Point<int> glows[std::size(knobSpecs)];
        for (size_t k = 0; k < std::size(knobSpecs); ++k) glows[k] = cells(knobSpecs[k].x, knobSpecs[k].y - 1, 8, 5).getCentre() * 2; // dial and name
        const int iw = image.getWidth(), ih = image.getHeight();
        plates.resize((size_t) (iw * ih));
        for (int y = 0; y < ih; ++y)
            for (int x = 0; x < iw; ++x)
            {
                const float X = (float) x * 2048 / (float) iw, Y = (float) y * 1152 / (float) ih;
                const int bx = (int) X, by = (int) Y, rank = bayer[by / 2 % 4][bx / 2 % 4];
                const float n = noise[(size_t) (by * 144 / 1152 * 256 + bx * 256 / 2048)] - 0.5f;
                const bool plot = X > 410 && X < 1650 && Y > 150 && Y < 680;
                int accent = 0, paper = 0;
                if (plot) accent = ramp((X - 420) / 1220 + n * 0.1f, 3, rank);
                else if (Y >= 700 && Y < 800 && X > 410 && X < 1650) accent = 1;
                else if (Y >= 820 && X > 380 && X < 1650) accent = bottom[ramp((X - 400) / 1250, 3, rank)];
                else if (X >= 1680) accent = Y < 380 ? 0 : Y < 680 ? 1 : 2;
                if (const float fx = X - 424, fy = Y - 170; fx >= 0 && fy >= 0 && fx < 1216 && fy < 494)
                {
                    const int c = (int) fx / 16, r = (int) fy / 32;
                    const float v = jlimit(0.0f, 1.0f, (field[(size_t) (r * 78 + c)] * 0.85f + 0.15f * (float) (r * 32) / 494 - 0.35f) * 1.6f);
                    for (int k = 0; k < 8; ++k)
                        if (const int dx = c * 16 + dots[k][0], dy = r * 32 + dots[k][1]; v * 8 > (float) order[k] + 0.5f && dx + 2 < 1216 && dy + 2 < 494
                                                                                          && std::abs(fx - (float) dx) <= 1 && std::abs(fy - (float) dy) <= 1)
                            paper = 2 + ramp((float) dy / 493, 2, bayer[dy / 2 % 4][dx / 2 % 4]);
                }
                for (size_t k = 0; k < std::size(knobSpecs); ++k)
                    if (const float gx = X - (float) glows[k].x + 85, gy = Y - (float) glows[k].y + 85; gx >= 0 && gy >= 0 && gx < 170 && gy < 170)
                    {
                        const float level = std::pow(jmax(0.0f, 1 - std::hypot(gx - 85, gy - 85) / 85), 1.6f), cs = std::cos(0.5236f), sn = std::sin(0.5236f);
                        const auto wrap = [](float t) { return t - 6 * std::floor(t / 6) - 3; }; // a 6 px halftone screen at 30 degrees
                        if (std::hypot(wrap(gx * cs + gy * sn), wrap(gy * cs - gx * sn)) / (6 / 1.414f) < std::sqrt(level) * 0.95f) paper = 4 + (int) k;
                    }
                if (rank == 15 && ((uint32) bx * 73856093u ^ (uint32) by * 19349663u) % 100 >= 55) paper = 1; // grain
                plates[(size_t) (y * iw + x)] = (uint8) (accent | ramp(1 - (Y - 160) / 520 + n * 0.15f, 2, rank) << 2 | (plot ? 8 : 0) | paper << 4);
            }
    }
    void reinkArea(Rectangle<int> area)
    {
        const Image::BitmapData d(image, area.getX(), area.getY(), area.getWidth(), area.getHeight(), Image::BitmapData::readWrite);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
            {
                auto* p = reinterpret_cast<PixelRGB*>(d.getPixelPointer(x, y));
                p->set(reink(Colour(p->getRed(), p->getGreen(), p->getBlue()), plates[(size_t) ((area.getY() + y) * image.getWidth() + area.getX() + x)]).getPixelARGB());
            }
    }
    Rectangle<int> pixelsOf(int i, int j) const // a cell in device pixels, snapped to whole pixels
    {
        const auto px = [this](int k) { return roundToInt((float) k * (float) image.getWidth() / C); };
        const auto py = [this](int k) { return roundToInt((float) k * (float) image.getHeight() / R); };
        return Rectangle<int>::leftTopRightBottom(px(i), py(j), px(i + 1), py(j + 1));
    }
    // One cell in device pixels, snapped to whole pixels so lines and blocks join; vector glyphs as grid.py draws them.
    void drawCell(Graphics& g, int i, int j)
    {
        const auto& c = shown.at[j][i];
        const auto r = pixelsOf(i, j);
        g.setColour(c.bg.isTransparent() ? page : c.bg), g.fillRect(r);
        if (c.c == ' ' && c.left <= ' ') return;
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r), g.setColour(c.fg); // nothing spills into a neighbour, which may be redrawn alone
        const float w = (float) image.getWidth() / C, h = (float) image.getHeight() / R, x = (float) r.getX(), y = (float) r.getY();
        const float cx = x + (float) r.getWidth() / 2, cy = y + (float) r.getHeight() / 2, lw = jmax(1.0f, w * 0.12f);
        const int t = jmax(1, roundToInt(lw)), hx = roundToInt(cx - (float) t / 2), hy = roundToInt(cy - (float) t / 2);
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
        const int arms = c.c == 0x2500 ? 10 : c.c == 0x2502 ? 5 : c.c == 0x2534 || c.c == 0x2538 ? 11 : 0; // ─ │ ┴ ┸: up 1 right 2 down 4 left 8
        if (arms != 0)
        {
            const int top = c.c == 0x2534 ? (r.getY() + hy) / 2 : r.getY(); // ┴ a short tick, ┸ a tall one
            if (arms & 8) g.fillRect(r.getX(), hy, hx + t - r.getX(), t);
            if (arms & 2) g.fillRect(hx, hy, r.getRight() - hx, t);
            if (arms & 1) g.fillRect(hx, top, t, hy + t - top);
            if (arms & 4) g.fillRect(hx, hy, t, r.getBottom() - hy);
        }
        else if (c.c == 0x250a) // ┊
            for (int k = 0; k < 4; ++k) g.fillRect(Rectangle<float>((float) hx, y + h * ((float) k + 0.2f) / 4, (float) t, h * 0.15f));
        else if (c.c == 0x2591) // ░: a CRT dither of 4 dots
            for (auto [di, dj] : { std::pair { 0, 0 }, { 0, 2 }, { 1, 1 }, { 1, 3 } })
                g.fillRect(Rectangle<float>(0.22f * w, 0.22f * w).withCentre({ x + w * ((float) di * 0.5f + 0.25f), y + h * ((float) dj * 0.25f + 0.125f) }));
        else if (c.c == 0x2594) g.fillRect(r.withBottom(roundToInt(y + 0.125f * (float) r.getHeight()))); // ▔
        else if (c.c >= 0x2800 && c.c <= 0x28ff) // braille
        {
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                    if ((c.c - 0x2800) & brailleBit(dx, dy))
                        g.fillEllipse(Rectangle<float>(0.3f * w, 0.3f * w).withCentre({ x + (float) r.getWidth() * (0.3f + 0.4f * (float) dx), y + (float) r.getHeight() * (0.125f + 0.25f * (float) dy) }));
        }
        else if (c.c == 0x25a0) g.fillRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy })); // ■
        else if (c.c == 0x25a1) g.drawRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy }), lw); // □
        else if (c.c == 0x25b2 || c.c == 0x25c0 || c.c == 0x25c6) // ▲ ◀ ◆
        {
            const float rr = 0.36f * w, s = 0.95f * rr;
            Path p;
            if (c.c == 0x25c6) p.addQuadrilateral(cx, cy - 1.1f * rr, cx + rr, cy, cx, cy + 1.1f * rr, cx - rr, cy);
            else if (c.c == 0x25b2) p.addTriangle(cx, cy - s, cx + s, cy + 0.8f * s, cx - s, cy + 0.8f * s);
            else p.addTriangle(cx - s, cy, cx + 0.8f * s, cy - s, cx + 0.8f * s, cy + s);
            g.fillPath(p);
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
