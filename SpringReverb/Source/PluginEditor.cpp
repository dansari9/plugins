#include "PluginEditor.h"

namespace
{
constexpr juce::uint32 kInk = 0xff24170f;
constexpr juce::uint32 kInkSoft = 0xff5c4632;
constexpr juce::uint32 kRust = 0xffc4472a;
constexpr juce::uint32 kCream = 0xfff6ead6;
constexpr juce::uint32 kFace = 0xffe6d2ae;
constexpr juce::uint32 kBrass = 0xffc6a15a;
constexpr juce::uint32 kChassis = 0xff140e0a;

juce::Font fontOf(float height, bool bold)
{
    auto options = juce::FontOptions { juce::jmax(8.0f, height) };
    if (bold)
        options = options.withStyle("Bold");
    return juce::Font { options };
}

void drawScrew(juce::Graphics& g, float x, float y, float d)
{
    g.setColour(juce::Colour(0xff8d7b5c));
    g.fillEllipse(x, y, d, d);
    g.setColour(juce::Colour(0xff3a2c1c));
    g.drawEllipse(x, y, d, d, 1.0f);
    g.drawLine(x + d * 0.2f, y + d * 0.5f, x + d * 0.8f, y + d * 0.5f, 1.0f);
    g.drawLine(x + d * 0.5f, y + d * 0.2f, x + d * 0.5f, y + d * 0.8f, 1.0f);
}
} // namespace

