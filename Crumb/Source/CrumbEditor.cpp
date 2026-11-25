// crumb's editor: design round 4, "wave" (crumb-options/round4/opt2.js without the verb section). One 132 x 41 cell grid
// scaled with the window (50-200 %, fixed aspect), as gulp's and silt's: text is Geist Mono in its cells; braille, shade,
// rules and markers are vectors per cell. The page is the riso paper, the accent prints in its ink ramp and the picture's
// neutrals in its shadow inks, from plates built once per image size (the same plates the site's skin uses).
// At 30 Hz the editor takes the samples the audio thread wrote (Wave.h draws them, nothing else moves) and rebuilds the
// grid when anything it shows changed; only cells that changed are redrawn and repainted: idle costs nothing.
// Controls are invisible Sliders and Buttons on their hit cells in a 1056 x 656 layer scaled with the window: drag,
// double-click = default, click the value to type. Every control is on the one page.
#include "BinaryData.h"
#include "Crumb.h"
#include "Wave.h"
#include <cstring>
#include <optional>

using namespace juce;

namespace
{
constexpr int C = 132, R = 41, W = 1056, H = 656, AX = 3, AT = 5, AW = crumb::Wave::columns / 2, AH = crumb::Wave::rows / 4;
constexpr int titleY = 19, knobY = 22, lowerY = 29, selectY = 36; // two rows of dials; selectors on lowerY + 1, + 4, + 7
constexpr float cw = 8, ch = 16;
const Colour page { 0xff0d0d0d }, primary { 0xffececec }, muted { 0xff7a7a7a }, rule { 0xff2a2a2a }, line { 0xff2c2c2c }, dim { 0xff454545 }, accent { 0xffff3d1f }, kick { 0xff5a5a5a }, hot { 0xffff2424 }; // hot: clipping, printed as it is
const Colour cobalt { 0xff5b7cff }, lilac { 0xffa98bff }, sun { 0xffffc83d }, tang { 0xffff6a2f }, pink { 0xffff4f9a }, cream { 0xffefe6d2 }; // riso inks
String u(const char* s) { return String::fromUTF8(s); }
String minus(const String& s) { return s.replace("-", u("−")); }
Rectangle<int> cells(int x, int y, int w, int h) { return { x * (int) cw, y * (int) ch, w * (int) cw, h * (int) ch }; }
constexpr uint8 brailleBit(int dx, int dy) { return (uint8) (dy == 3 ? 0x40 << dx : 1 << (dy + 3 * dx)); }
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size));
}

// The sections, in signal order, with their dials: two rows, left to right, `pitch` cells apart.
struct Section { const char* name; int x, w, pitch; };
const Section sections[] = { { "in", 3, 10, 0 }, { "crush", 16, 30, 11 }, { "loss", 49, 28, 10 }, { "filter", 80, 28, 10 }, { "out", 111, 18, 10 } };
enum Zoom { automatic, samples, frames }; // what a control's change makes the picture show
struct KnobSpec { const char *id, *name; int section, slot, row; Zoom zoom; bool centred; };
const KnobSpec knobSpecs[] = {
    { "preamp", "drive", 0, 0, 0, automatic, false }, { "gate", "gate", 0, 0, 1, automatic, false },
    { "bits", "bits", 1, 0, 0, samples, false }, { "rate", "rate", 1, 1, 0, samples, false }, { "jitter", "jitter", 1, 2, 0, samples, false },
    { "dither", "dither", 1, 0, 1, samples, false }, { "approxDev", "anti-alias", 1, 1, 1, samples, true }, { "imagesShift", "anti-image", 1, 2, 1, samples, false },
    { "loss", "loss", 2, 0, 0, frames, false }, { "speed", "speed", 2, 1, 0, frames, false }, { "drop", "packet loss", 2, 2, 0, frames, false },
    { "lowCut", "low cut", 3, 0, 0, automatic, false }, { "highCut", "high cut", 3, 1, 0, automatic, false }, { "reso", "resonance", 3, 2, 0, automatic, false },
    { "mix", "mix", 4, 0, 0, automatic, false }, { "output", "output", 4, 1, 0, automatic, false } };
constexpr int numKnobs = (int) std::size(knobSpecs);
int knobX(const KnobSpec& k) { const auto& s = sections[k.section]; return s.pitch > 0 ? s.x + k.slot * s.pitch : s.x + (s.w - 8) / 2; }
int knobRow(const KnobSpec& k) { return k.row == 0 ? knobY : lowerY; }

// `< name >`: the slot is the longest of its own names and one cell either side, so it never resizes. loss and filter stack
// theirs under their dials, labels and arrows each on one column, the stack centred; in and crush have one, centred.
struct SelectorSpec { const char *id, *label; StringArray names; int section, y; Zoom zoom; };
const SelectorSpec selectorSpecs[] = {
    { "stereo", "", { "stereo", "joint", "mono" }, 0, 2, frames }, { "dcShift", "dc shift", { "off", "on" }, 1, 2, samples },
    { "ear", "weighting", { "perceptual", "flat" }, 2, 0, frames }, { "fill", "packets", { "drop", "repeat" }, 2, 1, frames }, { "keep", "delta", { "off", "on" }, 2, 2, frames },
    { "slope", "slope", { "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB", "72 dB", "96 dB" }, 3, 0, automatic }, { "fOrder", "position", { "pre", "post" }, 3, 1, automatic }, { "invert", "invert", { "off", "on" }, 3, 2, automatic } };
constexpr int numSelectors = (int) std::size(selectorSpecs);
int slotWidth(const SelectorSpec& s)
{
    int longest = 0;
    for (const auto& n : s.names) longest = jmax(longest, n.length());
    return longest + 4;
}
int selectorY(const SelectorSpec& s) { return lowerY + 1 + 3 * s.y; }
std::pair<int, int> selectorX(const SelectorSpec& s) // the label's first cell, the slot's
{
    int label = 0, slot = 0;
    for (const auto& o : selectorSpecs)
        if (o.section == s.section) label = jmax(label, (int) std::strlen(o.label)), slot = jmax(slot, slotWidth(o));
    if (label > 0) ++label;
    const int x = sections[s.section].x + (sections[s.section].w - label - slot) / 2;
    return { x, x + label };
}

