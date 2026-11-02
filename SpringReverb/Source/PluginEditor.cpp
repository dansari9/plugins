#include "PluginEditor.h"
#include "BinaryData.h"

namespace
{
constexpr juce::uint32 kBg = 0xff090a0c;
constexpr juce::uint32 kText = 0xfff3f4f6;
constexpr juce::uint32 kMuted = 0xff8d929c;
constexpr juce::uint32 kLine = 0xff2a2d34;
constexpr juce::uint32 kAccent = 0xff8ea2ff;

juce::Typeface::Ptr typefaceFor(const void* data, size_t bytes)
{
    return juce::Typeface::createSystemTypefaceFor(data, bytes);
}

juce::Font fontFrom(const juce::Typeface::Ptr& face, float height)
{
    return juce::Font { juce::FontOptions { face }.withHeight(juce::jmax(8.0f, height)) };
}
} // namespace

class SpringTankEditor::LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
        : regular(typefaceFor(BinaryData::InterRegular_ttf, BinaryData::InterRegular_ttfSize)),
          medium(typefaceFor(BinaryData::InterMedium_ttf, BinaryData::InterMedium_ttfSize))
    {
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff14161b));
        setColour(juce::ComboBox::textColourId, juce::Colour(kText));
        setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ComboBox::arrowColourId, juce::Colour(kMuted));
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff14161b));
        setColour(juce::PopupMenu::textColourId, juce::Colour(kText));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, juce::Colour(0xff23262e));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colour(kText));
        setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour(juce::TextButton::textColourOffId, juce::Colour(kMuted));
        setColour(juce::Label::textColourId, juce::Colour(kMuted));
    }

    juce::Font regularFont(float height) const { return fontFrom(regular, height); }
    juce::Font mediumFont(float height) const { return fontFrom(medium, height); }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(width), static_cast<float>(height))
                          .reduced(2.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        if (radius < 6.0f)
            return;

        const float hover = static_cast<float>(slider.getProperties().getWithDefault("hover", 0.0f));
        const bool hero = static_cast<int>(slider.getProperties().getWithDefault("hero", 0)) != 0;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);
        const float arcR = radius - (hero ? 6.0f : 4.0f);
        const float stroke = (hero ? 2.4f : 1.35f) * (1.0f + 0.55f * hover);

        juce::Path track;
        track.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour(juce::Colour(kLine).withAlpha(0.9f + 0.1f * hover));
        g.strokePath(track, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));

        if (sliderPos > 0.001f)
        {
            juce::Path value;
            value.addCentredArc(cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
            g.setColour(juce::Colour(kAccent).withAlpha(0.72f + 0.28f * hover));
            g.strokePath(value, juce::PathStrokeType(stroke, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        const float cap = juce::jmax(2.2f, stroke * 1.15f);
        const auto end = juce::Point<float>(cx, cy).getPointOnCircumference(arcR, angle);
        g.setColour(juce::Colour(kText).withAlpha(0.85f + 0.15f * hover));
        g.fillEllipse(end.x - cap, end.y - cap, cap * 2.0f, cap * 2.0f);
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                      juce::ComboBox& box) override
    {
        const float hover = box.isMouseOver(true) ? 1.0f : 0.0f;
        auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        g.setColour(juce::Colour(0xff14161b).interpolatedWith(juce::Colour(0xff1c1f27), hover));
        g.fillRoundedRectangle(r, r.getHeight() * 0.5f);

        juce::Path arrow;
        const float cx = static_cast<float>(width) - 16.0f;
        const float cy = static_cast<float>(height) * 0.5f;
        arrow.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
        g.setColour(juce::Colour(kMuted));
        g.fillPath(arrow);
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override
    {
        return regularFont(juce::jmax(12.0f, box.getHeight() * 0.40f));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool highlighted, bool down) override
    {
        auto r = button.getLocalBounds().toFloat().reduced(1.0f);
        const float alpha = down ? 0.16f : (highlighted ? 0.10f : 0.0f);
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.fillEllipse(r);
    }

    juce::Font getTextButtonFont(juce::TextButton& button, int) override
    {
        return mediumFont(juce::jmax(13.0f, button.getHeight() * 0.42f));
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool highlighted, bool) override
    {
        auto area = button.getLocalBounds().toFloat().reduced(1.0f);
        const float h = juce::jmin(22.0f, area.getHeight());
        const float w = juce::jmin(area.getWidth(), h * 1.8f);
        auto track = juce::Rectangle<float>(area.getCentreX() - w * 0.5f,
                                            area.getCentreY() - h * 0.5f, w, h);
        const bool bypassed = button.getToggleState();
        g.setColour(bypassed ? juce::Colour(0xff22242c)
                             : juce::Colour(kAccent).withAlpha(highlighted ? 0.95f : 0.82f));
        g.fillRoundedRectangle(track, h * 0.5f);

        const float pad = 2.5f;
        const float d = h - pad * 2.0f;
        const float x = bypassed ? track.getX() + pad : track.getRight() - pad - d;
        g.setColour(juce::Colour(kText));
        g.fillEllipse(x, track.getY() + pad, d, d);
    }

private:
    juce::Typeface::Ptr regular;
    juce::Typeface::Ptr medium;
};

class SpringTankEditor::Knob : public juce::Component
{
public:
    Knob(juce::AudioProcessorValueTreeState& apvts, LookAndFeel& feel,
         const juce::String& paramId, const juce::String& labelText, bool heroKnob,
         std::function<juce::String(float)> formatter)
        : format(std::move(formatter)), hero(heroKnob), lookAndFeel(&feel)
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
        slider.getProperties().set("hero", hero ? 1 : 0);

        name.setText(labelText, juce::dontSendNotification);
        name.setJustificationType(juce::Justification::centred);
        name.setInterceptsMouseClicks(false, false);
        name.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        name.setColour(juce::Label::textColourId, juce::Colour(kMuted));

        value.setJustificationType(juce::Justification::centred);
        value.setInterceptsMouseClicks(false, false);
        value.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        value.setColour(juce::Label::textColourId, juce::Colour(kText));

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
        if (lookAndFeel == nullptr)
            return;
        name.setFont(lookAndFeel->regularFont((hero ? 12.0f : 11.0f) * scale));
        value.setFont(lookAndFeel->mediumFont((hero ? 22.0f : 13.0f) * scale));
    }

    void advance()
    {
        const float target = slider.isMouseOverOrDragging() ? 1.0f : 0.0f;
        if (std::abs(target - hover) < 0.002f)
        {
            if (std::abs(hover - target) >= 1.0e-6f)
            {
                hover = target;
                slider.getProperties().set("hover", hover);
                slider.repaint();
            }
            return;
        }

        hover += (target - hover) * 0.22f;
        slider.getProperties().set("hover", hover);
        slider.repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const int nameH = juce::jmax(14, juce::roundToInt(getHeight() * (hero ? 0.12f : 0.18f)));
        const int valueH = juce::jmax(16, juce::roundToInt(getHeight() * (hero ? 0.16f : 0.20f)));
        name.setBounds(area.removeFromTop(nameH));
        value.setBounds(area.removeFromBottom(valueH));
        slider.setBounds(area.reduced(hero ? 4 : 8, 0));
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
    bool hero = false;
    float hover = 0.0f;
    LookAndFeel* lookAndFeel = nullptr;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class SpringTankEditor::Meter : public juce::Component
{
public:
    explicit Meter(juce::String labelText, LookAndFeel& feel)
        : label(std::move(labelText)), lookAndFeel(&feel)
    {
    }

    void setLevels(float left, float right)
    {
        const float nextL = juce::jmax(left, displayL * 0.84f);
        const float nextR = juce::jmax(right, displayR * 0.84f);
        if (std::abs(nextL - displayL) <= 1.0e-4f && std::abs(nextR - displayR) <= 1.0e-4f)
            return;
        displayL = nextL;
        displayR = nextR;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colour(kMuted));
        if (lookAndFeel != nullptr)
            g.setFont(lookAndFeel->regularFont(juce::jmax(10.0f, area.getHeight() * 0.42f)));
        auto nameCol = area.removeFromLeft(juce::jmax(22.0f, area.getWidth() * 0.08f));
        g.drawFittedText(label, nameCol.toNearestInt(), juce::Justification::centredLeft, 1);

        auto drawBar = [&g](juce::Rectangle<float> row, float level)
        {
            g.setColour(juce::Colour(kLine));
            g.fillRoundedRectangle(row, row.getHeight() * 0.5f);
            const float clamped = juce::jlimit(0.0f, 1.0f, level);
            if (clamped <= 0.001f)
                return;
            g.setColour(juce::Colour(kAccent).withAlpha(0.9f));
            g.fillRoundedRectangle(row.withWidth(juce::jmax(2.0f, row.getWidth() * clamped)),
                                   row.getHeight() * 0.5f);
        };

        const float h = juce::jmax(2.0f, area.getHeight() * 0.28f);
        const float gap = juce::jmax(3.0f, area.getHeight() * 0.18f);
        const float y = area.getCentreY() - (h * 2.0f + gap) * 0.5f;
        drawBar({ area.getX(), y, area.getWidth(), h }, displayL);
        drawBar({ area.getX(), y + h + gap, area.getWidth(), h }, displayR);
    }

private:
    juce::String label;
    LookAndFeel* lookAndFeel = nullptr;
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
    constrainer.setSizeLimits(630, 315, 1680, 840);
    setConstrainer(&constrainer);
    setResizable(true, true);

    struct Spec
    {
        const char* id;
        const char* label;
        bool hero;
        std::function<juce::String(float)> format;
    };

    const Spec specs[7] = {
        { "decay", "Decay", true, [](float v) { return juce::String(v, 2) + " s"; } },
        { "tension", "Tension", false, [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "tone", "Tone", false, [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "drip", "Drip", false, [](float v) { return juce::String(juce::roundToInt(v * 100.0f)); } },
        { "predelay", "Pre-delay", false, [](float v) { return juce::String(v, 1) + " ms"; } },
        { "mix", "Mix", true, [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + "%"; } },
        { "output", "Output", false, [](float v) { return juce::String(v, 1) + " dB"; } }
    };

    auto& apvts = processor.getApvts();
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        knobs[i] = std::make_unique<Knob>(apvts, *lookAndFeel, specs[i].id, specs[i].label,
                                          specs[i].hero, specs[i].format);
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
            processor.selectProgramFromEditor(id - 1);
    };
    addAndMakeVisible(presetBox);

    presetPrev.setComponentID("preset-prev");
    presetNext.setComponentID("preset-next");
    presetPrev.setLookAndFeel(lookAndFeel.get());
    presetNext.setLookAndFeel(lookAndFeel.get());
    presetPrev.setColour(juce::TextButton::textColourOffId, juce::Colour(kMuted));
    presetNext.setColour(juce::TextButton::textColourOffId, juce::Colour(kMuted));
    presetPrev.onClick = [this] { stepPreset(-1); };
    presetNext.onClick = [this] { stepPreset(1); };
    addAndMakeVisible(presetPrev);
    addAndMakeVisible(presetNext);

    power.setComponentID("bypass");
    power.setLookAndFeel(lookAndFeel.get());
    power.setClickingTogglesState(true);
    power.setTooltip("Bypass");
    powerAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor.getApvts(), "bypass", power);
    addAndMakeVisible(power);

    inputMeterOwned = std::make_unique<Meter>("In", *lookAndFeel);
    outputMeterOwned = std::make_unique<Meter>("Out", *lookAndFeel);
    inputMeter = inputMeterOwned.get();
    outputMeter = outputMeterOwned.get();
    inputMeter->setComponentID("meter-in");
    outputMeter->setComponentID("meter-out");
    addAndMakeVisible(*inputMeter);
    addAndMakeVisible(*outputMeter);

    startTimerHz(30);
    const int savedWidth = processor.getEditorWidth();
    setSize(savedWidth, savedWidth / 2);
}

SpringTankEditor::~SpringTankEditor()
{
    stopTimer();
    setConstrainer(nullptr);
    presetBox.setLookAndFeel(nullptr);
    presetPrev.setLookAndFeel(nullptr);
    presetNext.setLookAndFeel(nullptr);
    power.setLookAndFeel(nullptr);
    detachLookAndFeel();
    setLookAndFeel(nullptr);
}

void SpringTankEditor::detachLookAndFeel()
{
    for (auto& knob : knobs)
        if (knob != nullptr)
            knob->detachLookAndFeel();
}

void SpringTankEditor::timerCallback()
{
    phase += 0.035f;
    for (auto& knob : knobs)
        if (knob != nullptr)
            knob->advance();

    const float inL = processor.takeInputPeak(0);
    const float inR = processor.takeInputPeak(1);
    const float outL = processor.takeOutputPeak(0);
    const float outR = processor.takeOutputPeak(1);
    shownOutput = juce::jmax(outL, outR);
    if (inputMeter != nullptr)
        inputMeter->setLevels(inL, inR);
    if (outputMeter != nullptr)
        outputMeter->setLevels(outL, outR);

    syncPresetBox();
    repaint(waveArea);
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
    processor.selectProgramFromEditor(index);
}

float SpringTankEditor::uiScale() const noexcept
{
    return juce::jmax(0.5f, static_cast<float>(getWidth()) / static_cast<float>(kWidth));
}

void SpringTankEditor::paint(juce::Graphics& g)
{
    const float scale = uiScale();
    g.fillAll(juce::Colour(kBg));

    juce::ColourGradient wash(juce::Colour(0xff12141a),
                              static_cast<float>(getWidth()) * 0.5f,
                              static_cast<float>(getHeight()) * 0.42f,
                              juce::Colour(kBg),
                              0.0f, 0.0f, true);
    g.setGradientFill(wash);
    g.fillAll();

    const float decay = processor.getApvts().getRawParameterValue("decay") != nullptr
                            ? processor.getApvts().getRawParameterValue("decay")->load()
                            : 2.2f;
    const float decayNorm = juce::jlimit(0.0f, 1.0f, (decay - 0.15f) / 5.35f);
    const float energy = 0.25f + 0.75f * shownOutput;
    juce::Path wave;
    const float yBase = static_cast<float>(waveArea.getCentreY());
    const float amp = juce::jmin((6.0f + 8.0f * decayNorm) * scale * (0.45f + 0.55f * energy),
                                 static_cast<float>(waveArea.getHeight()) * 0.4f);
    const float x0 = static_cast<float>(waveArea.getX());
    const float span = static_cast<float>(waveArea.getWidth());
    const int steps = 48;
    for (int i = 0; i <= steps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float x = x0 + t * span;
        const float envelope = std::sin(t * juce::MathConstants<float>::pi);
        const float y = yBase + std::sin(t * 10.0f + phase) * amp * envelope * 0.35f;
        if (i == 0)
            wave.startNewSubPath(x, y);
        else
            wave.lineTo(x, y);
    }
    g.setColour(juce::Colour(kAccent).withAlpha(0.10f + 0.06f * energy));
    g.strokePath(wave, juce::PathStrokeType(1.25f * scale, juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));

    auto header = getLocalBounds().toFloat().reduced(28.0f * scale, 22.0f * scale).removeFromTop(36.0f * scale);
    g.setColour(juce::Colour(kText));
    g.setFont(lookAndFeel->mediumFont(18.0f * scale));
    auto title = header.removeFromLeft(150.0f * scale);
    g.drawFittedText("Spring Tank", title.toNearestInt(), juce::Justification::centredLeft, 1);
    g.setColour(juce::Colour(kMuted));
    g.setFont(lookAndFeel->regularFont(12.0f * scale));
    g.drawFittedText(JucePlugin_VersionString, header.removeFromLeft(48.0f * scale).toNearestInt(),
                     juce::Justification::centredLeft, 1);
}

void SpringTankEditor::resized()
{
    const float scale = uiScale();
    waveArea = juce::Rectangle<float>(getWidth() * 0.40f, getHeight() * 0.40f - 12.0f * scale,
                                      getWidth() * 0.20f, 24.0f * scale)
                   .getSmallestIntegerContainer()
                   .expanded(2);
    processor.setEditorWidth(getWidth());
    auto area = getLocalBounds().reduced(juce::roundToInt(28.0f * scale), juce::roundToInt(20.0f * scale));

    auto header = area.removeFromTop(juce::roundToInt(40.0f * scale));
    header.removeFromLeft(juce::roundToInt(200.0f * scale));
    power.setBounds(header.removeFromRight(juce::roundToInt(44.0f * scale))
                        .withSizeKeepingCentre(juce::roundToInt(40.0f * scale),
                                               juce::roundToInt(24.0f * scale)));
    header.removeFromRight(juce::roundToInt(8.0f * scale));
    const int step = juce::roundToInt(32.0f * scale);
    presetNext.setBounds(header.removeFromRight(step).withSizeKeepingCentre(step, step));
    presetPrev.setBounds(header.removeFromRight(step).withSizeKeepingCentre(step, step));
    header.removeFromRight(juce::roundToInt(8.0f * scale));
    presetBox.setBounds(header.withSizeKeepingCentre(juce::jmin(header.getWidth(), juce::roundToInt(220.0f * scale)),
                                                     juce::roundToInt(32.0f * scale)));

    area.removeFromTop(juce::roundToInt(8.0f * scale));
    auto meters = area.removeFromBottom(juce::roundToInt(28.0f * scale));
    const int gap = juce::roundToInt(28.0f * scale);
    auto inBounds = meters.removeFromLeft((meters.getWidth() - gap) / 2);
    meters.removeFromLeft(gap);
    if (inputMeter != nullptr)
        inputMeter->setBounds(inBounds);
    if (outputMeter != nullptr)
        outputMeter->setBounds(meters);

    area.removeFromBottom(juce::roundToInt(12.0f * scale));
    auto secondary = area.removeFromBottom(juce::roundToInt(118.0f * scale));
    auto hero = area;

    auto place = [scale](Knob* knob, juce::Rectangle<int> slot)
    {
        if (knob == nullptr)
            return;
        knob->setScale(scale);
        knob->setBounds(slot);
    };

    place(knobs[0].get(), hero.removeFromLeft(hero.getWidth() / 2).reduced(juce::roundToInt(12.0f * scale), 0));
    place(knobs[5].get(), hero.reduced(juce::roundToInt(12.0f * scale), 0));

    const int secondaryIndex[] = { 1, 2, 3, 4, 6 };
    const int slot = secondary.getWidth() / 5;
    for (int i = 0; i < 5; ++i)
        place(knobs[static_cast<size_t>(secondaryIndex[i])].get(),
              secondary.removeFromLeft(slot).reduced(juce::roundToInt(4.0f * scale), 0));
}