class SpringTankEditor::LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Label::textColourId, juce::Colour(kInk));
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff2a1c12));
        setColour(juce::ComboBox::textColourId, juce::Colour(kCream));
        setColour(juce::ComboBox::outlineColourId, juce::Colour(kBrass));
        setColour(juce::ComboBox::arrowColourId, juce::Colour(kBrass));
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff2a1c12));
        setColour(juce::PopupMenu::textColourId, juce::Colour(kCream));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(kRust));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(kCream));
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a2918));
        setColour(juce::TextButton::textColourOffId, juce::Colour(kCream));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider&) override
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height))
                          .reduced(3.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        if (radius < 4.0f)
            return;

        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        const float knobR = radius * 0.72f;

        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillEllipse(cx - knobR, cy - knobR + 2.0f, knobR * 2.0f, knobR * 2.0f);

        juce::Path track;
        track.addCentredArc(cx, cy, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(kInk).withAlpha(0.25f));
        g.strokePath(track, juce::PathStrokeType(juce::jmax(2.0f, radius * 0.06f),
                                                 juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        if (sliderPos > 0.001f)
        {
            juce::Path value;
            value.addCentredArc(cx, cy, radius - 1.5f, radius - 1.5f, 0.0f, startAngle, angle, true);
            g.setColour(juce::Colour(kRust));
            g.strokePath(value, juce::PathStrokeType(juce::jmax(2.0f, radius * 0.06f),
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        juce::ColourGradient cap(juce::Colour(0xff6e4e34), cx, cy - knobR,
                                 juce::Colour(0xff1c120c), cx, cy + knobR, false);
        g.setGradientFill(cap);
        g.fillEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f);
        g.setColour(juce::Colour(kBrass));
        g.drawEllipse(cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f, juce::jmax(1.2f, radius * 0.035f));

        juce::Path ticks;
        for (int i = 0; i < 11; ++i)
        {
            const float t = static_cast<float>(i) / 10.0f;
            const float a = startAngle + t * (endAngle - startAngle);
            const float inner = radius * 0.84f;
            const float outer = radius * 0.96f;
            ticks.startNewSubPath(cx + std::cos(a - juce::MathConstants<float>::halfPi) * inner,
                                  cy + std::sin(a - juce::MathConstants<float>::halfPi) * inner);
            ticks.lineTo(cx + std::cos(a - juce::MathConstants<float>::halfPi) * outer,
                         cy + std::sin(a - juce::MathConstants<float>::halfPi) * outer);
        }
        g.setColour(juce::Colour(kInk).withAlpha(0.45f));
        g.strokePath(ticks, juce::PathStrokeType(1.0f));

        juce::Path needle;
        needle.addRoundedRectangle(-1.15f, -knobR * 0.78f, 2.3f, knobR * 0.5f, 1.0f);
        g.setColour(juce::Colour(kCream));
        g.fillPath(needle, juce::AffineTransform::rotation(angle).translated(cx, cy));

        const float dot = juce::jmax(2.2f, knobR * 0.16f);
        g.setColour(juce::Colour(kBrass));
        g.fillEllipse(cx - dot, cy - dot, dot * 2.0f, dot * 2.0f);
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                      juce::ComboBox&) override
    {
        auto box = juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f,
                                          static_cast<float>(height) - 1.0f);
        g.setColour(juce::Colour(0xff2a1c12));
        g.fillRoundedRectangle(box, 4.0f);
        g.setColour(juce::Colour(kBrass));
        g.drawRoundedRectangle(box, 4.0f, 1.2f);

        juce::Path arrow;
        const float cx = static_cast<float>(width) - 14.0f;
        const float cy = static_cast<float>(height) * 0.5f;
        arrow.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
        g.fillPath(arrow);
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override
    {
        return fontOf(juce::jmax(11.0f, box.getHeight() * 0.42f), false);
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool highlighted, bool down) override
    {
        auto r = button.getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(down ? juce::Colour(kRust) : juce::Colour(highlighted ? 0xff4a3420 : 0xff3a2918));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colour(kBrass));
        g.drawRoundedRectangle(r, 4.0f, 1.1f);
    }

    juce::Font getTextButtonFont(juce::TextButton& button, int) override
    {
        return fontOf(juce::jmax(12.0f, button.getHeight() * 0.46f), true);
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool highlighted, bool) override
    {
        auto area = button.getLocalBounds().toFloat();
        const float d = juce::jmin(area.getWidth(), area.getHeight() * 0.72f);
        const float x = area.getCentreX() - d * 0.5f;
        const float y = area.getY() + 1.0f;
        const bool powered = !button.getToggleState();

        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillEllipse(x, y + 2.0f, d, d);

        juce::Colour lamp = powered ? juce::Colour(0xffffb089) : juce::Colour(0xff4a2018);
        if (highlighted && powered)
            lamp = lamp.brighter(0.15f);
        juce::ColourGradient glow(lamp, x + d * 0.35f, y + d * 0.3f,
                                  powered ? juce::Colour(0xff8c2a14) : juce::Colour(0xff1a0c0a),
                                  x + d * 0.5f, y + d, true);
        g.setGradientFill(glow);
        g.fillEllipse(x, y, d, d);
        g.setColour(juce::Colour(kBrass));
        g.drawEllipse(x, y, d, d, 1.3f);
        if (powered)
        {
            g.setColour(juce::Colour(kCream).withAlpha(0.85f));
            g.fillEllipse(x + d * 0.28f, y + d * 0.22f, d * 0.22f, d * 0.16f);
        }

        g.setColour(juce::Colour(kInk));
        g.setFont(fontOf(juce::jmax(8.0f, area.getHeight() * 0.22f), true));
        g.drawFittedText(powered ? "ON" : "OFF",
                         juce::Rectangle<int>(0, static_cast<int>(y + d + 1.0f),
                                              static_cast<int>(area.getWidth()),
                                              static_cast<int>(area.getHeight() - d - 1.0f)),
                         juce::Justification::centred, 1);
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
        setComponentID("knob-" + paramId);

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
        name.setColour(juce::Label::textColourId, juce::Colour(kInk));
        name.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);

        value.setJustificationType(juce::Justification::centred);
        value.setInterceptsMouseClicks(false, false);
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
        setScale(1.0f);
    }

    ~Knob() override { slider.setLookAndFeel(nullptr); }

    void detachLookAndFeel() { slider.setLookAndFeel(nullptr); }

    void setScale(float scale)
    {
        name.setFont(fontOf(12.0f * scale, true));
        value.setFont(fontOf(13.0f * scale, false));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const int nameH = juce::jmax(14, juce::roundToInt(getHeight() * 0.12f));
        const int valueH = juce::jmax(14, juce::roundToInt(getHeight() * 0.13f));
        name.setBounds(area.removeFromBottom(nameH));
        value.setBounds(area.removeFromBottom(valueH));
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

class SpringTankEditor::Meter : public juce::Component
{
public:
    explicit Meter(juce::String labelText)
        : label(std::move(labelText))
    {
    }

    void setLevels(float left, float right)
    {
        displayL = juce::jmax(left, displayL * 0.82f);
        displayR = juce::jmax(right, displayR * 0.82f);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colour(kInkSoft));
        g.setFont(fontOf(juce::jmax(9.0f, area.getHeight() * 0.38f), true));
        auto nameCol = area.removeFromLeft(juce::jmax(28.0f, area.getWidth() * 0.16f));
        g.drawFittedText(label, nameCol.toNearestInt(), juce::Justification::centredLeft, 1);

        auto drawBar = [&g](juce::Rectangle<float> row, float level)
        {
            g.setColour(juce::Colour(0xff2a1c12));
            g.fillRoundedRectangle(row, 2.0f);
            const float clamped = juce::jlimit(0.0f, 1.0f, level);
            auto fill = row.withWidth(juce::jmax(0.0f, row.getWidth() * clamped));
            juce::Colour bar = clamped > 0.92f ? juce::Colour(kRust) : juce::Colour(0xffd08a3a);
            g.setColour(bar);
            g.fillRoundedRectangle(fill, 2.0f);
        };

        const float gap = 3.0f;
        const float h = juce::jmax(4.0f, (area.getHeight() - gap) * 0.5f);
        drawBar(area.removeFromTop(h), displayL);
        area.removeFromTop(gap);
        drawBar(area.removeFromTop(h), displayR);
    }

private:
    juce::String label;
    float displayL = 0.0f;
    float displayR = 0.0f;
};

SpringTankEditor::SpringTankEditor(SpringTankProcessor& p)
    : juce::AudioProcessorEditor(&p),
      processor(p),
      lookAndFeel(std::make_unique<LookAndFeel>())
{
    setLookAndFeel(lookAndFeel.get());
    setOpaque(true);
    setWantsKeyboardFocus(false);

    constrainer.setFixedAspectRatio(static_cast<double>(kWidth) / static_cast<double>(kHeight));
    constrainer.setSizeLimits(570, 285, 1520, 760);
    setConstrainer(&constrainer);
    setResizable(true, true);

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

    presetBox.setComponentID("preset-box");
    presetBox.setLookAndFeel(lookAndFeel.get());
    presetBox.setJustificationType(juce::Justification::centredLeft);
    presetBox.setTextWhenNothingSelected("Preset");
    for (int i = 0; i < processor.getNumPrograms(); ++i)
        presetBox.addItem(processor.getProgramName(i), i + 1);
    syncPresetBox();
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id > 0 && id - 1 != processor.getCurrentProgram())
            processor.setCurrentProgram(id - 1);
    };
    addAndMakeVisible(presetBox);

    presetPrev.setComponentID("preset-prev");
    presetNext.setComponentID("preset-next");
    presetPrev.setLookAndFeel(lookAndFeel.get());
    presetNext.setLookAndFeel(lookAndFeel.get());
    presetPrev.onClick = [this] { stepPreset(-1); };
    presetNext.onClick = [this] { stepPreset(1); };
    addAndMakeVisible(presetPrev);
    addAndMakeVisible(presetNext);

    power.setComponentID("bypass");
    power.setLookAndFeel(lookAndFeel.get());
    power.setClickingTogglesState(true);
    powerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts, "bypass", power);
    addAndMakeVisible(power);

    inputMeterOwned = std::make_unique<Meter>("IN");
    outputMeterOwned = std::make_unique<Meter>("OUT");
    inputMeter = inputMeterOwned.get();
    outputMeter = outputMeterOwned.get();
    inputMeter->setComponentID("meter-in");
    outputMeter->setComponentID("meter-out");
    addAndMakeVisible(*inputMeter);
    addAndMakeVisible(*outputMeter);

    processor.getProgramBroadcaster().addChangeListener(this);
    startTimerHz(30);
    setSize(kWidth, kHeight);
}