struct Cell
{
    juce_wchar c = ' ';
    Colour fg, bg; // bg transparent: the paper
    bool bold = false;
    uint8 edge = 0; // 1: a rule through the middle, 2: on the top edge
    juce_wchar left = 0; // nonzero: half-cell centring, with the previous glyph's right half clipped to this cell
    bool operator!=(const Cell& o) const { return c != o.c || fg != o.fg || bg != o.bg || bold != o.bold || edge != o.edge || left != o.left; }
};
enum Mode { merge, under, over }; // braille onto braille: or the dots, keep the old cell, replace it
using Dots = std::vector<std::pair<int, int>>;
struct Grid
{
    Cell at[R][C];
    void put(int x, int y, const String& s, Colour fg, bool bold = false, Colour bg = {})
    {
        for (auto p = s.getCharPointer(); !p.isEmpty(); ++x)
            if (const auto c = p.getAndAdvance(); isPositiveAndBelow(x, C) && isPositiveAndBelow(y, R))
                at[y][x].c = c, at[y][x].fg = fg, at[y][x].bg = bg, at[y][x].bold = bold, at[y][x].left = 0;
    }
    void centre(int x, int w, int y, String s, Colour fg, bool bold = false)
    {
        s = s.substring(0, w), x += (w - s.length()) / 2;
        put(x, y, s, fg, bold);
        if ((w - s.length()) % 2 != 0)
            for (int k = 0; k <= s.length(); ++k)
                if (isPositiveAndBelow(x + k, C) && isPositiveAndBelow(y, R))
                    at[y][x + k] = { k < s.length() ? s[k] : ' ', fg, {}, bold, at[y][x + k].edge, k > 0 ? s[k - 1] : ' ' };
    }
    void right(int x, int y, const String& s, Colour fg) { put(x - s.length() + 1, y, s, fg); }
    void edge(int x0, int x1, int y, uint8 kind)
    {
        for (int x = x0; x < x1; ++x) at[y][x].edge = kind;
    }
    void braille(int x, int y, int w, int h, const Dots& dots, Colour fg, Mode mode = merge) // dots in a 2w x 4h space
    {
        std::vector<uint8> bits((size_t) (w * h));
        for (auto [dx, dy] : dots)
            if (isPositiveAndBelow(dx, 2 * w) && isPositiveAndBelow(dy, 4 * h))
                bits[(size_t) (dy / 4 * w + dx / 2)] |= brailleBit(dx % 2, dy % 4);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                if (int v = bits[(size_t) (j * w + i)]; v != 0 && isPositiveAndBelow(x + i, C) && isPositiveAndBelow(y + j, R))
                {
                    auto& cell = at[y + j][x + i];
                    const bool old = cell.c >= 0x2800 && cell.c <= 0x28ff;
                    if (old && mode == under) continue;
                    cell = { (juce_wchar) (0x2800 + (v | (old && mode == merge ? (int) cell.c - 0x2800 : 0))), fg, {}, false, cell.edge };
                }
    }
};

