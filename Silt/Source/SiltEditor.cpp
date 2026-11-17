// One cell grid and device-pixel backing image, with the renderer and dial gestures shared with gulp.
// Layout only lives in panels[]; the cloud and each control group own their hit cells independently.
#include "BinaryData.h"
#include "Silt.h"
#include <optional>
#include <tuple>
using namespace juce;
namespace
{
constexpr int C = 120, R = 44, W = 1024, H = 704;
constexpr float cw = (float) W / C, ch = (float) H / R;
const Colour page { 0xff0d0d0d }, white { 0xffececec }, grey { 0xff7a7a7a }, rule { 0xff2c2c2c }, dark { 0xff454545 }, red { 0xffff3d1f };
String u(const char* s) { return String::fromUTF8(s); }
String minus(const String& s) { return s.replace("-", u("−")); }
Rectangle<int> cells(int x, int y, int w, int h)
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

};

struct Key : Button
{
    explicit Key(const String& id) : Button(id) { setComponentID(id), setRepaintsOnMouseActivity(false); }
    void paintButton(Graphics&, bool, bool) override {}
};
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

struct Panel { const char* name; int x, y, w, h; };
// The only arrangement table. Coordinates below each group are local to its panel.
constexpr Panel panels[] { { "header", 2, 1, 116, 1 }, { "cloud", 2, 4, 116, 9 },
    { "grain", 2, 15, 56, 17 }, { "pitch", 62, 15, 56, 17 },
    { "trigger", 2, 33, 48, 10 }, { "space / output", 52, 33, 66, 10 }, { "macros", 62, 4, 56, 9 } };

struct Group : Component
{
    SiltProcessor& p;
    Panel spec;
    Group(SiltProcessor& processor, Panel panel) : p(processor), spec(panel)
    {
        setComponentID(String("group:") + panel.name), setBounds(cells(panel.x, panel.y, panel.w, panel.h));
    }
    virtual void draw(Grid& g) = 0;
    float value(const char* id) const { return p.apvts.getRawParameterValue(id)->load(); }
};
struct Header : Group
{
    Key prev { "prev" }, next { "next" }, save { "save" }, a { "A" }, b { "B" }, rnd { "rnd" }, macro { "macro" };
    std::function<void()> changed;
    explicit Header(SiltProcessor& proc) : Group(proc, panels[0])
    {
        prev.onClick = [this] { p.stepPreset(-1); }, next.onClick = [this] { p.stepPreset(1); };
        save.onClick = [this] { p.saveUserPreset(); }, a.onClick = [this] { p.selectAb(0); }, b.onClick = [this] { p.selectAb(1); };
        rnd.onClick = [this] { p.randomise(Time::getHighResolutionTicks()); };
        macro.onClick = [this] { p.showMacros = !p.showMacros; if (changed) changed(); };
        for (auto [key, x, w] : { std::tuple { &prev, 15, 2 }, { &next, 44, 2 }, { &save, 49, 4 }, { &a, 57, 1 }, { &b, 60, 1 }, { &rnd, 66, 3 }, { &macro, 74, 5 } })
            addAndMakeVisible(*key), key->setBounds(cells(x, 0, w, 1));
    }
    void draw(Grid& g) override
    {
        const int x = spec.x, y = spec.y;
        g.put(x, y, "silt", white, true), g.put(x + 15, y, u("‹"), grey), g.put(x + 44, y, u("›"), grey);
        g.put(x + 18, y, p.preset >= 0 ? String(p.preset.load()).paddedLeft('0', 2) : String("--"), grey);
        g.centre(x + 21, 22, y, p.presetName(), white, true), g.put(x + 49, y, "save", grey);
        g.put(x + 57, y, "A", p.abSlot() == 0 ? red : grey, p.abSlot() == 0), g.put(x + 60, y, "B", p.abSlot() == 1 ? red : grey, p.abSlot() == 1);
        g.put(x + 66, y, "rnd", grey), g.put(x + 74, y, "macro", p.showMacros ? red : grey);
        g.right(C - 3, y, "daveydidit", dark);
    }
};