SpringTankEditor::~SpringTankEditor()
{
    stopTimer();
    processor.getProgramBroadcaster().removeChangeListener(this);
    setConstrainer(nullptr);
    presetBox.setLookAndFeel(nullptr);
    presetPrev.setLookAndFeel(nullptr);
    presetNext.setLookAndFeel(nullptr);
    power.setLookAndFeel(nullptr);
    powerAttachment.reset();
    detachLookAndFeel();
    setLookAndFeel(nullptr);
}

void SpringTankEditor::detachLookAndFeel()
{
    for (auto& knob : knobs)
        if (knob != nullptr)
            knob->detachLookAndFeel();
}

void SpringTankEditor::changeListenerCallback(juce::ChangeBroadcaster*)
{
    syncPresetBox();
}

void SpringTankEditor::timerCallback()
{
    if (inputMeter != nullptr)
        inputMeter->setLevels(processor.getInputMeter(0), processor.getInputMeter(1));
    if (outputMeter != nullptr)
        outputMeter->setLevels(processor.getOutputMeter(0), processor.getOutputMeter(1));
}

void SpringTankEditor::syncPresetBox()
{
    const int id = processor.getCurrentProgram() + 1;
    if (presetBox.getSelectedId() != id)
        presetBox.setSelectedId(id, juce::dontSendNotification);
}

