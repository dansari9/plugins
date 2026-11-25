// Vibrate's approved 132 x 47 cell layout, rendered using the family's grid, dials and preset list.
#include "BinaryData.h"
#include "Vibrate.h"
#include "Lanes.h"
#include "PresetMenu.h"
#include <cstring>
#include <optional>

using namespace juce;

namespace
{
constexpr int C = 132, R = 47, W = 1056, H = 752;
constexpr float cw = 8, ch = 16;
const Colour page { 0xff0d0d0d }, primary { 0xffececec }, muted { 0xff7a7a7a }, rule { 0xff2a2a2a },
             line { 0xff2c2c2c }, dim { 0xff454545 }, accent { 0xffff3d1f },
             cream { 0xffefe6d2 }, tang { 0xffff6a2f };
using vibrate::brailleBit;
Rectangle<int> cells(int x, int y, int w, int h) { return { x * (int) cw, y * (int) ch, w * (int) cw, h * (int) ch }; }
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size));
}

struct Cell
{
    juce_wchar c = ' ';
    Colour fg, bg; // bg transparent: the paper
    bool bold = false;
    uint8 edge = 0; // 1: a rule through the middle, 2: on the top edge, 3: down the middle
    juce_wchar left = 0; // nonzero: half-cell centring, with the previous glyph's right half clipped to this cell
    bool fixedInk = false;
    bool operator!=(const Cell& o) const { return c != o.c || fg != o.fg || bg != o.bg || bold != o.bold || edge != o.edge || left != o.left || fixedInk != o.fixedInk; }
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
    void right(int x, int y, const String& s, Colour fg, bool bold = false) { put(x - s.length() + 1, y, s, fg, bold); }
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

struct Dial : Slider
{
    bool keyPressed(const KeyPress& key) override
    {
        const int code = key.getKeyCode();
        if (code == KeyPress::returnKey)
        {
            showTextBox();
            return true;
        }
        const bool up = code == KeyPress::upKey || code == KeyPress::rightKey;
        const bool down = code == KeyPress::downKey || code == KeyPress::leftKey;
        if (up || down)
        {
            const double step = key.getModifiers().isShiftDown() ? .001 : .01;
            setValue(getValue() + (up ? step : -step), sendNotificationSync);
            return true;
        }
        if (code == KeyPress::homeKey || code == KeyPress::endKey)
        {
            setValue(code == KeyPress::homeKey ? 0.0 : 1.0, sendNotificationSync);
            return true;
        }
        return Slider::keyPressed(key);
    }
};

struct Plates
{
    Image ink = ImageFileFormat::loadFrom(BinaryData::vibrateink_png, BinaryData::vibrateink_pngSize);
    Image paper = ImageFileFormat::loadFrom(BinaryData::vibratepaper_png, BinaryData::vibratepaper_pngSize);
};

const Plates& plates()
{
    static const Plates value;
    return value;
}

struct Editor final : AudioProcessorEditor, Timer
{
    VibrateProcessor& proc;
    Look look;
    Component layer;
    std::array<Dial, vibrate::numControls> knobs;
    std::array<bool, vibrate::numControls> dragging {};
    Key prev { "prev" }, next { "next" }, name { "preset" }, slotA { "A" }, slotB { "B" };
    PresetMenu menu;
    vibrate::Lanes lanes;
    bool showingTrace = false;
    vibrate::Controls drawnValues {};
    int drawnSlot = -1;
    bool prevHover = false, nextHover = false;
    String drawnName;
    Grid shown;
    Image image, inkPlate, paperPlate;
    std::vector<uint8> dotMasks[4];
    Font fonts[2] { mono(1, false), mono(1, true) };