// A separate cloud component: waveform ring, write head, position/spray band and per-grain pitch/envelope glyphs.
struct Cloud : Group
{
    explicit Cloud(SiltProcessor& proc) : Group(proc, panels[1]) {}
    void draw(Grid& g) override
    {
        const int x = spec.x, y = spec.y, w = p.showMacros ? 56 : spec.w, h = spec.h - 2;
        const auto& f = p.feed;
        const int source = roundToInt(value("source")), head = f.at[source].load();
        g.put(x, y, f.frozen.load() ? "capture / hold" : "capture", f.frozen.load() ? red : grey);
        g.right(x + w - 1, y, source == 0 ? "input" : "side", dark);
        Dots wave;
        for (int k = 0; k < 2 * w; ++k)
        {
            const int bin = k * silt::Feed::bins / (2 * w);
            const int amp = roundToInt(jlimit(0.0f, 1.0f, f.wave[source][(size_t) bin].load() * 2) * (float) (h * 2 - 1));
            for (int t = -amp; t <= amp; t += 2) wave.push_back({ k, h * 2 + t });
        }
        g.braille(x, y + 1, w, h, wave, dark);
        const auto column = [&](double back) { double q = head / (double) silt::Feed::bins - back / 8; q -= std::floor(q); return x + jlimit(0, w - 1, (int) (q * w)); };
        const int centre = column(f.centre.load()), spread = roundToInt(value("spray") / 8000 * (float) w);
        for (int k = -spread; k <= spread; ++k) g.put(x + (centre - x + k + w) % w, y + h + 1, u("▔"), red);
        g.put(column(0), y + h + 1, u("▲"), f.frozen.load() ? dark : white);
        for (size_t k = 0; k < silt::Feed::dots; ++k)
            if (const float envelope = f.env[k].load(); envelope > 0)
            {
                const int row = y + 1 + jlimit(0, h - 1, roundToInt((48 - f.pitch[k].load()) / 96 * (float) (h - 1)));
                g.put(column(f.back[k].load()), row, u("⠿"), dark.interpolatedWith(red, envelope));
            }
    }
};

struct Dial { const char *id, *name; int x, y; bool centred = false; };
struct Choice { const char *id, *name; int x, y, w; };
struct Switch { const char *id, *name; int x, y; };
struct Controls : Group
{
    std::vector<Dial> dials;
    std::vector<Choice> choices;
    std::vector<Switch> switches;
    OwnedArray<Slider> sliders;
    OwnedArray<Key> keys;
    std::vector<std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment>> sliderAtt;
    std::vector<std::unique_ptr<ParameterAttachment>> keyAtt;
    Controls(SiltProcessor& proc, Panel panel, std::initializer_list<Dial> ds, std::initializer_list<Choice> cs = {}, std::initializer_list<Switch> ss = {})
        : Group(proc, panel), dials(ds), choices(cs), switches(ss)
    {
        for (const auto& d : dials)
        {
            auto& k = *sliders.add(new Slider());
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(Slider::TextBoxBelow, false, cells(0,0,14,1).getWidth(), roundToInt(ch));
            k.setComponentID(d.id), k.setRepaintsOnMouseActivity(false);
            sliderAtt.push_back(std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts, d.id, k));
            k.textFromValueFunction = [f = k.textFromValueFunction](double v) { return minus(f(v)); };
            k.setDoubleClickReturnValue(true, p.apvts.getParameter(d.id)->convertFrom0to1(p.apvts.getParameter(d.id)->getDefaultValue()));
            addAndMakeVisible(k), k.setBounds(cells(d.x - 3, d.y - 1, 14, 6)), k.updateText();
        }
        for (const auto& c : choices)
        {
            auto att = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(c.id), [](float) {});
            for (int direction : { -1, 1 })
            {
                auto* key = keys.add(new Key(String(c.id) + (direction < 0 ? "-" : "+")));
                key->onClick = [this, c, direction, a = att.get()] { auto* q = p.apvts.getParameter(c.id); a->setValueAsCompleteGesture(jlimit(q->getNormalisableRange().start, q->getNormalisableRange().end, value(c.id) + (float) direction)); };
                addAndMakeVisible(key), key->setBounds(cells(c.x + (direction < 0 ? 0 : c.w - 1), c.y + 1, 1, 1));
            }
            keyAtt.push_back(std::move(att));
        }
        for (const auto& s : switches)
        {
            auto* key = keys.add(new Key(s.id));
            auto att = std::make_unique<ParameterAttachment>(*p.apvts.getParameter(s.id), [key](float v) { key->setToggleState(v > 0.5f, dontSendNotification); });
            key->onClick = [this, s, a = att.get()] { a->setValueAsCompleteGesture(value(s.id) > 0.5f ? 0.0f : 1.0f); };
            att->sendInitialUpdate(), keyAtt.push_back(std::move(att));
            addAndMakeVisible(key), key->setBounds(cells(s.x, s.y, (int) String(s.name).length() + 2, 1));
        }
    }
    void draw(Grid& g) override
    {
        const int x = spec.x, y = spec.y;
        g.put(x, y, spec.name, grey);
        for (const auto& d : dials)
            knob(g, x + d.x, y + d.y, p.apvts.getParameter(d.id)->getValue(), d.name, minus(p.apvts.getParameter(d.id)->getCurrentValueAsText()), d.centred, false);
        for (const auto& c : choices)
        {
            g.centre(x + c.x, c.w, y + c.y, c.name, grey);
            g.put(x + c.x, y + c.y + 1, u("‹"), grey), g.put(x + c.x + c.w - 1, y + c.y + 1, u("›"), grey);
            g.centre(x + c.x + 1, c.w - 2, y + c.y + 1, p.apvts.getParameter(c.id)->getCurrentValueAsText(), white, true);
        }
        for (const auto& s : switches) g.toggle(x + s.x, y + s.y, s.name, value(s.id) > 0.5f);
        for (auto* k : sliders) // labels depend on grid mode; don't disturb a text edit
            if (auto* label = dynamic_cast<Label*>(k->getChildComponent(0)); label != nullptr && !label->isBeingEdited()) k->updateText();
    }
};