// Controls draw nothing: the grid shows them. The value box only appears while a value is typed.
struct Look : LookAndFeel_V4
{
    Look()
    {
        setColour(PopupMenu::backgroundColourId, Colour(0xff141414)), setColour(PopupMenu::textColourId, cream);
        setColour(PopupMenu::highlightedBackgroundColourId, Colour(0xff2a2a2a)), setColour(PopupMenu::highlightedTextColourId, cream);
    }
    Font getPopupMenuFont() override { return mono(13, false); }
    Font getLabelFont(Label&) override { return mono(cw / 0.6f, false); }
    void drawLabel(Graphics&, Label&) override {}
    void drawRotarySlider(Graphics&, int, int, int, int, float, float, float, Slider&) override {}
    Label* createSliderTextBox(Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox(s);
        l->setColour(Label::backgroundWhenEditingColourId, page), l->setColour(Label::textWhenEditingColourId, cream);
        l->setColour(Label::outlineWhenEditingColourId, {}), l->setColour(TextEditor::highlightColourId, tang.withAlpha(0.4f));
        l->setColour(TextEditor::highlightedTextColourId, cream), l->setColour(CaretComponent::caretColourId, tang);
        return l;
    }
};
struct Key : Button
{
    std::function<void(int)> onStep; // a selector's arrow: the wheel and the arrow keys step too
    float wheel = 0;
    explicit Key(const String& id) : Button(id) { setComponentID(id), setRepaintsOnMouseActivity(false); }
    void paintButton(Graphics&, bool, bool) override {}
    void mouseWheelMove(const MouseEvent& e, const MouseWheelDetails& w) override
    {
        if (!onStep) return Button::mouseWheelMove(e, w);
        wheel += (std::abs(w.deltaY) > 0 ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
        if (std::abs(wheel) >= 0.12f) onStep(wheel > 0 ? 1 : -1), wheel = 0;
    }
    bool keyPressed(const KeyPress& k) override
    {
        const int code = k.getKeyCode();
        if (onStep && (code == KeyPress::upKey || code == KeyPress::rightKey || code == KeyPress::downKey || code == KeyPress::leftKey))
            return onStep(code == KeyPress::upKey || code == KeyPress::rightKey ? 1 : -1), true;
        return Button::keyPressed(k);
    }
};

// fbm of the family's plates: octaves of a seeded random lattice (3 * 2^o rows) smoothly stretched over h x w, 0..1.
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

// opt2.js's plates in design pixels: the accent's three-ink ramp and the picture's two shadow inks at 1x, the paper (page,
// grain, the picture's printed dots, a halftone glow per dial) at 2x. The ramp runs across the window as it did across the
// approved frame, with out in its ink. CrumbTests --plates dumps them for the site.
struct Plates
{
    Image ink { Image::RGB, W, H, true, SoftwareImageType() }, tint { Image::RGB, W, H, true, SoftwareImageType() }, paper { Image::RGB, 2 * W, 2 * H, true, SoftwareImageType() };
    static constexpr int frameW = 148 * 8, frameAW = 142;
    static constexpr int bayer[4][4] { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    static bool inPlot(int x, int y) { return x >= AX * 8 && x < (AX + AW) * 8 && y >= AT * 16 && y < (AT + AH) * 16; }
    static float frameX(int x, int y) // a design pixel's place across the approved frame
    {
        if (inPlot(x, y)) return (float) (AX * 8) + (float) (x - AX * 8) * (float) frameAW / (float) AW;
        return (float) (x < 108 * 8 ? x : x + 128);
    }
    static int ramp(float at, int stops, int rank)
    {
        const float v = jlimit(0.0f, 1.0f, at) * (float) (stops - 1);
        const int k = jmin((int) v, stops - 1);
        return jmin(stops - 1, k + (jlimit(0.0f, 1.0f, (v - (float) k - 0.3f) / 0.4f) > (float) rank / 16 ? 1 : 0));
    }
    Plates()
    {
        const auto noise = fbm(144, 256, 5, 21), field = fbm(AH + 1, frameAW + 1, 4, 918);
        const Colour inks[] { pink, tang, sun }, shadows[] { cobalt.interpolatedWith(cream, 0.2f), lilac.interpolatedWith(cream, 0.2f) };
        const Colour papers[] { page.interpolatedWith(lilac, 0.55f), page.interpolatedWith(cobalt, 0.55f) };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float fx = frameX(x, y), n = noise[(size_t) (y * 144 / H * 256 + jmin(255, (int) (fx * 256 / frameW)))] - 0.5f;
                const int rank = bayer[y % 4][x % 4];
                ink.setPixelAt(x, y, inks[ramp(fx / frameW + n * 0.1f, 3, rank)]);
                tint.setPixelAt(x, y, inPlot(x, y) ? shadows[ramp(1 - (float) (y - AT * 16) / (AH * 16) + n * 0.15f, 2, rank)] : page);
            }
        for (int y = 0; y < 2 * H; ++y)
            for (int x = 0; x < 2 * W; ++x)
                paper.setPixelAt(x, y, bayer[y / 2 % 4][x / 2 % 4] == 15 && ((uint32) x * 73856093u ^ (uint32) y * 19349663u) % 100 >= 55 ? Colour(0xff121212) : page);
        constexpr int dots[8][2] { { 5, 4 }, { 11, 4 }, { 5, 12 }, { 11, 12 }, { 5, 20 }, { 11, 20 }, { 5, 28 }, { 11, 28 } }, order[8] { 0, 4, 6, 2, 5, 1, 3, 7 };
        constexpr int ph = AH * 32;
        for (int r = 0; r < AH; ++r)
            for (int c = 0; c < AW; ++c)
            {
                const float v = jlimit(0.0f, 1.0f, (field[(size_t) (r * (frameAW + 1) + c)] * 0.85f + 0.15f * (float) (r * 32) / ph - 0.35f) * 1.6f);
                for (int k = 0; k < 8; ++k)
                    if (v * 8 > (float) order[k] + 0.5f)
                    {
                        const int dx = c * 16 + dots[k][0], dy = r * 32 + dots[k][1];
                        for (int q = 0; q < 9; ++q) paper.setPixelAt(AX * 16 + dx - 1 + q % 3, AT * 32 + dy - 1 + q / 3, papers[ramp((float) dy / ph, 2, bayer[dy / 2 % 4][dx / 2 % 4])]);
                    }
            }
        for (const auto& spec : knobSpecs)
            {
                const int x0 = knobX(spec), gx = (x0 + 4) * 16, gy = (knobRow(spec) + 2) * 32;
                const float at = (frameX(x0 * 8, knobY * 16) + 32) / (float) frameW, cs = std::cos(0.5236f), sn = std::sin(0.5236f);
                const Colour glow = page.interpolatedWith(inks[at < 1.0f / 3 ? 0 : at < 0.72f ? 1 : 2], 0.12f);
                const auto wrap = [](float t) { return t - 6 * std::floor(t / 6) - 3; };
                for (int y = -85; y < 85; ++y)
                    for (int x = -85; x < 85; ++x)
                    {
                        const float r = std::hypot((float) x, (float) y), a = (float) x + 85, b = (float) y + 85;
                        if (r > 2 * 7.9f * 4) continue;
                        const float level = std::pow(jmax(0.0f, 1 - r / 85), 1.6f);
                        if (std::hypot(wrap(a * cs + b * sn), wrap(b * cs - a * sn)) / (6 / 1.414f) < std::sqrt(level) * 0.95f) paper.setPixelAt(gx + x, gy + y, glow);
                    }
            }
    }
};
const Plates& plates()
{
    static const Plates p;
    return p;
}

struct Editor : AudioProcessorEditor, Timer
{
    CrumbProcessor& proc;
    Look look;
    Component layer; // the controls, in design pixels
    Slider knobs[numKnobs];
    Key prev { "prev" }, next { "next" }, name { "preset" }, slotA { "A" }, slotB { "B" }, random { "randomize" };
    OwnedArray<Key> arrows; // two per selector
    crumb::Wave wave;
    uint32_t seen = 0;
    double heard = -1e9, peakAt = 0;
    float outPeak = 0;
    struct Held { float db = 0; double at = 0; }; // a clip meter: its highest, held for 0.6 s
    Held driveClip, outClip;
    bool flat = true;
    Zoom zoom = automatic;
    Grid shown;
    std::vector<float> drawnFrom; // what the shown grid was built from: the same again means idle, nothing to rebuild
    String drawnName;
    Image image, inkPlate, tintPlate, paperPlate; // the shown grid and the plates, in device pixels
    std::vector<uint8> dotMasks[4]; // coverage of one cell's eight dots and the shade's four, per cell size (w, h each one of two)
    Font fonts[2] { mono(1, false), mono(1, true) };

