#include "PluginEditor.h"

namespace
{
constexpr juce::uint32 kInk = 0xff24170f;
constexpr juce::uint32 kInkSoft = 0xff5c4632;
constexpr juce::uint32 kRust = 0xffc24b28;
constexpr juce::uint32 kCream = 0xfff3e6cf;
constexpr juce::uint32 kFace = 0xffe4d0ae;
constexpr juce::uint32 kBrass = 0xffc6a15a;

juce::Font fontOf(float height, bool bold)
{
    auto options = juce::FontOptions { height };
    if (bold)
        options = options.withStyle("Bold");
    return juce::Font { options };
}
} // namespace

class SpringTankEditor::LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Label::textColourId, juce::Colour(kInk));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height))
                          .reduced(4.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        const float knobR = radius * 0.72f;

        g.setColour(juce::Colours::black.withAlpha(0.22f));
        g.fillEllipse(cx - knobR, cy - knobR + 2.0f, knobR * 2.0f, knobR * 2.0f);

        juce::Path track;
        track.addCentredArc(cx, cy, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(kInk).withAlpha(0.22f));
        g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        if (sliderPos > 0.001f)
        {
            juce::Path value;
            value.addCentredArc(cx, cy, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, angle, true);
            g.setColour(juce::Colour(kRust));
            g.strokePath(value, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        juce::ColourGradient cap(juce::Colour(0xff6e4e34), cx, cy - knobR,
                                 juce::Colour(0xff1c120c), cx, cy + knobR, false);
        g.setGradientFill(cap);
        g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f);
        g.setColour(juce::Colour(kBrass));
        g.drawEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f, 1.6f);

        juce::Path needle;
        needle.addRoundedRectangle(-1.15f, -knobR * 0.78f, 2.3f, knobR * 0.48f, 1.0f);
        g.setColour(juce::Colour(kCream));
        g.fillPath(needle, juce::AffineTransform::rotation(angle).translated(cx, cy));

        const float dot = juce::jmax(2.4f, knobR * 0.16f);
        g.setColour(juce::Colour(kBrass));
        g.fillEllipse(cx - dot, cy - dot, dot * 2.0f, dot * 2.0f);
    }
};

class SpringTankEditor::Knob : public juce::Component
{
public:
    Knob(juce::AudioProcessorValueTreeState& apvts, juce::LookAndFeel& feel,
         const juce::String& paramId, const juce::String& labelText,
         std::function<juce::String(float)> formatter)
        : format(std::move(formatter))
    {
        slider.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters(juce::MathConstants<float>::pi * 1.2f,
                                   juce::MathConstants<float>::pi * 2.8f, true);
        slider.setScrollWheelEnabled(false);
        slider.setPopupDisplayEnabled(false, false, nullptr);
        slider.setLookAndFeel(&feel);
        slider.setWantsKeyboardFocus(false);

        name.setText(labelText, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        name.setInterceptsMouseClicks(false, false);
        name.setFont(fontOf(11.0f, true));
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
            slider.setDoubleClickReturnValue(true, param->convertFrom0to1(param->getDefaultValue()));

        slider.onValueChange = [this] { refreshValue(); };
        refreshValue();
    }

    ~Knob() override
    {
        slider.setLookAndFeel(nullptr);
    }

    void detachLookAndFeel()
    {
        slider.setLookAndFeel(nullptr);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        name.setBounds(area.removeFromBottom(16));
        value.setBounds(area.removeFromBottom(18));
        slider.setBounds(area);
    }

private:
    void refreshValue()
    {
        value.setText(format(static_cast<float>(slider.getValue())), juce::dontSendNotification);
    }

    juce::Slider slider;
    juce::Label name;
    juce::Label value;
    std::function<juce::String(float)> format;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

SpringTankEditor::SpringTankEditor(SpringTankProcessor& p)
    : juce::AudioProcessorEditor(&p),
      processor(p),
      lookAndFeel(std::make_unique<LookAndFeel>())
{
    setLookAndFeel(lookAndFeel.get());
    setOpaque(true);
    setResizable(false, false);
    setWantsKeyboardFocus(false);

    struct Spec
    {
        const char* id;
        const char* label;
        std::function<juce::String(float)> format;
    };

    const Spec specs[7] = {
        { "decay", "DECAY", [](float v) { return juce::String(v, 2) + " s"; } },
        { "tension", "TENSION", [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "tone", "TONE", [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "drip", "DRIP", [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "predelay", "PRE-DELAY", [](float v) { return juce::String(v, 1) + " ms"; } },
        { "mix", "MIX", [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; } },
        { "output", "OUTPUT", [](float v) { return juce::String(v, 1) + " dB"; } }
    };

    auto& apvts = processor.getApvts();
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        knobs[i] = std::make_unique<Knob>(apvts, *lookAndFeel, specs[i].id, specs[i].label, specs[i].format);
        addAndMakeVisible(*knobs[i]);
    }

    presetBox.setLookAndFeel(lookAndFeel.get());
    presetBox.setJustificationType(juce::Justification::centredLeft);
    presetBox.setTextWhenNothingSelected("Preset");
    presetBox.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff2a1c12));
    presetBox.setColour(juce::ComboBox::textColourId, juce::Colour(kCream));
    presetBox.setColour(juce::ComboBox::outlineColourId, juce::Colour(kBrass));
    presetBox.setColour(juce::ComboBox::arrowColourId, juce::Colour(kBrass));
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem(processor.getProgramName(i), i + 1);
    syncPresetBox();
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id > 0)
            processor.setCurrentProgram(id - 1);
    };
    addAndMakeVisible(presetBox);
    processor.getProgramBroadcaster().addChangeListener(this);

