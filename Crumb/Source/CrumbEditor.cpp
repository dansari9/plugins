// crumb editor: ASCII design option 5, "one wave" (crusher-design/ascii/option_5.*, made by src/grid.py + opt5.py).
// One 120 x 34 cell grid scaled with the window (50-200 %, fixed aspect). Text is Geist Mono in its cells; braille, box,
// block and geometric glyphs are vectors per cell, so they join. The grid is rebuilt from the state at 30 Hz: the 20 ms
// scope runs through an editor-side copy of the stage chain and crosses the panel, each stage showing its own output.
// Only cells that changed are redrawn into a device-pixel image and repainted, so an idle editor paints nothing.
// Controls are invisible Sliders and Buttons on their hit cells in a 1024 x 576 layer scaled with the window: drag,
// double-click = default and click the value to type work as before. Keys follow their parameters through attachments.
#include "BinaryData.h"
#include "Crumb.h"

using namespace juce;

namespace
{
constexpr int C = 120, R = 34, W = 1024, H = 576, VX = 108, band = 12, dotCols = (VX - 2) * 2;
constexpr float cw = (float) W / C, ch = (float) H / R;
constexpr double pi = MathConstants<double>::pi;
const Colour page { 0xff0d0d0d }, white { 0xffececec }, grey { 0xff7a7a7a }, rule { 0xff2c2c2c }, dark { 0xff454545 }, red { 0xffff3d1f };
Colour fade(Colour c, bool on) { return on ? c : c.interpolatedWith(page, 0.6f); } // a stage that does nothing
String u(const char* s) { return String::fromUTF8(s); }
String minus(const String& s) { return s.replace("-", u("−")); }
Rectangle<int> cells(int x, int y, int w, int h) { return Rectangle<float>(cw * (float) x, ch * (float) y, cw * (float) w, ch * (float) h).toNearestInt(); }
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
    bool operator!=(const Cell& o) const { return c != o.c || fg != o.fg || bg != o.bg || bold != o.bold; }
};
struct Grid
{
    Cell at[R][C];
    void put(int x, int y, const String& s, Colour fg, bool bold = false, Colour bg = {})
    {
        for (auto p = s.getCharPointer(); !p.isEmpty(); ++x)
            if (const auto c = p.getAndAdvance(); isPositiveAndBelow(x, C) && isPositiveAndBelow(y, R))
                at[y][x] = { c, fg, bg, bold };
    }
    void centre(int x, int w, int y, const String& s, Colour fg, bool bold = false) { put(x + (w - s.length()) / 2, y, s.substring(0, w), fg, bold); }
    void right(int x, int y, const String& s, Colour fg, bool bold = false) { put(x - s.length() + 1, y, s, fg, bold); }
};

// Stages in display (= signal) order; filter pre swaps the filter with the empty dock. Knobs sit dx cells into theirs.
enum { In, Dock, Quant, Approx, Resample, Images, Filter, Mix, numStages };
constexpr int widths[] = { 14, 6, 16, 12, 14, 12, 20, 12 };
const char* const names[] = { "in", "", "quantize", "approx", "resample", "images", "filter", "mix" };
struct KnobSpec { const char* id; const char* name; int stage, dx; bool up, centred; };
const KnobSpec knobSpecs[] = { { "preamp", "preamp", In, 3, true, false },          { "dither", "dither", In, 3, false, false },
                               { "approxDev", "deviate", Approx, 2, false, true },   { "rate", "freq", Resample, 3, true, false },
                               { "jitter", "jitter", Resample, 3, false, false },    { "imagesShift", "shift", Images, 2, false, false },
                               { "cutoff", "cutoff", Filter, 1, true, false },       { "reso", "reso", Filter, 11, true, false },
                               { "mix", "fx", Mix, 2, true, false },                 { "output", "output", Mix, 2, false, false } };
const char* const types[] = { "off", "lp", "bp", "hp", "br" };
const char* const digits[10][4] = { { "█▀█", "█ █", "█ █", "▀▀▀" }, { "▗█ ", " █ ", " █ ", "▀▀▀" }, { "▀▀█", "▄▄█", "█  ", "▀▀▀" },
                                    { "▀▀█", "▄▄█", "  █", "▀▀▀" }, { "█ █", "█▄█", "  █", "  ▀" }, { "█▀▀", "█▄▄", "  █", "▀▀▀" },
                                    { "█▀▀", "█▄▄", "█ █", "▀▀▀" }, { "▀▀█", "  █", "  █", "  ▀" }, { "█▀█", "█▄█", "█ █", "▀▀▀" },
                                    { "█▀█", "█▄█", "  █", "▀▀▀" } };