    explicit Editor(CrumbProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setOpaque(true), setLookAndFeel(&look);
        addAndMakeVisible(layer);
        layer.setBounds(0, 0, W, H);
        for (int i = 0; i < numKnobs; ++i)
        {
            auto& k = knobs[i];
            const auto& s = knobSpecs[i];
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(Slider::TextBoxBelow, false, 80, 16);
            k.setTitle(s.name), k.setComponentID(s.id), k.setRepaintsOnMouseActivity(false);
            const auto range = knobRange(i);
            k.setRange(range.getStart(), range.getEnd(), 0);
            k.textFromValueFunction = [this, i](double) { return knobText(i); };
            k.valueFromTextFunction = [this, i](const String& t) { return knobFromText(i, t); };
            k.setDoubleClickReturnValue(true, knobDefault(i));
            k.setValue(knobValue(i), dontSendNotification);
            k.onDragStart = [this, i] { gesture(i, true); };
            k.onDragEnd = [this, i] { gesture(i, false); };
            k.onValueChange = [this, i] { setKnob(i, knobs[i].getValue()); };
            k.setBounds(cells(knobX(s) - 1, knobRow(s) - 1, 10, 6));
            layer.addAndMakeVisible(k);
        }
        prev.onClick = [this] { proc.stepPreset(-1), zoom = automatic; };
        next.onClick = [this] { proc.stepPreset(1), zoom = automatic; };
        name.onClick = [this] { showPresets(); };
        slotA.onClick = [this] { proc.selectAb(0), zoom = automatic; };
        slotB.onClick = [this] { proc.selectAb(1), zoom = automatic; };
        random.onClick = [this] { proc.randomize(Time::currentTimeMillis()), zoom = automatic; };
        for (auto [key, x, w] : { std::tuple { &prev, 11, 3 }, { &next, 38, 3 }, { &name, 14, 24 }, { &slotA, 43, 1 }, { &slotB, 45, 1 }, { &random, 49, 9 } })
            layer.addAndMakeVisible(*key), key->setBounds(cells(x, 1, w, 1));
        prev.setTitle("previous preset"), next.setTitle("next preset"), name.setTitle("presets");
        prev.onStep = next.onStep = [this](int by) { proc.stepPreset(by), zoom = automatic; };
        for (int i = 0; i < numSelectors; ++i)
            for (int by : { -1, 1 })
            {
                const auto& s = selectorSpecs[i];
                auto* key = arrows.add(new Key(String(s.id) + (by < 0 ? "-" : "+")));
                const int x = selectorX(s).second;
                key->setBounds(cells(by < 0 ? x : x + slotWidth(s) - 2, selectorY(s), 2, 1));
                key->onClick = [this, i, by] { step(i, by); }, key->onStep = [this, i](int d) { step(i, d); };
                key->setTitle(String(by < 0 ? "previous " : "next ") + (s.label[0] != 0 ? s.label : s.id));
                layer.addAndMakeVisible(key);
            }

        const int stored = proc.editorWidth; // read first: setting the limits already resizes
        setResizable(true, false);
        setResizeLimits(W / 2, H / 2, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio((double) W / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, stored) : W;
        setSize(width, roundToInt(width * (double) H / W));
        seen = proc.scope.write.load(std::memory_order_acquire) - (uint32_t) crumb::Scope::size;
        heard = peakAt = proc.scope.clock();
        timerCallback();
        startTimerHz(30);
    }
    ~Editor() override { setLookAndFeel(nullptr); }

    // ---- values
    float value(const char* id) const { return proc.apvts.getRawParameterValue(id)->load(); }
    int index(const char* id) const { return roundToInt(value(id)); }
    void setParam(const char* id, float v)
    {
        auto* p = proc.apvts.getParameter(id);
        p->beginChangeGesture(), p->setValueNotifyingHost(p->convertTo0to1(v)), p->endChangeGesture();
    }
    static bool is(const KnobSpec& s, const char* id) { return std::strcmp(s.id, id) == 0; }
    // A dial's position 0..1; just past an end lies "off" (bits over 24, approx and images under their least).
    static constexpr double offAt = 0.06;
    static Range<double> knobRange(int i)
    {
        const auto& s = knobSpecs[i];
        return { is(s, "approxDev") || is(s, "imagesShift") ? -offAt : 0, is(s, "bits") ? 1 + offAt : 1 };
    }
    double knobValue(int i) const
    {
        const auto& s = knobSpecs[i];
        const auto* p = proc.apvts.getParameter(s.id);
        if (is(s, "bits")) return index("bits") == 0 ? 1 + offAt : (index("bits") - 1) / 23.0;
        if (is(s, "approxDev")) return value("approxOn") > 0.5f ? p->getValue() : -offAt;
        if (is(s, "imagesShift")) return value("imagesOn") > 0.5f ? p->getValue() : -offAt;
        if (is(s, "lowCut")) return p->convertTo0to1(proc.band().low);
        if (is(s, "highCut")) return p->convertTo0to1(proc.band().high);
        return p->getValue();
    }
    static double knobDefaultFor(const CrumbProcessor& proc, int i)
    {
        const auto& s = knobSpecs[i];
        return is(s, "bits") ? 11 / 23.0 : is(s, "imagesShift") ? -offAt : (double) proc.apvts.getParameter(s.id)->getDefaultValue();
    }
    double knobDefault(int i) const { return knobDefaultFor(proc, i); }
    String knobText(int i) const
    {
        const auto& s = knobSpecs[i];
        const auto* p = proc.apvts.getParameter(s.id);
        if (is(s, "bits")) return index("bits") == 0 ? String("off") : String(index("bits"));
        if ((is(s, "approxDev") && value("approxOn") < 0.5f) || (is(s, "imagesShift") && value("imagesOn") < 0.5f)) return "off";
        if (is(s, "lowCut") || is(s, "highCut")) return minus(p->getText((float) knobValue(i), 0));
        return minus(p->getCurrentValueAsText());
    }
    double knobFromText(int i, const String& t) const
    {
        const auto& s = knobSpecs[i];
        const bool off = t.containsIgnoreCase("off");
        if (is(s, "bits")) return off || t.getIntValue() == 0 ? 1 + offAt : (jlimit(1, 24, t.getIntValue()) - 1) / 23.0;
        if (off && knobRange(i).getStart() < 0) return -offAt;
        return proc.apvts.getParameter(s.id)->getValueForText(t);
    }
    const char* switchOf(int i) const { return is(knobSpecs[i], "approxDev") ? "approxOn" : is(knobSpecs[i], "imagesShift") ? "imagesOn" : nullptr; }
    void gesture(int i, bool begin)
    {
        const auto& s = knobSpecs[i];
        if (begin && (is(s, "lowCut") || is(s, "highCut"))) proc.adoptBand();
        for (auto* id : { s.id, switchOf(i) })
            if (id != nullptr)
            {
                auto* p = proc.apvts.getParameter(id);
                begin ? p->beginChangeGesture() : p->endChangeGesture();
            }
    }
    void setKnob(int i, double v)
    {
        const auto& s = knobSpecs[i];
        auto* p = proc.apvts.getParameter(s.id);
        const auto put = [](RangedAudioParameter* q, float n) {
            if (const float legal = q->convertTo0to1(q->convertFrom0to1(jlimit(0.0f, 1.0f, n))); std::abs(legal - q->getValue()) > 1e-7f) q->setValueNotifyingHost(legal);
        };
        if (is(s, "bits")) put(p, p->convertTo0to1(v > 1 + offAt / 2 ? 0.0f : (float) (1 + roundToInt(jmin(1.0, v) * 23))));
        else
        {
            if (const auto* sw = switchOf(i)) put(proc.apvts.getParameter(sw), v < -offAt / 2 ? 0.0f : 1.0f);
            if (v >= 0 || switchOf(i) == nullptr) put(p, (float) v);
        }
        if (s.zoom != automatic) zoom = s.zoom;
    }
    void step(int i, int by)
    {
        const auto& s = selectorSpecs[i];
        if (std::strcmp(s.id, "invert") == 0 || std::strcmp(s.id, "slope") == 0) proc.adoptBand();
        setParam(s.id, (float) ((index(s.id) + by + s.names.size()) % s.names.size()));
        if (s.zoom != automatic) zoom = s.zoom;
    }
    void showPresets()
    {
        PopupMenu menu;
        for (int i : crumb::presetOrder) menu.addItem(1 + i, proc.getProgramName(i), true, proc.preset == i);
        const auto files = proc.userPresets();
        if (!files.isEmpty()) menu.addSeparator();
        for (int k = 0; k < files.size(); ++k) menu.addItem(1000 + k, files[k], true, proc.preset < 0 && proc.presetName() == files[k]);
        menu.addSeparator();
        menu.addItem(999, "save");
        menu.setLookAndFeel(&look);
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(name), [self = SafePointer<Editor>(this), files](int r) {
            if (!self || r == 0) return;
            if (r == 999) self->proc.saveUserPreset();
            else if (r >= 1000) self->proc.loadUserPreset(files[r - 1000]);
            else self->proc.setCurrentProgram(r - 1);
            self->zoom = automatic;
        });
    }
    void resized() override
    {
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }

    // ---- the picture
    // One step a codec frame's hop (at least 256 samples at 48 kHz), three columns wide, while the codec runs; the samples
    // themselves, four columns a hold, while it does not or the crush controls are being turned.
    bool showsFrames() const { return zoom == automatic ? proc.codecOn() : zoom == frames; }
    void buildWave()
    {
        const double fs = proc.sampleRate.load();
        if (showsFrames())
        {
            const int scale = crumb::rateScale(fs), hop = crumb::hops[crumb::speedIndex(value("speed") / 100)];
            // a frame shows from a latency and half a hop after it was coded
            const auto coded = (int64_t) (uint32_t) (seen - proc.scope.origin.load(std::memory_order_relaxed)) - 2048 * scale - hop * scale / 2;
            wave.build(hop * ((255 + hop) / hop) * scale, 3, false, 0, coded);
        }
        else
            wave.build(jlimit(0.5, 0.04 * fs / crumb::Wave::columns, 0.25 * fs / jmin(fs, (double) value("rate"))), 1, true, (int) (0.04 * fs));
    }
    void timerCallback() override
    {
        const double t = proc.scope.clock();
        auto& scope = proc.scope;
        const auto w = scope.write.load(std::memory_order_acquire);
        const bool fresh = w != seen;
        float cut = 0, over = 0;
        if (w - seen > (uint32_t) crumb::Scope::size) seen = w - (uint32_t) crumb::Scope::size;
        for (; seen != w; ++seen)
        {
            const auto i = (size_t) (seen & (crumb::Scope::size - 1));
            wave.add(scope.dry[i].load(std::memory_order_relaxed), scope.wet[i].load(std::memory_order_relaxed), scope.gone[i].load(std::memory_order_relaxed), scope.mark[i].load(std::memory_order_relaxed),
                     scope.cut[i].load(std::memory_order_relaxed), scope.over[i].load(std::memory_order_relaxed));
            cut = jmax(cut, std::abs(scope.cut[i].load(std::memory_order_relaxed))), over = jmax(over, std::abs(scope.over[i].load(std::memory_order_relaxed)));
        }
        if (fresh) heard = t, flat = false;
        else if (!flat && t - heard > 150) wave.clear(), scope.peak = outPeak = 0, driveClip = outClip = {}, flat = true; // no audio: a flat line, nothing lit
        for (auto [held, now] : { std::pair { &driveClip, cut }, { &outClip, over } })
            if (now >= held->db || t - held->at > 600) *held = { now, t };
        // The output's peak, falling 20 dB a second.
        outPeak = jmax(scope.peak.exchange(0.0f, std::memory_order_relaxed), outPeak * Decibels::decibelsToGain((float) (-20 * (t - peakAt) / 1000)));
        if (outPeak < 1e-5f) outPeak = 0;
        peakAt = t;
        for (int i = 0; i < numKnobs; ++i) // the parameters move the dials (also while one is typed into: not its text)
            if (auto& k = knobs[i]; !k.isMouseButtonDown())
                if (const double v = knobValue(i); std::abs(v - k.getValue()) > 1e-6) k.setValue(v, dontSendNotification);
        buildWave();

        std::vector<float> now;
        for (auto* param : proc.getParameters()) now.push_back(param->getValue());
        now.insert(now.end(), { (float) proc.preset.load(), (float) proc.abSlot(), (float) showsFrames(), (float) meter(driveClip), (float) meter(outClip), (float) roundToInt(10 * Decibels::gainToDecibels(outPeak, -100.0f)) });
        int hovered = 0;
        for (auto* key : arrows) hovered = 2 * hovered + (key->isMouseOver() ? 1 : 0);
        now.push_back((float) hovered);
        now.push_back((float) (prev.isMouseOver() ? 1 : next.isMouseOver() ? 2 : random.isMouseOver() ? 3 : 0));
        now.push_back(flat ? 0.0f : 1.0f);
        if (!flat)
            for (const auto& c : wave.column) now.insert(now.end(), { c.dry.lo, c.dry.hi, c.wet.lo, c.wet.hi, c.gone.lo, c.gone.hi, c.cut.lo, c.cut.hi, c.over.lo, c.over.hi, (float) c.mark });
        now.push_back(flat ? 0 : wave.peak);
        if (now == drawnFrom && proc.presetName() == drawnName) return;
        drawnFrom = std::move(now), drawnName = proc.presetName();
        refresh();
    }