    setSize(kWidth, kHeight);
}

SpringTankEditor::~SpringTankEditor()
{
    processor.getProgramBroadcaster().removeChangeListener(this);
    presetBox.setLookAndFeel(nullptr);
    detachLookAndFeel();
    setLookAndFeel(nullptr);
}

void SpringTankEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    syncPresetBox();
}

void SpringTankEditor::syncPresetBox()
{
    const int id = processor.getCurrentProgram() + 1;
    if (presetBox.getSelectedId() != id)
        presetBox.setSelectedId(id, juce::dontSendNotification);
}

void SpringTankEditor::detachLookAndFeel()
{
    for (auto& knob : knobs)
        if (knob != nullptr)
            knob->detachLookAndFeel();
}

void SpringTankEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff140e0a));

    auto face = getLocalBounds().toFloat().reduced(8.0f, 8.0f);
    g.setColour(juce::Colour(kFace));
    g.fillRoundedRectangle(face, 10.0f);
    g.setColour(juce::Colour(kBrass));
    g.drawRoundedRectangle(face.reduced(0.5f), 10.0f, 1.5f);
    g.setColour(juce::Colour(kCream).withAlpha(0.45f));
    g.drawRoundedRectangle(face.reduced(4.0f), 8.0f, 1.0f);

    auto header = face.reduced(16.0f, 8.0f).removeFromTop(28.0f);
    g.setColour(juce::Colour(kInk));
    g.setFont(fontOf(20.0f, true));
    g.drawFittedText("SPRING TANK", header.removeFromLeft(180.0f).toNearestInt(),
                     juce::Justification::centredLeft, 1);
}

void SpringTankEditor::resized()
{
    auto area = getLocalBounds().reduced(16, 10);
    auto header = area.removeFromTop(32);
    header.removeFromLeft(176);
    presetBox.setBounds(header.reduced(0, 4));
    area.removeFromTop(4);

    const int count = static_cast<int>(knobs.size());
    if (count == 0 || area.isEmpty())
        return;

    const int slot = area.getWidth() / count;
    for (int i = 0; i < count; ++i)
    {
        if (knobs[static_cast<size_t>(i)] != nullptr)
            knobs[static_cast<size_t>(i)]->setBounds(area.removeFromLeft(slot).reduced(3, 0));
    }
}
