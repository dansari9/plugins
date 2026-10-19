#include "PluginEditor.h"

namespace springui
{
juce::Font fontOf(float height, bool bold)
{
    auto options = juce::FontOptions { height };
    if (bold)
        options = options.withStyle("Bold");
    return juce::Font { options };
}

constexpr juce::uint32 kInk = 0xff2a1c12;
constexpr juce::uint32 kInkSoft = 0xff5a4632;
constexpr juce::uint32 kRust = 0xffb84324;
constexpr juce::uint32 kCream = 0xfff4ead2;
constexpr juce::uint32 kBrass = 0xffc6a15a;

class VintageLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VintageLookAndFeel()
    {
        setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff2a1c12));
        setColour(juce::TooltipWindow::textColourId, juce::Colour(kCream));
        setColour(juce::TooltipWindow::outlineColourId, juce::Colour(kBrass));
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height))
                          .reduced(8.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        const float knobR = radius * 0.74f;

        g.setColour(juce::Colours::black.withAlpha(0.28f));
        g.fillEllipse(cx - knobR, cy - knobR + 3.0f, knobR * 2.0f, knobR * 2.0f);

        juce::Path track;
        track.addCentredArc(cx, cy, radius - 1.0f, radius - 1.0f, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(kInk).withAlpha(0.28f));
        g.strokePath(track, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        if (sliderPos > 0.001f)
        {
            juce::Path value;
            value.addCentredArc(cx, cy, radius - 1.0f, radius - 1.0f, 0.0f, startAngle, angle, true);
            g.setColour(juce::Colour(kRust));
            g.strokePath(value, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        juce::ColourGradient cap(juce::Colour(0xff6a4b34), cx, cy - knobR,
                                 juce::Colour(0xff1a100c), cx, cy + knobR, false);
        g.setGradientFill(cap);
        g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f);

        g.setColour(juce::Colour(kBrass));
        g.drawEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f, 1.6f);
        g.setColour(juce::Colour(0xff8a6433).withAlpha(0.9f));
        g.drawEllipse(cx - knobR + 3.0f, cy - knobR + 3.0f, knobR * 2.0f - 6.0f, knobR * 2.0f - 6.0f, 1.0f);

        juce::Path needle;
        const float needleLen = knobR * 0.82f;
        needle.addRoundedRectangle(-1.25f, -needleLen, 2.5f, needleLen * 0.58f, 1.0f);
        g.setColour(juce::Colour(kCream));
        g.fillPath(needle, juce::AffineTransform::rotation(angle).translated(cx, cy));

        const float dot = juce::jmax(2.5f, knobR * 0.16f);
        g.setColour(juce::Colour(kBrass));
        g.fillEllipse(cx - dot, cy - dot, dot * 2.0f, dot * 2.0f);
        g.setColour(juce::Colour(0xff3a2416));
        g.fillEllipse(cx - dot * 0.45f, cy - dot * 0.45f, dot * 0.9f, dot * 0.9f);
    }
};

class VintageKnob : public juce::Component
{
public:
    VintageKnob(juce::AudioProcessorValueTreeState& apvts, const juce::String& paramId,
                const juce::String& labelText, const juce::String& tip,
                std::function<juce::String(float)> formatter)
        : format(std::move(formatter))
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.18f,
                                   juce::MathConstants<float>::pi * 2.82f, true);
        slider.setTooltip(tip);
        slider.setWantsKeyboardFocus(false);

        name.setText(labelText, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        name.setInterceptsMouseClicks(false, false);
        name.setFont(fontOf(12.5f, true));
        name.setColour(juce::Label::textColourId, juce::Colour(kInk));
        name.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        value.setJustificationType(juce::Justification::centred);
        value.setInterceptsMouseClicks(false, false);
        value.setFont(fontOf(12.0f, false));
        value.setColour(juce::Label::textColourId, juce::Colour(kInkSoft));
        value.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        addAndMakeVisible(slider);
        addAndMakeVisible(name);
        addAndMakeVisible(value);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, paramId, slider);

        if (auto* param = apvts.getParameter(paramId))
            slider.setDoubleClickReturnValue(true, static_cast<double>(param->convertFrom0to1(param->getDefaultValue())));

        slider.onValueChange = [this] { refresh(); };
        refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        name.setBounds(area.removeFromTop(18));
        value.setBounds(area.removeFromBottom(18));
        const int side = juce::jmin(area.getWidth(), area.getHeight());
        slider.setBounds(area.withSizeKeepingCentre(side, side));
    }