    explicit Editor(VibrateProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setOpaque(true);
        setLookAndFeel(&look);
        addAndMakeVisible(layer);
        layer.setBounds(0, 0, W, H);
        for (std::size_t i = 0; i < knobs.size(); ++i)
        {
            auto& dial = knobs[i];
            const auto id = vibrate::ids[i];
            dial.setSliderStyle(Slider::RotaryVerticalDrag);
            dial.setTextBoxStyle(Slider::TextBoxBelow, false, 80, 16);
            dial.setTitle(id);
            dial.setComponentID(id);
            dial.setWantsKeyboardFocus(true);
            dial.setRepaintsOnMouseActivity(false);
            dial.setRange(0, 1, .001);
            dial.textFromValueFunction = [this, i](double value) { return parameter(i)->getText(static_cast<float>(value), 0); };
            dial.valueFromTextFunction = [this, i](const String& text) { return parameter(i)->getValueForText(text); };
            dial.setDoubleClickReturnValue(true, vibrate::defaults[i]);
            dial.setValue(value(i), dontSendNotification);
            dial.onDragStart = [this, i] {
                dragging[i] = true;
                parameter(i)->beginChangeGesture();
            };
            dial.onDragEnd = [this, i] {
                parameter(i)->endChangeGesture();
                dragging[i] = false;
            };
            dial.onValueChange = [this, i] {
                auto* param = parameter(i);
                const auto n = static_cast<float>(knobs[i].getValue());
                if (std::abs(param->getValue() - n) <= 1e-7f) return;
                if (!dragging[i]) param->beginChangeGesture();
                param->setValueNotifyingHost(n);
                if (!dragging[i]) param->endChangeGesture();
            };
            dial.setBounds(cells(115, vibrate::dialRows[i] - 1, 10, 6));
            layer.addAndMakeVisible(dial);
        }
        prev.onClick = [this] { proc.stepPreset(-1); };
        next.onClick = [this] { proc.stepPreset(1); };
        name.onClick = [this] { showPresets(); };
        slotA.onClick = [this] { proc.selectAb(0); };
        slotB.onClick = [this] { proc.selectAb(1); };
        prev.onStep = next.onStep = [this](int by) { proc.stepPreset(by); };
        for (auto [key, x, width] : { std::tuple { &prev, 11, 3 }, { &next, 38, 3 }, { &name, 14, 24 },
                                     { &slotA, 43, 1 }, { &slotB, 45, 1 } })
        {
            layer.addAndMakeVisible(*key);
            key->setBounds(cells(x, 1, width, 1));
        }
        prev.setTitle("previous preset");
        next.setTitle("next preset");
        name.setTitle("presets");
        layer.addChildComponent(menu);
        menu.font = [](float size, bool bold) { return mono(size, bold); };
        menu.onChoose = [this](int id) {
            if (id == saveId) proc.saveUserPreset();
            else if (id >= userId) proc.loadUserPreset(listed[id - userId]);
            else proc.setCurrentProgram(id);
        };

        const int stored = proc.editorWidth;
        setResizable(true, false);
        setResizeLimits(W / 2, H / 2, W * 2, H * 2);
        getConstrainer()->setFixedAspectRatio(static_cast<double>(W) / H);
        const int width = stored > 0 ? jlimit(W / 2, W * 2, stored) : W;
        setSize(width, roundToInt(width * static_cast<double>(H) / W));
        proc.feed.discard();
        timerCallback();
        startTimerHz(60);
    }

    ~Editor() override
    {
        stopTimer();
        setLookAndFeel(nullptr);
    }

    RangedAudioParameter* parameter(std::size_t i) const { return proc.apvts.getParameter(vibrate::ids[i]); }
    double value(std::size_t i) const { return static_cast<double>(proc.apvts.getRawParameterValue(vibrate::ids[i])->load()) / 100.0; }

    static constexpr int userId = 1000, saveId = 999;
    StringArray listed;

    void showPresets()
    {
        if (menu.isVisible())
            return menu.close();
        std::vector<PresetMenu::Group> groups;
        for (const auto& group : vibrate::presetGroups)
        {
            groups.push_back({ group.name, {} });
            for (int i : group.programs)
                groups.back().items.push_back({ proc.getProgramName(i), i });
        }
        listed = proc.userPresets();
        groups.push_back({ "user", {} });
        for (int k = 0; k < listed.size(); ++k)
            groups.back().items.push_back({ listed[k], userId + k });
        groups.back().items.push_back({ "save", saveId });
        const int user = listed.indexOf(proc.presetName());
        const auto factory = proc.preset.load();
        menu.open(std::move(groups), factory >= 0 ? factory : user >= 0 ? userId + user : -1, { 3, 3, C - 6, R - 5 });
    }

    void resized() override
    {
        layer.setTransform(AffineTransform::scale(static_cast<float>(getWidth()) / W, static_cast<float>(getHeight()) / H));
        proc.editorWidth = getWidth();
    }