// The processor's TPT state-variable filter (Crumb.cpp), for the editor's copy of the stage chain.
struct Svf
{
    double s1 = 0, s2 = 0, k = 2, a1 = 1, a2 = 0, a3 = 0;
    void set(double g, double damping) { k = damping, a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2; }
    std::array<double, 3> run(double x)
    {
        const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2 * v1 - s1, s2 = 2 * v2 - s2;
        return { v2, v1, x - k * v1 - v2 };
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

struct Editor : AudioProcessorEditor, Timer
{
    CrumbProcessor& proc;
    Look look;
    Component layer; // the controls, in design pixels
    Slider knobs[10];
    Key prev { "prev" }, next { "next" }, save { "save" }, slotA { "A" }, slotB { "B" }, minusKey { "minus" }, plus { "plus" }, dcShift { "dcShift" },
        approx { "approxOn" }, images { "imagesOn" }, pre { "pre" }, post { "post" }, dock { "dock" }, autoGain { "autoGain" }, typeKeys[5] { Key { "off" }, Key { "lp" }, Key { "bp" }, Key { "hp" }, Key { "br" } };
    // Attachments after the components they drive, so they are destroyed first.
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> knobAtt[10];
    std::vector<std::unique_ptr<ParameterAttachment>> keyAtt;
    std::vector<float> in = std::vector<float>(crumb::Scope::size), out = in, work = in, seg[numStages];
    int n = 16, start = 0;
    bool laidOutPre = false;
    float scopePeak = 0.1f, needle[2] {}, speed[2] {}, level[2] {}, peak[2] {};
    crumb::Scope::Meter seen; // the meter totals last read: opening the editor starts from the current position
    double stepAt = 0, heard = 0, peakAt = 0;
    bool clip[2] {};
    std::unique_ptr<Grid> shown = std::make_unique<Grid>(), building = std::make_unique<Grid>();
    Image image; // the shown grid in device pixels
    Font fonts[2] { mono(1, false), mono(1, true) };

    explicit Editor(CrumbProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        for (auto& s : seg)
            s.resize(crumb::Scope::size / 2);
        setOpaque(true), setLookAndFeel(&look);
        addAndMakeVisible(layer);
        layer.setBounds(0, 0, W, H);
        for (int i = 0; i < 10; ++i)
        {
            auto& k = knobs[i];
            const auto& s = knobSpecs[i];
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(s.up ? Slider::TextBoxBelow : Slider::TextBoxAbove, false, cells(0, 0, 10, 1).getWidth(), roundToInt(ch));
            k.setTitle(s.name), k.setComponentID(s.id), k.setRepaintsOnMouseActivity(false);
            knobAtt[i] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts, s.id, k);
            k.textFromValueFunction = [f = k.textFromValueFunction](double v) { return minus(f(v)); };
            k.updateText();
            auto* param = p.apvts.getParameter(s.id);
            k.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
            layer.addAndMakeVisible(k);
        }
        prev.onClick = [this] { proc.stepPreset(-1); };
        next.onClick = [this] { proc.stepPreset(1); };
        save.onClick = [this] { proc.saveUserPreset(); };
        slotA.onClick = [this] { proc.selectAb(0); };
        slotB.onClick = [this] { proc.selectAb(1); };
        for (auto [key, x, w] : { std::tuple { &prev, 30, 2 }, { &next, 62, 2 }, { &save, 67, 4 }, { &slotA, 74, 1 }, { &slotB, 76, 1 } })
            layer.addAndMakeVisible(*key), key->setBounds(cells(x, 1, w, 1));
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
            return keyAtt.back().get();
        };
        bind("dcShift", { &dcShift }), bind("approxOn", { &approx }), bind("imagesOn", { &images }), bind("autoGain", { &autoGain });
        bind("fType", { &typeKeys[0], &typeKeys[1], &typeKeys[2], &typeKeys[3], &typeKeys[4] });
        auto* order = bind("fOrder", { &pre, &post });
        dock.onClick = [this, order] { order->setValueAsCompleteGesture(preOrder() ? 1.0f : 0.0f); }; // the filter moves into the dock
        keyAtt.push_back(std::make_unique<ParameterAttachment>(*p.apvts.getParameter("bits"), [](float) {}));
        auto* bitsAtt = keyAtt.back().get();
        minusKey.onClick = [this, bitsAtt] { bitsAtt->setValueAsCompleteGesture((float) jmax(0, bits() - 1)); };
        plus.onClick = [this, bitsAtt] { bitsAtt->setValueAsCompleteGesture((float) jmin(24, bits() + 1)); };
        for (auto* key : { &minusKey, &plus, &dcShift, &approx, &images, &pre, &post, &dock, &autoGain, &typeKeys[0], &typeKeys[1], &typeKeys[2], &typeKeys[3], &typeKeys[4] })
            layer.addAndMakeVisible(*key);
        layout();

        const int stored = proc.editorWidth; // read first: setting the limits already resizes
        setResizable(true, false);
        setResizeLimits(W / 2, H / 2, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio((double) W / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, stored) : W;
        setSize(width, width * H / W);
        proc.scope.meter(seen), stepAt = heard = peakAt = proc.scope.clock();
        timerCallback();
        startTimerHz(30);
    }
    ~Editor() override { setLookAndFeel(nullptr); }

    float value(const char* id) const { return proc.apvts.getRawParameterValue(id)->load(); }
    String text(const char* id) const { return minus(proc.apvts.getParameter(id)->getCurrentValueAsText()); }
    int bits() const { return roundToInt(value("bits")); }
    bool preOrder() const { return value("fOrder") < 0.5f; }
    std::array<int, numStages> order() const { return { In, preOrder() ? Filter : Dock, Quant, Approx, Resample, Images, preOrder() ? Dock : Filter, Mix }; }
    std::array<int, numStages> stageX() const // first column of each stage
    {
        std::array<int, numStages> x {};
        int at = 2;
        for (auto s : order())
            x[(size_t) s] = at, at += widths[s];
        return x;
    }
    // Stages that currently do nothing are dimmed.
    bool active(int stage) const
    {
        return stage == Quant ? bits() > 0 : stage == Approx ? value("approxOn") > 0.5f : stage == Images ? value("imagesOn") > 0.5f : stage == Filter ? value("fType") > 0.5f : true;
    }
    void layout()
    {
        const auto x = stageX();
        for (int i = 0; i < 10; ++i)
            knobs[i].setBounds(cells(x[(size_t) knobSpecs[i].stage] + knobSpecs[i].dx - 1, knobSpecs[i].up ? 4 : 23, 10, 6));
        const int q = x[Quant], f = x[Filter];
        minusKey.setBounds(cells(q + 1, 5, 3, 4)), plus.setBounds(cells(q + 12, 5, 3, 4)), dcShift.setBounds(cells(q + 3, 25, 12, 1));
        approx.setBounds(cells(x[Approx] + 1, 6, 11, 1)), images.setBounds(cells(x[Images] + 1, 6, 11, 1));
        pre.setBounds(cells(f + 2, 27, 3, 2)), post.setBounds(cells(f + 6, 27, 4, 2)), dock.setBounds(cells(x[Dock] + 1, band - 1, 5, 12));
        autoGain.setBounds(cells(x[Mix] + 3, 30, 6, 1));
        for (int k = 0, tx = f + 2; k < 5; tx += (int) std::strlen(types[k]) + 1, ++k)
            typeKeys[k].setBounds(cells(tx, 24, (int) std::strlen(types[k]), 2));
        laidOutPre = preOrder();
    }
    void resized() override
    {
        layer.setTransform(AffineTransform::scale((float) getWidth() / W, (float) getHeight() / H));
        proc.editorWidth = getWidth();
    }

    void timerCallback() override
    {
        if (laidOutPre != preOrder())
            layout();
        // VU: the RMS of the audio since the last new block (held between blocks, silence after 0.5 s without audio)
        // drives a 2nd-order needle (99 % in 300 ms, 1.5 % overshoot) in 1/240 s steps of elapsed time, at most 1 s of
        // catch-up; 0 VU = -12 dBFS RMS (K-12). The clip light holds for 1 s after a sample over 0 dBFS.
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
        for (int c = 0; c < 2; ++c)
        {
            clip[c] = proc.scope.clipAt[c].load() + 1000 > t;
            peak[c] = jmax(vuRows(needle[c]), peak[c] - 12 * (float) (t - peakAt) / 1000); // peak marker falls 12 rows/s
            for (auto [id, v] : { std::pair { "vu" + String(c), var(needle[c]) }, { "clip" + String(c), var(clip[c]) } }) // for tests
                if (!getProperties().contains(id) || getProperties()[id] != v) getProperties().set(id, v);
        }
        peakAt = t;
        // The scope: 20 ms started on the last rising zero crossing of the input, so it stands still, 5 ms before the
        // newest sample (the zero-phase passes settle there).
        n = jlimit(16, crumb::Scope::size / 3, roundToInt(proc.sampleRate.load() * 0.02));
        const int m = 2 * n + n / 4;
        proc.scope.read(in.data(), out.data(), m);
        start = m - n - n / 4;
        for (int i = start; i > 1; --i)
            if (in[(size_t) i - 1] <= 0 && in[(size_t) i] > 0)
            {
                start = i;
                break;
            }
        refresh(true);
    }
    static float vuRows(float gain) // lit rows of 24: +3 VU fills the column, the scale marks are where opt5 puts them
    {
        const float db = Decibels::gainToDecibels(gain, -100.0f), stops[][2] { { -26, 0 }, { -20, 1 }, { -10, 8 }, { -7, 11 }, { -5, 14 }, { -3, 17 }, { 0, 21 }, { 3, 24 } };
        int s = 0;
        while (s < 6 && db > stops[s + 1][0])
            ++s;
        return jlimit(0.0f, 24.0f, stops[s][1] + (stops[s + 1][1] - stops[s][1]) * (db - stops[s][0]) / (stops[s + 1][0] - stops[s][0]));
    }

    // The stage chain as Crumb.cpp runs it, without its smoothing, ADAA, ladder errors, DC coupling or band-limited steps,
    // magnified so each stage's crush shows at braille resolution: the steps are counted on the screen (2^(bits/4 + 0.8)
    // levels across the band: 14 at 12 bit, 7 at 8 bit) and every rate and cutoff is divided by 16, as if the 20 ms
    // played 16 times faster. Cheap enough to rerun on the snapshot every frame. seg[s] = the output of stage s.
    void runChain()
    {
        constexpr double zoom = 16;
        const double fs = proc.sampleRate.load(), rate = jmin(fs, (double) value("rate")) / zoom, gain = Decibels::decibelsToGain((double) value("preamp"));
        const int resolution = bits();
        const double levels = std::exp2(resolution / 4.0 + 0.8), lsb = 2 / levels * scopePeak / 0.92, half = levels / 2;
        const double dither = value("dither") / 100.0, jitter = value("jitter") / 100.0, mix = value("mix") / 100.0;
        const double inc0 = rate / fs, jc = 1 - std::exp(-2 * pi * 30 * zoom / fs), jn = 0.5 / std::sqrt(jc / (3 * (2 - jc)));
        const double outGain = Decibels::decibelsToGain((double) value("output")), wet = value("autoGain") > 0.5f ? 1 / gain : 1;
        const bool tread = value("dcShift") > 0.5f, approxOn = value("approxOn") > 0.5f, imagesOn = value("imagesOn") > 0.5f, identity = value("rate") >= fs && jitter <= 0;
        const int type = roundToInt(value("fType"));
        const auto g = [fs](double hz) { return std::tan(pi * jmin(0.49 * fs, hz) / fs); };
        Svf approxF[4], imagesF[2], filter;
        for (int k = 0; k < 4; ++k)
            approxF[k].set(g(0.5 * rate * std::exp2(value("approxDev") / 100.0)), std::array { 1.9615705608, 1.6629392246, 1.1111404660, 0.3901806440 }[(size_t) k]);
        for (int k = 0; k < 2; ++k)
            imagesF[k].set(g(0.5 * rate * std::exp2(2 * value("imagesShift") / 100.0)), std::array { 1.8477590650, 0.7653668647 }[(size_t) k]);
        filter.set(g(value("cutoff") / zoom), 2 * std::pow(40.0, -value("reso") / 100.0));
        uint32 seed = 1;
        double phase = 0, held = 0, walk = 0;
        const auto random = [&seed] { return (double) ((seed = seed * 1664525u + 1013904223u) >> 8) * 0x1p-24; };
        const int length = start + n + n / 4;
        float* y = work.data();
        const auto zeroPhase = [&](const Svf* f, int sections, int shape) { // forward then backward: lined up with its input
            for (int pass = 0; pass < 2; ++pass)
                for (int k = 0; k < sections; ++k)
                {
                    Svf svf = f[k];
                    svf.s2 = y[pass == 0 ? 0 : length - 1]; // settled on the first sample
                    for (int i = 0; i < length; ++i)
                    {
                        auto& v = y[pass == 0 ? i : length - 1 - i];
                        const auto o = svf.run(v);
                        v = (float) (shape == 1 ? o[0] : shape == 2 ? svf.k * o[1] : shape == 3 ? o[2] : o[0] + o[2]); // lp bp hp br
                    }
                }
        };
        for (auto s : order())
        {
            if (s == In)
                for (int i = 0; i < length; ++i)
                {
                    const double a = jlimit(-64.0, 64.0, in[(size_t) i] * gain);
                    y[i] = (float) (std::abs(a) <= 0.5 ? a : std::copysign(0.5 + 0.5 * std::tanh((std::abs(a) - 0.5) / 0.5), a));
                }
            else if (s == Quant && resolution > 0)
                for (int i = 0; i < length; ++i)
                {
                    const double v = (y[i] + dither * lsb * (random() - 0.5)) / lsb;
                    y[i] = (float) ((jlimit(-half, half - 1, tread ? std::round(v) : std::floor(v)) + (tread ? 0 : 0.5)) * lsb);
                }
            else if (s == Approx && approxOn)
                zeroPhase(approxF, 4, 1);
            else if (s == Resample && !identity)
                for (int i = 0; i < length; ++i)
                {
                    walk += jc * (2 * random() - 1 - walk);
                    if (i == 0 || (phase += jmin(1.0, inc0 * (1 + 0.03 * jitter * jlimit(-1.0, 1.0, walk * jn)))) >= 1)
                        phase -= phase >= 1 ? 1 : 0, held = y[i];
                    y[i] = held;
                }
            else if (s == Images && imagesOn)
                zeroPhase(imagesF, 2, 1);
            else if (s == Filter && type > 0)
                zeroPhase(&filter, 1, type);
            else if (s == Mix)
                for (int i = 0; i < length; ++i)
                    y[i] = (float) ((in[(size_t) i] + mix * (y[i] * wet - in[(size_t) i])) * outGain);
            std::copy(y + start, y + start + n, seg[s].begin());
        }
    }

    // A round braille dial, 8 x 4 cells = 16 x 16 dots: a dotted 270 degree track, the value arc (centred: from the top)
    // and a two-dot-wide needle. Cells with track dots are grey, then arc red, then needle white (opt5's layering).
    void knob(Grid& g, int x, int y, float norm, const KnobSpec& s, const String& valueText, bool on)
    {
        uint8 track[4][8] {}, arc[4][8] {}, hand[4][8] {};
        const auto dot = [](uint8 (&m)[4][8], double px, double py) {
            const int ix = (int) px, iy = (int) py;
            if (isPositiveAndBelow(ix, 16) && isPositiveAndBelow(iy, 16)) m[iy / 4][ix / 2] |= brailleBit(ix % 2, iy % 4);
        };
        const double c = 7.5, r = 7.2, v = 270.0 * norm;
        for (int t = 0; t <= 270; t += 3)
        {
            const double a = degreesToRadians(225.0 + t);
            dot((s.centred ? jmin(135.0, v) <= t && t <= jmax(135.0, v) : t <= v) ? arc : track, c + r * std::sin(a), c - r * std::cos(a));
        }
        const double a = degreesToRadians(225 + v), sn = std::sin(a), cs = std::cos(a);
        for (auto [x0, y0, len] : { std::tuple { c, c, r - 2 }, { c + 0.5, c + 0.5, r - 2.5 } }) // opt5's dotline, Python rounding
        {
            const int steps = (int) jmax(std::abs(len * sn), std::abs(len * cs)) + 1;
            for (int k = 0; k < steps; ++k)
                dot(hand, std::nearbyint(x0 + len * sn * k / jmax(1, steps - 1)), std::nearbyint(y0 - len * cs * k / jmax(1, steps - 1)));
        }
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 8; ++i)
                if (const int b = track[j][i] | arc[j][i] | hand[j][i])
                    g.at[y + j][x + i] = { (juce_wchar) (0x2800 + b), fade(track[j][i] ? dark : arc[j][i] ? red : white, on), {} };
        g.centre(x - 1, 10, s.up ? y - 1 : y + 4, s.name, fade(grey, on));
        g.centre(x - 1, 10, s.up ? y + 4 : y - 1, valueText, fade(white, on), true);
    }