struct Editor : AudioProcessorEditor, Timer
{
    SiltProcessor& proc;
    Look look;
    Component layer;
    Header header;
    Cloud cloud;
    Controls grain, pitch, trigger, space, macros;
    Grid shown;
    std::vector<float> drawnFrom;
    String drawnName;
    Image image;
    Font fonts[2] { mono(1, false), mono(1, true) };
    explicit Editor(SiltProcessor& p) : AudioProcessorEditor(p), proc(p), header(p), cloud(p),
        grain(p, panels[2], {{"size","size",3,3},{"density","density",17,3},{"position","position",31,3},{"spray","spray",45,3},
            {"slope","spray slope",3,10},{"scan","scan",17,10,true},{"skew","skew",31,10}},
            {{"grid","grid",42,9,12},{"sprayDir","spray dir",42,12,12}}),
        pitch(p, panels[3], {{"pitch","pitch",3,3,true},{"fine","fine",17,3,true},{"pitchRand","pitch rnd",31,3},{"reverse","reverse",45,3},
            {"width","width",3,10},{"pan","pan rnd",17,10},{"mix","mix",31,10},{"output","output",45,10,true}},
            {{"pitchMode","mode",0,15,12},{"snap","snap",14,15,12},{"window","window",28,15,12}}, {{"freeze","hold",43,16}}),
        trigger(p, panels[4], {{"rate","rate",3,3},{"jitter","jitter",18,3},{"thresh","thresh",33,3}},
            {{"trigger","source",0,8,12},{"division","division",15,8,12}}, {{"sync","sync",32,9}}),
        space(p, panels[5], {{"feedback","feedback",5,3},{"tone","tone",21,3,true},{"verbSize","space",37,3},{"verbMix","reverb",53,3}},
            {{"source","capture",0,8,12}}),
        macros(p, panels[6], {{"blur","blur",3,3},{"drift","drift",17,3},{"bloom","bloom",31,3},{"glint","glint",45,3}})
    {
        setOpaque(true), setLookAndFeel(&look), addAndMakeVisible(layer);
        layer.setBounds(0, 0, W, H);
        for (auto* group : groups()) layer.addAndMakeVisible(group);
        header.changed = [this] { macros.setVisible(proc.showMacros); refresh(); };
        macros.setVisible(proc.showMacros);
        const int stored = proc.editorWidth;
        setResizable(true, false), setResizeLimits(W / 2, H / 2, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio((double) W / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, stored) : W;
        setSize(width, width * H / W), timerCallback(), startTimerHz(30);
    }
    ~Editor() override { setLookAndFeel(nullptr); }
    std::array<Group*, 7> groups() { return { &header, &cloud, &grain, &pitch, &trigger, &space, &macros }; }
    void resized() override
    {
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }
    void timerCallback() override
    {
        std::vector<float> now;
        for (auto* param : proc.getParameters()) now.push_back(param->getValue());
        auto& f = proc.feed;
        now.insert(now.end(), { (float) f.at[0].load(), (float) f.at[1].load(), f.centre.load(), (float) f.frozen.load(), (float) proc.showMacros.load(), (float) proc.abSlot(), (float) proc.preset.load() });
        for (size_t k = 0; k < silt::Feed::bins; ++k) now.insert(now.end(), { f.wave[0][k].load(), f.wave[1][k].load() });
        for (size_t k = 0; k < silt::Feed::dots; ++k) now.insert(now.end(), { f.back[k].load(), f.pitch[k].load(), f.env[k].load() });
        if (now == drawnFrom && proc.presetName() == drawnName) return;
        drawnFrom = std::move(now), drawnName = proc.presetName();
        macros.setVisible(proc.showMacros), refresh();
    }
    void refresh()
    {
        Grid g;
        for (auto* group : groups()) if (group->isVisible()) group->draw(g);
        for (int y : { 2, 13, 32, 43 }) g.put(2, y, String::repeatedString(u("─"), C - 4), rule);
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
AudioProcessorEditor* SiltProcessor::createEditor() { return new Editor(*this); }