    void timerCallback() override
    {
        bool changed = false;
        const bool timedOut = proc.clockMs() - proc.feed.lastBlockMs.load(std::memory_order_acquire) > 150.0;
        if (timedOut)
        {
            proc.feed.discard();
            if (showingTrace)
            {
                lanes.clear();
                changed = true;
                showingTrace = false;
            }
        }
        else
        {
            vibrate::Frame frame;
            // Fence the catch-up work even if the audio thread writes while the timer drains.
            for (uint32_t i = 0; i < vibrate::Feed::capacity && proc.feed.pop(frame); ++i)
            {
                changed |= lanes.add(frame);
                showingTrace = true;
            }
        }

        vibrate::Controls values;
        for (std::size_t i = 0; i < knobs.size(); ++i)
        {
            values[i] = value(i);
            if (!knobs[i].isMouseButtonDown() && std::abs(values[i] - knobs[i].getValue()) > 1e-6)
                knobs[i].setValue(values[i], dontSendNotification);
        }
        const auto preset = proc.presetName();
        const int slot = proc.abSlot();
        const bool prevOver = prev.isMouseOver(), nextOver = next.isMouseOver();
        if (!changed && values == drawnValues && preset == drawnName && slot == drawnSlot
            && prevOver == prevHover && nextOver == nextHover)
            return;
        drawnValues = values;
        drawnName = preset;
        drawnSlot = slot;
        prevHover = prevOver;
        nextHover = nextOver;
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
    static String signedValue(double value, int decimals)
    {
        const double scale = decimals == 1 ? 10.0 : 100.0;
        const double rounded = std::round(value * scale) / scale;
        return String(rounded < 0 ? "-" : "+") + String(std::abs(rounded), decimals);
    }

    void refresh()
    {
        Grid g;
        g.put(3, 1, "vibrate", primary, true);
        g.put(12, 1, "<", prevHover ? primary : muted);
        g.centre(14, 24, 1, drawnName, primary, true);
        g.put(39, 1, ">", nextHover ? primary : muted);
        for (int k = 0; k < 2; ++k)
            g.put(43 + 2 * k, 1, k == 0 ? "A" : "B", drawnSlot == k ? page : dim, drawnSlot == k, drawnSlot == k ? primary : Colour());
        g.right(128, 1, "daveydidit", dim);
        g.edge(3, 129, 3, 2);
        for (int y = 4; y <= 44; ++y)
            g.put(109, y, String::charToString(0x2502), rule);
        g.edge(3, 129, 45, 1);

        const auto& info = lanes.readout();
        const auto sideOf = [](double value) { return String(value < 0 ? "R " : "L "); };
        const double gapSamples = std::abs(info.gapMs) * proc.sampleRate.load() / 1000.0;
        const String gap = std::abs(info.gapMs) >= 0.1 ? String(std::abs(info.gapMs), 2) + " ms" : String(roundToInt(gapSamples)) + " smp";
        const std::array<std::array<String, 2>, vibrate::numControls> readouts {{
            { signedValue(info.cents[0], 1) + " c", signedValue(info.cents[1], 1) + " c" },
            { sideOf(info.panDb) + String(std::abs(info.panDb), 1) + " dB", {} },
            { sideOf(info.gapMs) + gap, {} },
            { String(info.hz[0] / 1000.0, 1) + "k", String(info.hz[1] / 1000.0, 1) + "k" },
            { String(roundToInt(info.sideFraction * 100)) + " %", {} },
            { signedValue(info.correlation, 2), String(roundToInt(info.mixFraction * 100)) + " %" },
        }};

        for (std::size_t lane = 0; lane < knobs.size(); ++lane)
        {
            const int origin = vibrate::dialRows[lane];
            if (lane != 0)
                g.edge(3, 108, origin - 2, 1);
            const auto plot = lanes.plot(lane);
            for (int y = 0; y < vibrate::plotRows; ++y)
                for (int x = 0; x < vibrate::plotWidth; ++x)
                {
                    const auto& cell = plot[static_cast<std::size_t>(y * vibrate::plotWidth + x)];
                    if (cell.dots != 0)
                        g.at[origin - 1 + y][vibrate::plotX + x] = {
                            static_cast<juce_wchar>(0x2800 + cell.dots), Colour(0xff000000u | cell.ink), {}, false, 0, 0, true
                        };
                }
            if ((info.active & (1u << lane)) != 0)
            {
                if (lane == vibrate::mix)
                    g.right(vibrate::readoutX, origin, "corr", muted);
                const auto color = Colour(0xff000000u | vibrate::Lanes::tint(vibrate::inks[lane], 0xe8dfcd, 0.28));
                for (std::size_t i = 0; i < readouts[lane].size(); ++i)
                {
                    const auto& text = readouts[lane][i];
                    g.right(vibrate::readoutX, origin + 1 + static_cast<int>(i), text, color, true);
                }
            }
            knob(g, 116, origin, drawnValues[lane], vibrate::ids[lane], parameter(lane)->getCurrentValueAsText(), false);
        }
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
        inkPlate = nearest(from.ink);
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
    Ink inkOf(Colour c, bool fixed = false) const
    {
        if (fixed) return { nullptr, 1, c };
        const int r = c.getRed(), g = c.getGreen(), b = c.getBlue();
        const float lum = (float) (r + g + b) / 3, t = jlimit(0.0f, 1.0f, (lum - 13) / 223);
        if (r - g > 40 && r - b > 30) return { &inkPlate, jlimit(0.0f, 1.0f, (float) (r - 13) / 242), {} };
        return { nullptr, 1, lum > 150 ? page.interpolatedWith(cream, t) : c };
    }
    // A cell of paper (bare: of the plain page) and dots (a mask layer per bit, one byte of coverage per pixel), straight into the image.
    void printDots(Rectangle<int> r, const std::vector<uint8>& masks, int bits, const Ink& ink, bool bare)
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
            if (bare)
                for (int x = 0; x < w; ++x) reinterpret_cast<PixelRGB*>(to.getPixelPointer(x, y))->setARGB(255, page.getRed(), page.getGreen(), page.getBlue());
            else std::memcpy(to.getLinePointer(y), paper.getLinePointer(y), (size_t) (w * to.pixelStride));
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
        const bool braille = c.c >= 0x2800 && c.c <= 0x28ff, shade = c.c == 0x2591, bare = c.bg == page, plain = c.edge == 0 && (bare || c.bg.isTransparent());
        if (plain && (braille || shade || (c.c == ' ' && c.left <= ' ')))
        {
            const int s = (r.getWidth() > image.getWidth() / C ? 1 : 0) + (r.getHeight() > image.getHeight() / R ? 2 : 0);
            int bits = shade ? 1 << 8 : 0;
            for (int q = 0; q < 8; ++q)
                if (braille && ((c.c - 0x2800) & brailleBit(q % 2, q / 2))) bits |= 1 << q;
            return printDots(r, dotMasks[s], bits, inkOf(c.fg, c.fixedInk), bare);
        }
        Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r); // nothing spills into a neighbour, which may be redrawn alone
        g.drawImageAt(paperPlate, 0, 0);
        if (!c.bg.isTransparent()) g.setColour(inkOf(c.bg).colour), g.fillRect(r);
        const float w = (float) image.getWidth() / C, h = (float) image.getHeight() / R, x = (float) r.getX(), y = (float) r.getY();
        const float cx = x + (float) r.getWidth() / 2, lw = jmax(1.0f, w * 0.12f);
        const int t = jmax(1, roundToInt(lw));
        const int thin = jmax(1, roundToInt((float) image.getWidth() / (float) jmax(1, getWidth())));
        if (c.edge == 3) g.setColour(line), g.fillRect(roundToInt(x + (float) (r.getWidth() - thin) / 2), r.getY(), thin, r.getHeight());
        else if (c.edge != 0)
            g.setColour(line), g.fillRect(r.getX(), c.edge == 2 ? r.getY() : roundToInt(y + (float) r.getHeight() / 2), r.getWidth(), thin);
        if (c.c == ' ' && c.left <= ' ') return;
        const auto ink = inkOf(c.fg, c.fixedInk);
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
        if (c.c == 0x2502)
            g.fillRect(roundToInt(cx - static_cast<float>(t) / 2), r.getY(), t, r.getHeight());
        else glyph(c.c, 0);
    }
};
} // namespace

AudioProcessorEditor* VibrateProcessor::createEditor() { return new Editor(*this); }
