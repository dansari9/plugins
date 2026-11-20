#include "PluginProcessor.h"
#include "BinaryData.h"

using namespace juce;

// Minimal interim editor (the full one-screen UI arrives later): the master shaper, Magnitude and Output.
namespace
{
const Colour face { 0xffd9d8d2 }, ink { 0xff1a1a18 }, sub { 0xff6e6d68 }, screen { 0xff0f100f }, key { 0xffecebe6 },
    orange { 0xffff5a1f }, blue { 0xff2f6bff }, grey { 0xff9a9993 }, white { 0xfff6f5f0 }, dim { 0xff7a7a74 };

struct Look : LookAndFeel_V4
{
    Typeface::Ptr mono = Typeface::createSystemTypefaceFor(BinaryData::GeistMonoMedium_ttf, BinaryData::GeistMonoMedium_ttfSize);
    Font font(float height) const { return FontOptions(mono).withHeight(height); }
    Font getLabelFont(Label&) override { return font(13); }
    // Flat encoder: a light ring, a coloured cap and an indicator dot.
    void drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s) override
    {
        const auto r = Rectangle<int>(x, y, w, h).toFloat().withSizeKeepingCentre(60, 60);
        g.setColour(Colours::black.withAlpha(0.08f)), g.fillEllipse(r.translated(0, 2));
        g.setColour(key), g.fillEllipse(r);
        g.setColour(s.findColour(Slider::thumbColourId)), g.fillEllipse(r.reduced(12));
        g.setColour(ink), g.fillEllipse(Rectangle<float>(6, 6).withCentre(r.getCentre().getPointOnCircumference(21, a0 + pos * (a1 - a0))));
    }
};

struct Editor : AudioProcessorEditor, Timer
{
    static constexpr const char* ids[] = { "attack", "sustain", "magnitude", "out_gain" };
    static constexpr const char* names[] = { "ATTACK", "SUSTAIN", "MAGNITUDE", "OUTPUT" };
    SnapProcessor& proc;
    Look look;
    Slider knobs[4];
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> attachments[4];
    const Rectangle<float> display { 16, 52, 488, 216 };

    explicit Editor(SnapProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setLookAndFeel(&look);
        const Colour caps[] = { orange, blue, grey, white };
        for (int i = 0; i < 4; ++i)
        {
            auto& k = knobs[i];
            k.setSliderStyle(Slider::RotaryVerticalDrag), k.setTextBoxStyle(Slider::TextBoxBelow, false, 100, 18);
            k.setColour(Slider::thumbColourId, caps[i]), k.setColour(Slider::textBoxTextColourId, ink);
            k.setColour(Slider::textBoxOutlineColourId, {}), k.setTitle(names[i]);
            attachments[i] = std::make_unique<AudioProcessorValueTreeState::SliderAttachment>(p.apvts, ids[i], k);
            auto* param = p.apvts.getParameter(ids[i]);
            k.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));
            k.setBounds(16 + i * 122, 280, 122, 84);
            addAndMakeVisible(k);
        }
        setSize(520, 392);
        startTimerHz(30);
    }
    ~Editor() override { setLookAndFeel(nullptr); }
    void timerCallback() override { repaint(display.toNearestInt()); }

    void text(Graphics& g, const String& s, float size, Colour c, Rectangle<float> r, Justification j = Justification::left)
    {
        g.setColour(c), g.setFont(look.font(size)), g.drawText(s, r, j);
    }

    void paint(Graphics& g) override
    {
        g.fillAll(face);
        text(g, "snap", 24, ink, { 20, 14, 80, 28 });
        text(g, String(CharPointer_UTF8("drum multi-fx \xc2\xb7 daveydidit")), 11, sub, { 96, 21, 300, 16 });
        text(g, "v" JucePlugin_VersionString, 11, sub, { 400, 21, 104, 16 }, Justification::right);
        for (int i = 0; i < 4; ++i)
            text(g, names[i], 10, sub, knobs[i].getBounds().toFloat().withY(368).withHeight(14), Justification::centred);

        g.setColour(screen), g.fillRoundedRectangle(display, 10);
        const auto graph = display.reduced(16, 14).withTrimmedTop(16);
        const float mid = graph.getCentreY(), half = graph.getHeight() * 0.5f;
        text(g, "IN", 10, dim, display.reduced(16, 10).withHeight(14));
        text(g, "OUT", 10, orange, display.reduced(16, 10).withHeight(14).withTrimmedLeft(30));
        text(g, "GAIN +/-18 DB", 10, blue.brighter(0.5f), display.reduced(16, 10).withHeight(14).withTrimmedLeft(66));
        text(g, "2.5 S", 10, dim, display.reduced(16, 10).withHeight(14), Justification::right);

        // History, oldest on the left: mirrored level envelopes (square-root scale, 0 dBFS at 70 %) and the applied gain (+-18 dB).
        Path in, out, gain;
        const int w = proc.history.write.load(std::memory_order_acquire);
        auto level = [&](float v) { return half * jmin(1.0f, std::sqrt(v * 0.5f)); };
        for (int i = 0; i < SnapProcessor::historySize; ++i)
        {
            const size_t j = (size_t) ((w + i) % SnapProcessor::historySize);
            const float x = graph.getX() + graph.getWidth() * (float) i / (SnapProcessor::historySize - 1);
            const float gy = mid - half * jlimit(-1.0f, 1.0f, proc.history.gainDb[j].load() / 18.0f);
            for (auto [p, v] : { std::pair { &in, proc.history.in[j].load() }, std::pair { &out, proc.history.out[j].load() } })
                i == 0 ? p->startNewSubPath(x, mid - level(v)) : p->lineTo(x, mid - level(v));
            i == 0 ? gain.startNewSubPath(x, gy) : gain.lineTo(x, gy);
        }
        for (auto [p, colour] : { std::pair { &in, Colour(0xff4a4a46) }, std::pair { &out, orange.withAlpha(0.9f) } })
        {
            p->lineTo(graph.getRight(), mid), p->lineTo(graph.getX(), mid), p->closeSubPath();
            g.setColour(colour), g.fillPath(*p), g.fillPath(*p, AffineTransform::verticalFlip(2 * mid));
        }
        g.setColour(white.withAlpha(0.35f)), g.drawHorizontalLine((int) mid, graph.getX(), graph.getRight());
        g.setColour(blue.brighter(0.5f)), g.strokePath(gain, PathStrokeType(1.5f));

    }
};
} // namespace

AudioProcessorEditor* SnapProcessor::createEditor() { return new Editor(*this); }