private:
    void refresh()
    {
        value.setText(format(static_cast<float>(slider.getValue())), juce::dontSendNotification);
    }

    juce::Slider slider;
    juce::Label name;
    juce::Label value;
    std::function<juce::String(float)> format;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

juce::Image makeGrain(int size)
{
    juce::Image image(juce::Image::ARGB, size, size, true);
    juce::Graphics g(image);
    juce::Random rng(0x51A7C0u);
    for (int i = 0; i < size * 14; ++i)
    {
        const int x = rng.nextInt(size);
        const int y = rng.nextInt(size);
        const juce::uint8 alpha = static_cast<juce::uint8>(rng.nextInt(36));
        g.setColour(juce::Colour::fromRGBA(48, 28, 12, alpha));
        g.fillRect(x, y, 1, 1);
    }
    return image;
}
} // namespace springui

struct SpringTankEditor::Content
{
    springui::VintageLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltip;
    std::array<std::unique_ptr<springui::VintageKnob>, 7> knobs;

    explicit Content(juce::Component& owner)
        : tooltip(&owner, 700)
    {
    }
};

SpringTankEditor::SpringTankEditor(SpringTankProcessor& p)
    : juce::AudioProcessorEditor(&p),
      processor(p),
      content(std::make_unique<Content>(*this)),
      grain(springui::makeGrain(128))
{
    setLookAndFeel(&content->lookAndFeel);
    content->tooltip.setLookAndFeel(&content->lookAndFeel);

    auto& apvts = processor.getApvts();
    struct Spec
    {
        const char* id;
        const char* label;
        const char* tip;
        std::function<juce::String(float)> format;
    };

    const Spec specs[7] = {
        { "decay", "DECAY",
          "Tank decay, from a short amp splash to a long surf bloom.",
          [](float v) { return juce::String(v, 2) + " s"; } },
        { "tension", "TENSION",
          "Spring tension. Loose is a slow chirp; tight is a fast ping.",
          [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "tone", "TONE",
          "Damping and brightness of the tank.",
          [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "drip", "DRIP",
          "Emphasis of the bright, falling chirp on the note attack.",
          [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "predelay", "PRE-DELAY",
          "Silence before the tank speaks.",
          [](float v) { return juce::String(v, 1) + " ms"; } },
        { "mix", "MIX",
          "Dry/wet balance. Fully wet is the spring alone.",
          [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; } },
        { "output", "OUTPUT",
          "Level after the dry/wet mix.",
          [](float v) { return juce::String(v, 1) + " dB"; } }
    };

    for (size_t i = 0; i < 7; ++i)
    {
        content->knobs[i] = std::make_unique<springui::VintageKnob>(apvts, specs[i].id, specs[i].label,
                                                                   specs[i].tip, specs[i].format);
        addAndMakeVisible(*content->knobs[i]);
    }

    setOpaque(true);
    setResizable(true, true);
    setResizeLimits(680, 300, 1180, 520);
    setSize(860, 392);
}

SpringTankEditor::~SpringTankEditor()
{
    setLookAndFeel(nullptr);
    if (content != nullptr)
        content->tooltip.setLookAndFeel(nullptr);
}

int SpringTankEditor::headerHeight() const
{
    return juce::roundToInt(static_cast<float>(getHeight()) * 0.40f);
}

void SpringTankEditor::drawScrew(juce::Graphics& g, float x, float y) const
{
    g.setColour(juce::Colour(0xff8d7b5c));
    g.fillEllipse(x, y, 13.0f, 13.0f);
    g.setColour(juce::Colour(0xff3a2c1c));
    g.drawEllipse(x, y, 13.0f, 13.0f, 1.0f);
    g.drawLine(x + 2.5f, y + 6.5f, x + 10.5f, y + 6.5f, 1.2f);
    g.drawLine(x + 6.5f, y + 2.5f, x + 6.5f, y + 10.5f, 1.2f);
}

void SpringTankEditor::paint(juce::Graphics& g)
{
    using springui::kCream;
    using springui::kInk;
    using springui::kInkSoft;
    using springui::kRust;

    g.fillAll(juce::Colour(0xff120d09));

    auto face = getLocalBounds().toFloat().reduced(16.0f, 14.0f);
    g.setColour(juce::Colour(0xffd8c4a0));
    g.fillRoundedRectangle(face, 12.0f);

    g.setTiledImageFill(grain, face.getX(), face.getY(), 0.45f);
    g.fillRoundedRectangle(face, 12.0f);

    g.setColour(juce::Colour(0xff8a6433));
    g.drawRoundedRectangle(face, 12.0f, 2.0f);
    g.setColour(juce::Colour(kCream).withAlpha(0.55f));
    g.drawRoundedRectangle(face.reduced(5.0f), 9.0f, 1.0f);

    drawScrew(g, face.getX() + 12.0f, face.getY() + 12.0f);
    drawScrew(g, face.getRight() - 25.0f, face.getY() + 12.0f);
    drawScrew(g, face.getX() + 12.0f, face.getBottom() - 25.0f);
    drawScrew(g, face.getRight() - 25.0f, face.getBottom() - 25.0f);

    // Power jewel.
    const float lampX = face.getX() + 36.0f;
    const float lampY = face.getY() + 28.0f;
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(lampX, lampY + 1.5f, 16.0f, 16.0f);
    juce::ColourGradient lamp(juce::Colour(0xffffb089), lampX + 5.0f, lampY + 4.0f,
                              juce::Colour(0xff8c2a14), lampX + 4.0f, lampY + 14.0f, true);
    g.setGradientFill(lamp);
    g.fillEllipse(lampX, lampY, 16.0f, 16.0f);
    g.setColour(juce::Colour(kCream).withAlpha(0.85f));
    g.fillEllipse(lampX + 3.5f, lampY + 3.0f, 4.5f, 3.5f);

    auto header = face.reduced(28.0f, 16.0f);
    header.setHeight(static_cast<float>(headerHeight()) - 28.0f);

    g.setColour(juce::Colour(kInk));
    g.setFont(springui::fontOf(juce::jlimit(22.0f, 34.0f, getWidth() * 0.034f), true));
    g.drawFittedText("SPRING TANK", header.removeFromTop(40.0f).toNearestInt(),
                     juce::Justification::centred, 1);

    g.setColour(juce::Colour(kRust));
    g.setFont(springui::fontOf(12.0f, true));
    auto subtitle = header.removeFromTop(18.0f);
    g.drawFittedText("SURF   SPRING   REVERB", subtitle.toNearestInt(),
                     juce::Justification::centred, 1);

    // A small coil under the name.
    juce::Path coil;
    const float coilW = juce::jmin(280.0f, face.getWidth() * 0.42f);
    const float x0 = face.getCentreX() - coilW * 0.5f;
    const float y0 = subtitle.getBottom() + 8.0f;
    const int turns = 22;
    for (int i = 0; i <= turns * 8; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(turns * 8);
        const float x = x0 + t * coilW;
        const float envelope = std::sin(t * juce::MathConstants<float>::pi);
        const float y = y0 + std::sin(t * static_cast<float>(turns) * juce::MathConstants<float>::twoPi)
                             * (7.0f * envelope + 1.5f);
        if (i == 0)
            coil.startNewSubPath(x, y);
        else
            coil.lineTo(x, y);
    }
    g.setColour(juce::Colour(kInk).withAlpha(0.78f));
    g.strokePath(coil, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));

    g.setColour(juce::Colour(kInkSoft));
    g.setFont(springui::fontOf(11.0f, false));
    auto footer = face.removeFromBottom(26.0f).reduced(36.0f, 0.0f);
    const juce::String footerText = "DRIPWORKS    DW-9    v" + juce::String(JucePlugin_VersionString);
    g.drawFittedText(footerText, footer.toNearestInt(), juce::Justification::centred, 1);
}

void SpringTankEditor::resized()
{
    if (content == nullptr)
        return;

    auto area = getLocalBounds();
    area.removeFromTop(headerHeight());
    area.removeFromBottom(juce::jmax(28, getHeight() / 12));
    area = area.reduced(28, 0);

    const int count = static_cast<int>(content->knobs.size());
    const int slot = area.getWidth() / count;
    for (int i = 0; i < count; ++i)
    {
        auto column = area.removeFromLeft(slot);
        if (content->knobs[static_cast<size_t>(i)] != nullptr)
            content->knobs[static_cast<size_t>(i)]->setBounds(column.reduced(2, 0));
    }
}