void SpringTankEditor::stepPreset(int delta)
{
    const int count = processor.getNumPrograms();
    if (count <= 0)
        return;
    int index = processor.getCurrentProgram() + delta;
    if (index < 0)
        index = count - 1;
    else if (index >= count)
        index = 0;
    processor.setCurrentProgram(index);
}

float SpringTankEditor::uiScale() const noexcept
{
    return juce::jmax(0.5f, static_cast<float>(getWidth()) / static_cast<float>(kWidth));
}

void SpringTankEditor::paint(juce::Graphics& g)
{
    const float scale = uiScale();
    g.fillAll(juce::Colour(kChassis));

    auto face = getLocalBounds().toFloat().reduced(8.0f * scale);
    g.setColour(juce::Colour(kFace));
    g.fillRoundedRectangle(face, 12.0f * scale);
    g.setColour(juce::Colour(kBrass));
    g.drawRoundedRectangle(face.reduced(0.5f), 12.0f * scale, 1.6f * scale);
    g.setColour(juce::Colour(kCream).withAlpha(0.55f));
    g.drawRoundedRectangle(face.reduced(5.0f * scale), 9.0f * scale, 1.0f);

    const float screw = 11.0f * scale;
    drawScrew(g, face.getX() + 10.0f * scale, face.getY() + 10.0f * scale, screw);
    drawScrew(g, face.getRight() - 10.0f * scale - screw, face.getY() + 10.0f * scale, screw);
    drawScrew(g, face.getX() + 10.0f * scale, face.getBottom() - 10.0f * scale - screw, screw);
    drawScrew(g, face.getRight() - 10.0f * scale - screw, face.getBottom() - 10.0f * scale - screw, screw);

    auto header = face.reduced(18.0f * scale, 10.0f * scale).removeFromTop(34.0f * scale);
    g.setColour(juce::Colour(kInk));
    g.setFont(fontOf(22.0f * scale, true));
    g.drawFittedText("SPRING TANK", header.removeFromLeft(210.0f * scale).toNearestInt(),
                     juce::Justification::centredLeft, 1);

    g.setColour(juce::Colour(kRust));
    g.setFont(fontOf(11.0f * scale, true));
    auto badge = header.removeFromLeft(110.0f * scale);
    g.drawFittedText("DW-9  v" + juce::String(JucePlugin_VersionString),
                     badge.toNearestInt(), juce::Justification::centredLeft, 1);
}

void SpringTankEditor::resized()
{
    const float scale = uiScale();
    auto area = getLocalBounds().reduced(juce::roundToInt(16.0f * scale), juce::roundToInt(12.0f * scale));

    auto header = area.removeFromTop(juce::roundToInt(58.0f * scale));
    header.removeFromLeft(juce::roundToInt(300.0f * scale));
    power.setBounds(header.removeFromRight(juce::roundToInt(52.0f * scale)).reduced(0, juce::roundToInt(4.0f * scale)));
    header.removeFromRight(juce::roundToInt(6.0f * scale));
    const int step = juce::roundToInt(32.0f * scale);
    presetNext.setBounds(header.removeFromRight(step).reduced(0, juce::roundToInt(14.0f * scale)));
    presetPrev.setBounds(header.removeFromRight(step).reduced(0, juce::roundToInt(14.0f * scale)));
    header.removeFromRight(juce::roundToInt(6.0f * scale));
    presetBox.setBounds(header.reduced(0, juce::roundToInt(14.0f * scale)));

    area.removeFromBottom(juce::roundToInt(16.0f * scale));
    auto footer = area.removeFromBottom(juce::roundToInt(32.0f * scale));
    footer = footer.reduced(juce::roundToInt(18.0f * scale), 0);
    const int meterGap = juce::roundToInt(10.0f * scale);
    auto leftMeter = footer.removeFromLeft((footer.getWidth() - meterGap) / 2);
    footer.removeFromLeft(meterGap);
    if (inputMeter != nullptr)
        inputMeter->setBounds(leftMeter);
    if (outputMeter != nullptr)
        outputMeter->setBounds(footer);

    area.removeFromTop(juce::roundToInt(8.0f * scale));
    const int count = static_cast<int>(knobs.size());
    if (count == 0 || area.getWidth() <= 0 || area.getHeight() <= 0)
        return;

    const int slot = area.getWidth() / count;
    for (int i = 0; i < count; ++i)
    {
        if (knobs[static_cast<size_t>(i)] == nullptr)
            continue;
        knobs[static_cast<size_t>(i)]->setScale(scale);
        knobs[static_cast<size_t>(i)]->setBounds(area.removeFromLeft(slot).reduced(juce::roundToInt(3.0f * scale), 0));
    }
}