    // gulp's dial: a round braille ring of 16 x 16 dots, the value's arc (centred: from the top) and a two-dot needle.
    static void knob(Grid& g, int x, int y, double norm, const String& label, const String& text, bool centred)
    {
        Dots track, arc, needle;
        norm = jlimit(0.0, 1.0, norm);
        const double c = 7.5, r = 7.2, v = 270 * norm;
        for (int t = 0; t <= 270; t += 3)
        {
            const double a = degreesToRadians(225.0 + t);
            ((centred ? jmin(135.0, v) <= t && t <= jmax(135.0, v) : t <= v) ? arc : track).push_back({ (int) (c + r * std::sin(a)), (int) (c - r * std::cos(a)) });
        }
        const double a = degreesToRadians(225 + v), sn = std::sin(a), cs = std::cos(a);
        for (auto [x0, y0, len] : { std::tuple { c, c, r - 2 }, { c + 0.5, c + 0.5, r - 2.5 } })
        {
            const int steps = (int) jmax(std::abs(len * sn), std::abs(len * cs)) + 1;
            for (int k = 0; k < steps; ++k)
                needle.push_back({ (int) std::nearbyint(x0 + len * sn * k / jmax(1, steps - 1)), (int) std::nearbyint(y0 - len * cs * k / jmax(1, steps - 1)) });
        }
        g.braille(x, y, 8, 4, track, dim), g.braille(x, y, 8, 4, arc, accent), g.braille(x, y, 8, 4, needle, primary, over);
        g.centre(x - 3, 14, y - 1, label, muted), g.centre(x - 3, 14, y + 4, text, primary, true);
    }
    // A clip meter's lit cells: one a dB, at least one while it clips.
    static constexpr int meterCells = 11;
    static int meter(const Held& h) { return h.db > 0 ? jlimit(1, meterCells, (int) std::floor(h.db + 0.5f)) : 0; }
    void refresh()
    {
        Grid g;
        // crumb < preset > A B randomize, the maker; a rule on the edge under them.
        g.put(3, 1, "crumb", primary, true);
        g.put(12, 1, "<", prev.isMouseOver() ? primary : muted), g.centre(14, 24, 1, proc.presetName(), primary, true), g.put(39, 1, ">", next.isMouseOver() ? primary : muted);
        for (int k = 0; k < 2; ++k)
            if (proc.abSlot() == k) g.put(43 + 2 * k, 1, k == 0 ? "A" : "B", page, true, primary);
            else g.put(43 + 2 * k, 1, k == 0 ? "A" : "B", dim);
        g.put(49, 1, "randomize", random.isMouseOver() ? primary : muted);
        g.right(C - 4, 1, "daveydidit", dim);
        g.edge(3, C - 3, 3, 2);

        for (auto [i, j] : wave.water) g.put(AX + i, AT + j, u("░"), rule);
        g.braille(AX, AT, AW, AH, wave.line, accent), g.braille(AX, AT, AW, AH, wave.lost, primary, over);
        g.braille(AX, AT, AW, AH, wave.hot, hot);
        g.braille(AX, AT, AW, AH, wave.taken, muted, under), g.braille(AX, AT, AW, AH, wave.ghost, showsFrames() ? kick : muted, under);

        for (const auto& s : sections)
        {
            if (&s != sections) g.put(s.x - 2, titleY, u("→"), muted);
            g.put(s.x, titleY, s.name, primary), g.edge(s.x + (int) std::strlen(s.name) + 1, s.x + s.w, titleY, 1);
        }
        for (int i = 0; i < numKnobs; ++i)
        {
            const auto& s = knobSpecs[i];
            knob(g, knobX(s), knobRow(s), knobValue(i), s.name, knobText(i), s.centred);
        }
        for (int i = 0; i < numSelectors; ++i)
        {
            const auto& s = selectorSpecs[i];
            const auto [x, at] = selectorX(s);
            const int w = slotWidth(s), y = selectorY(s), chosen = std::strcmp(s.id, "invert") == 0 ? (proc.band().invert ? 1 : 0) : index(s.id);
            g.put(x, y, s.label, muted);
            g.put(at, y, "<", arrows[2 * i]->isMouseOver() ? primary : muted), g.put(at + w - 1, y, ">", arrows[2 * i + 1]->isMouseOver() ? primary : muted);
            g.centre(at + 1, w - 2, y, s.names[jlimit(0, s.names.size() - 1, chosen)], primary, true);
        }
        // Where it clips, lit by the samples themselves: the drive's clip, the output past 0 dBFS; the output's peak.
        const auto& out = sections[4];
        const int lit[2] { meter(driveClip), meter(outClip) };
        g.put(out.x, lowerY - 1, "clip", muted), g.put(out.x + out.w - 1, lowerY - 1, u("⠶"), lit[0] + lit[1] > 0 ? hot : dim);
        for (int k = 0; k < 2; ++k)
        {
            g.put(out.x, lowerY + 1 + 3 * k, k == 0 ? "drive" : "output", muted);
            for (int c = 0; c < meterCells; ++c) g.put(out.x + out.w - meterCells + c, lowerY + 1 + 3 * k, u("⠶"), c < lit[k] ? hot : dim);
        }
        g.put(out.x, selectY, "peak", muted), g.put(out.x + out.w - meterCells, selectY, outPeak > 0 ? (outPeak > 1 ? "+" : "") + minus(String(Decibels::gainToDecibels(outPeak), 1)) + " dB" : u("−inf"), primary, true);
        g.put(3, R - 3, String::repeatedString(u("─"), C - 6), rule);
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
            image = Image(Image::RGB, iw, ih, false, SoftwareImageType()); // software: the dots are written in place
            fonts[0] = mono((float) iw / C / 0.6f, false), fonts[1] = mono((float) iw / C / 0.6f, true);
            cutPlates(iw, ih);
            Graphics ig(image);
            for (int j = 0; j < R; ++j)
                for (int i = 0; i < C; ++i)
                    drawCell(ig, i, j);
        }
        g.drawImageTransformed(image, AffineTransform::scale((float) getWidth() / (float) iw, (float) getHeight() / (float) ih));
    }
    // The plates at this editor's size in points (inks by the nearest pixel, the 2x paper averaged), then in device pixels.
    void cutPlates(int iw, int ih)
    {
        const int w = jmax(1, jmin(iw, getWidth())), h = jmax(1, jmin(ih, getHeight()));
        const auto& from = plates();
        const auto device = [&](const std::function<Colour(int, int)>& at) {
            Image out(Image::RGB, iw, ih, false, SoftwareImageType());
            std::vector<Colour> row((size_t) w);
            const Image::BitmapData d(out, Image::BitmapData::writeOnly);
            for (int y = 0, done = -1; y < ih; ++y)
            {
                if (const int py = jmin(h - 1, y * h / ih); py != done)
                {
                    done = py;
                    for (int x = 0; x < w; ++x) row[(size_t) x] = at(x, py);
                }
                for (int x = 0; x < iw; ++x) d.setPixelColour(x, y, row[(size_t) jmin(w - 1, x * w / iw)]);
            }
            return out;
        };
        const auto nearest = [&](const Image& img) {
            return device([&](int x, int y) { return img.getPixelAt((int) (((float) x + 0.5f) * W / (float) w), (int) (((float) y + 0.5f) * H / (float) h)); });
        };
        inkPlate = nearest(from.ink), tintPlate = nearest(from.tint);
        const Image::BitmapData p(from.paper, Image::BitmapData::readOnly);
        paperPlate = device([&](int x, int y) {
            const double x0 = x * 2.0 * W / w, x1 = (x + 1) * 2.0 * W / w, y0 = y * 2.0 * H / h, y1 = (y + 1) * 2.0 * H / h;
            double sum[3] {}, area = 0;
            for (int sy = (int) y0; sy < jmin(2 * H, (int) std::ceil(y1)); ++sy)
                for (int sx = (int) x0; sx < jmin(2 * W, (int) std::ceil(x1)); ++sx)
                {
                    const double a = (jmin(x1, sx + 1.0) - jmax(x0, (double) sx)) * (jmin(y1, sy + 1.0) - jmax(y0, (double) sy));
                    const auto c = p.getPixelColour(sx, sy);
                    sum[0] += a * c.getRed(), sum[1] += a * c.getGreen(), sum[2] += a * c.getBlue(), area += a;
                }
            return Colour((uint8) roundToInt(sum[0] / area), (uint8) roundToInt(sum[1] / area), (uint8) roundToInt(sum[2] / area));
        });
        // One cell's eight braille dots (one byte each, coverage 0..255), then the shade's four, for the cell sizes in use.
        const float cellW = (float) iw / C, cellH = (float) ih / R;
        for (int s = 0; s < 4; ++s)
        {
            const int rw = (int) cellW + s % 2, rh = (int) cellH + s / 2;
            dotMasks[s].assign((size_t) (9 * rw * rh), 0);
            constexpr int fine = 8;
            for (int q = 0; q < 12; ++q)
            {
                const float r = 0.15f * cellW, half = 0.11f * cellW;
                const float x = q < 8 ? (float) rw * (0.3f + 0.4f * (float) (q % 2)) : cellW * ((float) ((q - 8) / 2) * 0.5f + 0.25f);
                const float y = q < 8 ? (float) rh * (0.125f + 0.25f * (float) (q / 2)) : cellH * ((float) ((q - 8) / 2 + 2 * ((q - 8) % 2)) * 0.25f + 0.125f);
                for (int py = 0; py < rh; ++py)
                    for (int px = 0; px < rw; ++px)
                    {
                        int hits = 0;
                        for (int k = 0; k < fine * fine; ++k)
                        {
                            const float dx = (float) px + ((float) (k % fine) + 0.5f) / fine - x, dy = (float) py + ((float) (k / fine) + 0.5f) / fine - y;
                            hits += (q < 8 ? dx * dx + dy * dy <= r * r : std::abs(dx) <= half && std::abs(dy) <= half) ? 1 : 0;
                        }
                        auto& m = dotMasks[s][(size_t) (jmin(q, 8) * rw * rh + py * rw + px)];
                        m = (uint8) jmin(255, (int) m + hits * 255 / (fine * fine));
                    }
            }
        }
    }
    Rectangle<int> pixelsOf(int i, int j) const // a cell in device pixels, snapped to whole pixels
    {
        const auto px = [this](int k) { return roundToInt((float) k * (float) image.getWidth() / C); };
        const auto py = [this](int k) { return roundToInt((float) k * (float) image.getHeight() / R); };
        return Rectangle<int>::leftTopRightBottom(px(i), py(j), px(i + 1), py(j + 1));
    }
    // How a colour prints (the site's riso.js): the accent in its ink, the picture's neutrals in their shadow ink, other
    // bright neutrals cream, anything else as it is.
    struct Ink { const Image* plate; float alpha; Colour colour; };
    Ink inkOf(Colour c, int i, int j) const
    {
        if (c == hot) return { nullptr, 1, c };
        const int r = c.getRed(), g = c.getGreen(), b = c.getBlue();
        const float lum = (float) (r + g + b) / 3, t = jlimit(0.0f, 1.0f, (lum - 13) / 223);
        if (r - g > 40 && r - b > 30) return { &inkPlate, jlimit(0.0f, 1.0f, (float) (r - 13) / 242), {} };
        const bool plot = i >= AX && i < AX + AW && j >= AT && j < AT + AH;
        if (plot && lum > 20) return { &tintPlate, t, {} };
        return { nullptr, 1, lum > 150 ? page.interpolatedWith(cream, t) : c };
    }
    // A cell of paper and dots (a mask layer per bit, one byte of coverage per pixel), straight into the image.
    void printDots(Rectangle<int> r, const std::vector<uint8>& masks, int bits, const Ink& ink)
    {
        const Image::BitmapData to(image, r.getX(), r.getY(), r.getWidth(), r.getHeight(), Image::BitmapData::readWrite);
        const Image::BitmapData paper(paperPlate, r.getX(), r.getY(), r.getWidth(), r.getHeight());
        std::optional<Image::BitmapData> plate;
        if (ink.plate != nullptr) plate.emplace(*ink.plate, r.getX(), r.getY(), r.getWidth(), r.getHeight());
        const int w = r.getWidth(), h = r.getHeight(), n = w * h;
        const uint8* layers[9];
        int count = 0;
        for (int q = 0; q < 9; ++q)
            if (bits & 1 << q) layers[count++] = masks.data() + q * n;
        for (int y = 0; y < h; ++y)
        {
            std::memcpy(to.getLinePointer(y), paper.getLinePointer(y), (size_t) (w * to.pixelStride));
            for (int x = 0; x < w; ++x)
            {
                int cover = 0;
                for (int q = 0; q < count; ++q) cover += layers[q][y * w + x];
                if (cover == 0) continue;
                const float a = ink.alpha * (float) jmin(255, cover) / 255;
                auto* p = reinterpret_cast<PixelRGB*>(to.getPixelPointer(x, y));
                const auto mix = [a](uint8 under, uint8 top) { return (uint8) ((float) under + ((float) top - (float) under) * a + 0.5f); };
                const auto c = plate ? plate->getPixelColour(x, y) : ink.colour;
                p->setARGB(255, mix(p->getRed(), c.getRed()), mix(p->getGreen(), c.getGreen()), mix(p->getBlue(), c.getBlue()));
            }
        }
    }
    // One cell in device pixels, snapped to whole pixels so lines and blocks join; vector glyphs as grid.js draws them.
    void drawCell(Graphics& g, int i, int j)
    {
        const auto& c = shown.at[j][i];
        const auto r = pixelsOf(i, j);
        const bool braille = c.c >= 0x2800 && c.c <= 0x28ff, shade = c.c == 0x2591, plain = c.edge == 0 && c.bg.isTransparent();
        if (plain && (braille || shade || (c.c == ' ' && c.left <= ' ')))
        {
            const int s = (r.getWidth() > image.getWidth() / C ? 1 : 0) + (r.getHeight() > image.getHeight() / R ? 2 : 0);
            int bits = shade ? 1 << 8 : 0;
            for (int q = 0; q < 8; ++q)
                if (braille && ((c.c - 0x2800) & brailleBit(q % 2, q / 2))) bits |= 1 << q;
            return printDots(r, dotMasks[s], bits, inkOf(c.fg, i, j));
        }
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r); // nothing spills into a neighbour, which may be redrawn alone
        g.drawImageAt(paperPlate, 0, 0);
        if (!c.bg.isTransparent()) g.setColour(inkOf(c.bg, i, j).colour), g.fillRect(r);
        const float w = (float) image.getWidth() / C, h = (float) image.getHeight() / R, x = (float) r.getX(), y = (float) r.getY();
        const float cx = x + (float) r.getWidth() / 2, cy = y + (float) r.getHeight() / 2, lw = jmax(1.0f, w * 0.12f);
        const int t = jmax(1, roundToInt(lw)), hy = roundToInt(cy - (float) t / 2);
        if (c.edge != 0)
            g.setColour(line), g.fillRect(r.getX(), c.edge == 2 ? r.getY() : roundToInt(y + (float) r.getHeight() / 2), r.getWidth(), jmax(1, roundToInt((float) image.getWidth() / (float) jmax(1, getWidth()))));
        if (c.c == ' ' && c.left <= ' ') return;
        const auto ink = inkOf(c.fg, i, j);
        if (ink.plate != nullptr) g.setTiledImageFill(*ink.plate, 0, 0, ink.alpha);
        else g.setColour(ink.colour);
        const auto glyph = [&](juce_wchar code, float offset) {
            const float gx = (c.left != 0 ? (float) i * w : x + ((float) r.getWidth() - w) / 2) + offset;
            if (const auto id = fonts[c.bold ? 1 : 0].getTypefacePtr()->getNominalGlyphForCodepoint(code))
            {
                const float size = w / 0.6f; // Geist Mono: advance 0.6 em, ascent 1.005 + descent 0.295 em
                const uint16_t at = (uint16_t) *id;
                const Point<float> origin { gx, y + (h - 1.3f * size) / 2 + 1.005f * size };
                g.setFont(fonts[c.bold ? 1 : 0]);
                g.getInternalContext().drawGlyphs({ &at, 1 }, { &origin, 1 }, {});
            }
        };
        if (c.left != 0) { glyph(c.left, -w / 2), glyph(c.c, w / 2); return; }
        if (c.c == 0x2500) g.fillRect(r.getX(), hy, r.getWidth(), t); // ─
        else if (c.c == 0x2192) // →
        {
            Path p;
            p.startNewSubPath(cx - 0.32f * w, cy + 0.04f * h), p.lineTo(cx + 0.3f * w, cy + 0.04f * h);
            p.startNewSubPath(cx + 0.08f * w, cy - 0.08f * h), p.lineTo(cx + 0.3f * w, cy + 0.04f * h), p.lineTo(cx + 0.08f * w, cy + 0.16f * h);
            g.strokePath(p, PathStrokeType((float) t));
        }
        else glyph(c.c, 0);
    }
};
} // namespace

AudioProcessorEditor* CrumbProcessor::createEditor() { return new Editor(*this); }

namespace crumb
{
// The plates as PNGs (tests and the site: CrumbTests --plates).
juce::Image plate(int k) { return k == 0 ? plates().ink : k == 1 ? plates().tint : plates().paper; }
} // namespace crumb
