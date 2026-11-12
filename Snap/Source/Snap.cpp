#include "Snap.h"
#include "BinaryData.h"

using namespace juce;

namespace
{
const char* const ids[] = { "attack", "sustain", "mix", "output" }; // FROZEN

auto percent(bool sign)
{
    return AudioParameterFloatAttributes().withStringFromValueFunction(
        [sign](float v, int) { return (sign && v > 0.05f ? "+" : "") + String(v, 0) + "%"; });
}

AudioProcessorValueTreeState::ParameterLayout layout()
{
    auto p = [](const char* id, const char* name, float lo, float hi, float def, AudioParameterFloatAttributes a) {
        return std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name, NormalisableRange<float>(lo, hi, 0.1f), def, a);
    };
    return { p(ids[0], "Attack", -100, 100, 0, percent(true)), p(ids[1], "Sustain", -100, 100, 0, percent(true)),
             p(ids[2], "Mix", 0, 100, 100, percent(false)),
             p(ids[3], "Output", -24, 12, 0, AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) {
                   return (v > 0.05f ? "+" : "") + String(v, 1) + " dB";
               })) };
}

float coef(double fs, double ms) { return (float) std::exp(-1000.0 / (ms * fs)); }
float follow(float e, float x, float a, float r) { return x + (x > e ? a : r) * (e - x); }
float db(float g) { return Decibels::gainToDecibels(g, -120.0f); }
} // namespace

SnapProcessor::SnapProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", AudioChannelSet::stereo()).withOutput("Output", AudioChannelSet::stereo())),
      apvts(*this, nullptr, "SNAP", layout()), attack(apvts.getRawParameterValue(ids[0])),
      sustain(apvts.getRawParameterValue(ids[1])), mix(apvts.getRawParameterValue(ids[2])), output(apvts.getRawParameterValue(ids[3]))
{
}

bool SnapProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

void SnapProcessor::prepareToPlay(double fs, int)
{
    cFastA = coef(fs, 0.3), cFastR = coef(fs, 40), cSlowA = coef(fs, 20), cSlowR = coef(fs, 300), cGain = coef(fs, 1);
    fast = slowAttack = slowRelease = gainDb = peakIn = peakOut = peakGain = 0;
    decimation = jmax(1, (int) (fs * 2.5 / historySize)), count = 0;
    for (auto* s : { &sAttack, &sSustain, &sMix, &sOut })
        s->reset(fs, 0.02);
    sAttack.setCurrentAndTargetValue(*attack), sSustain.setCurrentAndTargetValue(*sustain);
    sMix.setCurrentAndTargetValue(*mix * 0.01f), sOut.setCurrentAndTargetValue(Decibels::decibelsToGain(output->load()));
}

void SnapProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), channels = jmin(2, getTotalNumInputChannels(), buffer.getNumChannels());
    for (int c = channels; c < buffer.getNumChannels(); ++c)
        buffer.clear(c, 0, n);
    if (channels == 0)
        return;
    sAttack.setTargetValue(*attack), sSustain.setTargetValue(*sustain);
    sMix.setTargetValue(*mix * 0.01f), sOut.setTargetValue(Decibels::decibelsToGain(output->load()));
    float* x[2] = { buffer.getWritePointer(0), buffer.getWritePointer(channels - 1) };

    for (int i = 0; i < n; ++i)
    {
        float level = 0, outLevel = 0;
        for (int c = 0; c < channels; ++c) // non-finite input becomes silence; absurd levels are clipped
            level = jmax(level, std::abs(x[c][i] = std::isfinite(x[c][i]) ? jlimit(-1000.0f, 1000.0f, x[c][i]) : 0.0f));

        // Envelope difference, linked across channels: transient = fast attack over slow attack,
        // sustain = slow release over fast release. Both are >= 0 dB amounts.
        fast = follow(fast, level, cFastA, cFastR);
        slowAttack = follow(slowAttack, level, cSlowA, cFastR);
        slowRelease = follow(slowRelease, level, cFastA, cSlowR);
        const float transient = jmax(0.0f, db(fast) - db(slowAttack)), tail = jmax(0.0f, db(slowRelease) - db(fast));
        const float target = jlimit(-24.0f, 24.0f, sAttack.getNextValue() * 0.006f * transient + sSustain.getNextValue() * 0.01f * tail);
        gainDb = target + cGain * (gainDb - target);

        const float wet = sMix.getNextValue(), gain = (1.0f + wet * (Decibels::decibelsToGain(gainDb) - 1.0f)) * sOut.getNextValue();
        for (int c = 0; c < channels; ++c)
            outLevel = jmax(outLevel, std::abs(x[c][i] *= gain));

        peakIn = jmax(peakIn, level), peakOut = jmax(peakOut, outLevel);
        peakGain = std::abs(gainDb) > std::abs(peakGain) ? gainDb : peakGain;
        if (++count >= decimation)
        {
            const int w = history.write.load(std::memory_order_relaxed);
            history.in[(size_t) w] = peakIn, history.out[(size_t) w] = peakOut, history.gainDb[(size_t) w] = peakGain;
            history.write.store((w + 1) % historySize, std::memory_order_release);
            peakIn = peakOut = peakGain = 0, count = 0;
        }
    }
}

void SnapProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", stateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, dest);
}

// Accepts only a <SNAP> tree. Unknown entries are ignored, values are clamped, missing ones reset to default.
void SnapProcessor::setStateInformation(const void* data, int size)
{
    const auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
        return;
    const auto tree = ValueTree::fromXml(*xml);
    for (auto* id : ids)
    {
        auto* p = apvts.getParameter(id);
        const auto s = tree.getChildWithProperty("id", id)["value"].toString().trim();
        const float f = s.isNotEmpty() && s.containsOnly("+-.0123456789eE") ? s.getFloatValue() : NAN;
        p->setValueNotifyingHost(std::isfinite(f) ? p->convertTo0to1(p->getNormalisableRange().snapToLegalValue(f)) : p->getDefaultValue());
    }
}

//==============================================================================
namespace
{
const Colour face { 0xffd9d8d2 }, ink { 0xff1a1a18 }, sub { 0xff6e6d68 }, screen { 0xff0f100f }, key { 0xffecebe6 },
    orange { 0xffff5a1f }, blue { 0xff2f6bff }, grey { 0xff9a9993 }, white { 0xfff6f5f0 }, dim { 0xff7a7a74 }, rule { 0xff262624 };

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
    static constexpr const char* names[] = { "ATTACK", "SUSTAIN", "MIX", "OUTPUT" };
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
        text(g, "SNAP", 24, ink, { 20, 14, 80, 28 });
        text(g, String(CharPointer_UTF8("transient shaper \xc2\xb7 dripworks")), 11, sub, { 96, 21, 300, 16 });
        text(g, "v" JucePlugin_VersionString, 11, sub, { 400, 21, 104, 16 }, Justification::right);
        for (int i = 0; i < 4; ++i)
            text(g, names[i], 10, sub, knobs[i].getBounds().toFloat().withY(368).withHeight(14), Justification::centred);

        g.setColour(screen), g.fillRoundedRectangle(display, 10);
        const auto graph = display.reduced(16, 14).withTrimmedTop(16).withTrimmedBottom(62);
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
            p->addPath(*p, AffineTransform::verticalFlip(2 * mid));
            g.setColour(colour), g.fillPath(*p);
        }
        g.setColour(white.withAlpha(0.35f)), g.drawHorizontalLine((int) mid, graph.getX(), graph.getRight());
        g.setColour(blue.brighter(0.5f)), g.strokePath(gain, PathStrokeType(1.5f));

        g.setColour(rule), g.drawHorizontalLine((int) display.getBottom() - 62, display.getX() + 16, display.getRight() - 16);
        for (int i = 0; i < 4; ++i)
        {
            const float x = display.getX() + 18 + (float) i * 118, y = display.getBottom() - 52;
            g.setColour(knobs[i].findColour(Slider::thumbColourId)), g.fillEllipse(x, y + 4, 6, 6);
            text(g, names[i], 10, dim, { x + 12, y, 100, 14 });
            text(g, knobs[i].getTextFromValue(knobs[i].getValue()), 22, white, { x, y + 16, 116, 28 });
        }
    }
};
} // namespace

AudioProcessorEditor* SnapProcessor::createEditor() { return new Editor(*this); }
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SnapProcessor(); }