    void refresh(bool frame)
    {
        runChain();
        Grid& g = *building;
        g = {};
        const auto x = stageX();
        const bool preFilter = preOrder();
        // Header: name, preset number and name with prev / next, save, A / B, maker.
        g.put(2, 1, "crumb", white, true);
        g.put(30, 1, u("‹"), grey), g.put(63, 1, u("›"), grey), g.put(67, 1, "save", grey);
        g.put(33, 1, proc.preset >= 0 ? String(proc.preset.load()).paddedLeft('0', 2) : String("--"), grey);
        g.centre(36, 26, 1, proc.presetName(), white, true);
        for (int s = 0; s < 2; ++s)
            g.put(74 + 2 * s, 1, s == 0 ? "A" : "B", proc.abSlot() == s ? page : grey, proc.abSlot() == s, proc.abSlot() == s ? white : Colour());
        g.right(C - 3, 1, "daveydidit", dark);
        g.put(2, 2, String::repeatedString(u("─"), C - 4), rule), g.put(2, R - 3, String::repeatedString(u("─"), C - 4), rule);
        // Hairlines between the stages (dashed around the dock) and before the meters; stage names on the band's edge.
        auto o = order();
        for (int k = 1; k <= numStages; ++k)
            for (int j = 3; j < R - 3; ++j)
                g.put(k < numStages ? x[(size_t) o[(size_t) k]] : VX, j, u(k < numStages && (o[(size_t) k] == Dock || o[(size_t) k - 1] == Dock) ? "┆" : "│"), rule);
        for (int s = 0; s < numStages; ++s)
            g.put(x[(size_t) s] + 1, band - 1, names[s], fade(dark, active(s)));
        g.centre(x[Dock], widths[Dock], band - 1, preFilter ? "post" : "pre", dark);
        for (int i = 0; i < 10; ++i)
        {
            const auto& s = knobSpecs[i];
            knob(g, x[(size_t) s.stage] + s.dx, s.up ? 5 : 24, proc.apvts.getParameter(s.id)->getValue(), s, text(s.id), active(std::strcmp(s.id, "dither") == 0 ? Quant : s.stage));
        }
        // quantize: the resolution in block numerals, - / + to step it; dc shift.
        const int q = x[Quant], b = bits();
        g.centre(q, 16, 4, "resolution", grey);
        g.put(q + 2, 6, u("−"), grey, true), g.put(q + 13, 6, "+", grey, true);
        if (b > 0)
        {
            const String digitsText(b);
            for (int k = 0; k < digitsText.length(); ++k)
                for (int j = 0; j < 4; ++j)
                    g.put(q + (b < 10 ? 7 : 5 + 4 * k), 5 + j, u(digits[digitsText[k] - '0'][j]), white);
            g.put(q + 5, 9, "bit", grey);
        }
        else
            g.centre(q, 16, 6, "off", grey);
        const auto toggle = [&](int tx, int ty, const char* id, const String& name, bool on) { g.put(tx, ty, u(value(id) > 0.5f ? "■" : "□"), fade(value(id) > 0.5f ? red : grey, on)), g.put(tx + 2, ty, name, value(id) > 0.5f ? fade(white, on) : fade(grey, on), value(id) > 0.5f); };
        toggle(q + 4, 25, "dcShift", "dc shift", b > 0);
        toggle(x[Approx] + 2, 6, "approxOn", "approx", true), toggle(x[Images] + 2, 6, "imagesOn", "images", true);
        toggle(x[Mix] + 3, 30, "autoGain", "auto", true); // output follows the preamp down
        // filter: type words (the selected one red, underlined), pre / post.
        const int f = x[Filter], type = roundToInt(value("fType"));
        for (int k = 0, tx = f + 2; k < 5; tx += (int) std::strlen(types[k]) + 1, ++k)
        {
            const auto colour = k == type ? (k == 0 ? white : red) : grey;
            g.put(tx, 24, types[k], colour, k == type);
            if (k == type) g.put(tx, 25, String::repeatedString(u("▔"), (int) std::strlen(types[k])), colour);
        }
        g.put(f + 2, 27, "pre", preFilter ? white : grey, preFilter), g.put(f + 6, 27, "post", preFilter ? grey : white, !preFilter);
        g.put(preFilter ? f + 2 : f + 6, 28, String::repeatedString(u("▔"), preFilter ? 3 : 4), white);

        // The wave: dot column k shows the stage that owns its cell, so the signal is crushed stage by stage across the
        // panel and turns red at mix. The input stage's output stays as a grey ghost wherever the wave left it.
        int stageOf[C] {};
        for (int s = 0; s < numStages; ++s)
            for (int c = x[(size_t) s]; c < x[(size_t) s] + widths[s]; ++c) stageOf[c] = s;
        float range[2][dotCols][2] {}; // [wave, ghost][column][lo, hi]
        float top = 0;
        for (int w = 0; w < 2; ++w)
            for (int k = 0; k < dotCols; ++k)
            {
                const auto& v = seg[w == 0 ? stageOf[2 + k / 2] : In];
                const int a = k * n / dotCols, e = jmax(a + 1, (k + 1) * n / dotCols);
                float lo = v[(size_t) a], hi = lo;
                if (k > 0) // joined to the previous column's last sample
                    lo = jmin(lo, seg[w == 0 ? stageOf[2 + (k - 1) / 2] : In][(size_t) a - 1]), hi = jmax(hi, seg[w == 0 ? stageOf[2 + (k - 1) / 2] : In][(size_t) a - 1]);
                for (int i = a; i < e; ++i)
                    lo = jmin(lo, v[(size_t) i]), hi = jmax(hi, v[(size_t) i]);
                range[w][k][0] = lo, range[w][k][1] = hi, top = jmax(top, -lo, hi);
            }
        if (frame)
            scopePeak = jmax(0.05f, top, scopePeak * 0.95f); // auto gain: fast up, slow down
        uint8 dots[2][2][10][VX - 2] {}; // [wave, ghost][red][row][column]
        for (int w = 0; w < 2; ++w)
            for (int k = 0; k < dotCols; ++k)
            {
                const auto row = [this](float v) { return (int) std::nearbyint((1 - (jlimit(-1.1f, 1.1f, v / scopePeak * 0.92f) + 1) / 2) * 39); };
                for (int py = row(range[w][k][1]); py <= row(range[w][k][0]); ++py)
                    if (isPositiveAndBelow(py, 40))
                        dots[w][w == 0 && stageOf[2 + k / 2] == Mix][py / 4][k / 2] |= brailleBit(k % 2, py % 4);
            }
        for (int j = 0; j < 10; ++j)
            for (int i = 0; i < VX - 2; ++i)
                if (const int wave = dots[0][0][j][i] | dots[0][1][j][i])
                    g.at[band + j][2 + i] = { (juce_wchar) (0x2800 + wave), dots[0][0][j][i] ? white : red, {} };
                else if (dots[1][0][j][i])
                    g.at[band + j][2 + i] = { (juce_wchar) (0x2800 + dots[1][0][j][i]), dark, {} };

        // Meters: L / R columns from 20 to +3 VU (red from 0), eighth-cell tops, a falling peak mark, clip lights.
        const std::pair<int, const char*> marks[] { { 0, "+3" }, { 3, "0" }, { 7, "3" }, { 10, "5" }, { 13, "7" }, { 16, "10" }, { 23, "20" } };
        for (auto [j, t] : marks)
            g.right(VX + 6, 4 + j, t, j < 4 ? red : grey);
        for (int c = 0; c < 2; ++c)
        {
            const int cx = VX + 3 + 5 * c;
            const float lit = vuRows(needle[c]);
            g.put(cx, 3, u(clip[c] ? "●" : "○"), clip[c] ? red : dark);
            for (int j = 0; j < 24; ++j)
            {
                const int eighths = jlimit(0, 8, (int) std::floor((lit - (float) (23 - j)) * 8));
                g.put(cx, 4 + j, eighths > 0 ? String::charToString((juce_wchar) (0x2580 + eighths)) : u("│"), eighths > 0 ? (j < 4 ? red : white) : rule);
            }
            if (peak[c] >= 0.5f)
                g.put(cx, 28 - roundToInt(peak[c]), u("▬"), 24 - roundToInt(peak[c]) < 4 ? red : white);
            g.put(cx, 28, c == 0 ? "L" : "R", grey);
        }
        g.centre(VX + 1, 11, 30, text("output"), white, true);

        // Status line: rate, resolution, then each stage's setting; the scope span and the version.
        const float rate = value("rate");
        int at = 2;
        const auto word = [&](const String& s, Colour colour, bool bold = false) { g.put(at, R - 2, s, colour, bold), at += s.length() + 1; };
        word(rate >= 1000 ? String(rate / 1000, 1) : String(roundToInt(rate)), white, true), word(rate >= 1000 ? "khz" : "hz", grey);
        b > 0 ? (word(String(b), white, true), word("bit", grey)) : word("off", white, true);
        const String dot = u(" · ");
        g.put(19, R - 2, String(value("dcShift") > 0.5f ? "mid-tread" : "mid-riser") + dot + "dither " + text("dither") + dot
                             + (value("approxOn") > 0.5f ? "approx " + text("approxDev") : String("approx off")) + dot
                             + (value("imagesOn") > 0.5f ? "images +" + text("imagesShift") : String("images off")) + dot
                             + (type > 0 ? String(types[type]) + " " + text("cutoff") + (preFilter ? " pre" : " post") : String("filter off"))
                             + (value("autoGain") > 0.5f ? dot + "auto " + (value("preamp") > 0 ? u("−") + text("preamp").substring(1) : text("preamp")) : String()), dark);
        g.right(VX - 2, R - 2, "20 ms", dark), g.right(C - 3, R - 2, "v" JucePlugin_VersionString, dark);
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
                if (i < C && g.at[j][i] != shown->at[j][i])
                {
                    shown->at[j][i] = g.at[j][i], first = first < 0 ? i : first, last = i;
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
    // One cell in device pixels. Cells are snapped to whole pixels, so lines and blocks join without seams.
    void drawCell(Graphics& g, int i, int j)
    {
        const auto& c = shown->at[j][i];
        const auto px = [this](int k) { return roundToInt((float) k * (float) image.getWidth() / C); };
        const auto py = [this](int k) { return roundToInt((float) k * (float) image.getHeight() / R); };
        const auto r = Rectangle<int>::leftTopRightBottom(px(i), py(j), px(i + 1), py(j + 1));
        g.setColour(c.bg.isTransparent() ? page : c.bg), g.fillRect(r);
        if (c.c == ' ') return;
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r), g.setColour(c.fg); // nothing spills into a neighbour, which may be redrawn alone
        const float w = (float) image.getWidth() / C, h = (float) image.getHeight() / R, x = (float) r.getX(), y = (float) r.getY();
        const float cx = x + (float) r.getWidth() / 2, cy = y + (float) r.getHeight() / 2, lw = jmax(1.0f, w * 0.12f);
        const int t = jmax(1, roundToInt(lw));
        const auto block = [&](float l, float tp, float rt, float bt) {
            g.fillRect(Rectangle<int>::leftTopRightBottom(roundToInt(x + l * (float) r.getWidth()), roundToInt(y + tp * (float) r.getHeight()),
                                                          roundToInt(x + rt * (float) r.getWidth()), roundToInt(y + bt * (float) r.getHeight())));
        };
        if (c.c == 0x2500) g.fillRect(r.getX(), roundToInt(cy - (float) t / 2), r.getWidth(), t); // ─
        else if (c.c == 0x2502) g.fillRect(roundToInt(cx - (float) t / 2), r.getY(), t, r.getHeight()); // │
        else if (c.c == 0x2506) // ┆
            for (int k = 0; k < 3; ++k) g.fillRect(Rectangle<float>((float) roundToInt(cx - (float) t / 2), y + h * ((float) k + 0.2f) / 3, (float) t, h * 0.2f));
        else if (c.c >= 0x2581 && c.c <= 0x2588) block(0, 1 - (float) (c.c - 0x2580) / 8, 1, 1); // ▁ .. █
        else if (c.c == 0x2580) block(0, 0, 1, 0.5f); // ▀
        else if (c.c == 0x2594) block(0, 0, 1, 0.125f); // ▔
        else if (c.c == 0x2597) block(0.5f, 0.5f, 1, 1); // ▗
        else if (c.c >= 0x2800 && c.c <= 0x28ff) // braille
        {
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                    if ((c.c - 0x2800) & brailleBit(dx, dy))
                        g.fillEllipse(Rectangle<float>(0.3f * w, 0.3f * w).withCentre({ x + (float) r.getWidth() * (0.3f + 0.4f * (float) dx), y + (float) r.getHeight() * (0.125f + 0.25f * (float) dy) }));
        }
        else if (c.c == 0x25a0 || c.c == 0x25ac) g.fillRect(Rectangle<float>(c.c == 0x25a0 ? 0.72f * w : w, c.c == 0x25a0 ? 0.36f * h : 0.22f * h).withCentre({ cx, cy })); // ■ ▬
        else if (c.c == 0x25a1) g.drawRect(Rectangle<float>(0.72f * w, 0.36f * h).withCentre({ cx, cy }), lw); // □
        else if (c.c == 0x25cf) g.fillEllipse(Rectangle<float>(0.72f * w, 0.72f * w).withCentre({ cx, cy })); // ●
        else if (c.c == 0x25cb) g.drawEllipse(Rectangle<float>(0.72f * w - lw, 0.72f * w - lw).withCentre({ cx, cy }), lw); // ○
        else if (c.c == 0x2039 || c.c == 0x203a) // ‹ › (not in Geist Mono)
        {
            const float d = c.c == 0x2039 ? 1.0f : -1.0f;
            Path p;
            p.startNewSubPath(cx + d * 0.14f * w, cy - 0.14f * h), p.lineTo(cx - d * 0.14f * w, cy + 0.02f * h), p.lineTo(cx + d * 0.14f * w, cy + 0.18f * h);
            g.strokePath(p, PathStrokeType(lw));
        }
        else if (const auto glyph = fonts[c.bold ? 1 : 0].getTypefacePtr()->getNominalGlyphForCodepoint(c.c))
        {
            const float size = w / 0.6f; // Geist Mono: advance 0.6 em, line box ascent 1.005 + descent 0.295 em
            const uint16_t id = (uint16_t) *glyph;
            const Point<float> at { x + ((float) r.getWidth() - w) / 2, y + (h - 1.3f * size) / 2 + 1.005f * size };
            g.setFont(fonts[c.bold ? 1 : 0]);
            g.getInternalContext().drawGlyphs({ &id, 1 }, { &at, 1 }, {});
        }
    }
};
} // namespace

AudioProcessorEditor* CrumbProcessor::createEditor() { return new Editor(*this); }
