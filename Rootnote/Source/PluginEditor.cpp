#include "PluginEditor.h"

#include "Parameters.h"

using namespace rootnote;
using namespace rootnote::ui;

namespace
{
const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");
const juce::String kArrow = juce::String::fromUTF8(" \xe2\x86\x92 ");

constexpr float kEncoderX = 646.0f, kEncoderY = 150.0f, kEncoderDx = 83.0f, kEncoderDy = 112.0f;
const juce::Rectangle<int> kFooterArea { 24, 448, 912, 58 };
const juce::Rectangle<int> kOctaveArea { 156, 386, 44, 34 };

bool isSharp(int pc)
{
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
} // namespace

RootnoteEditor::RootnoteEditor(RootnoteProcessor& p)
    : AudioProcessorEditor(p), processor(p), apvts(p.getApvts()), screen(p)
{
    setLookAndFeel(&lookAndFeel);
    addAndMakeVisible(plate);
    plate.setBounds(0, 0, kPlateWidth, kPlateHeight);

    // Header.
    preset.onStep = [this](int by) {
        const int count = RootnoteProcessor::numPresets();
        processor.loadPreset((processor.currentPreset() + by + count) % count);
    };
    preset.onMenu = [this] {
        juce::PopupMenu menu;
        for (int i = 0; i < RootnoteProcessor::numPresets(); ++i)
            menu.addItem(i + 1, RootnoteProcessor::presetName(i), true, i == processor.currentPreset());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&preset), [this](int chosen) {
            if (chosen > 0)
                processor.loadPreset(chosen - 1);
        });
    };
    slotA.onPress = [this](const juce::ModifierKeys&) { processor.selectAbSlot(0); };
    slotB.onPress = [this](const juce::ModifierKeys&) { processor.selectAbSlot(1); };
    settings.onClick = [this] { showSettings(); };
    power.onClick = [this] { toggleParameter(ids::bypass); };
    power.setTooltip("Bypass");
    place(preset, 370, 9, 220, 26);
    place(slotA, 598, 9, 26, 26);
    place(slotB, 627, 9, 26, 26);
    place(settings, 856, 9, 26, 26);
    place(power, 910, 9, 26, 26);
    place(screen, 24, 52, 560, 252);

    // Pitch keys: click sets the boost note, Alt-click the cut note.
    for (int pc = 0; pc < 12; ++pc)
    {
        auto* key = pitchKeys.add(new Key(noteText(pc + 12, true).dropLastCharacters(1), 11.0f, 0.02f));
        key->onPress = [this, pc](const juce::ModifierKeys& mods) {
            const double note = juce::jlimit(kMinNote, kMaxNote, static_cast<double>((octave + 1) * 12 + pc));
            if (mods.isAltDown())
            {
                setParameter(ids::cutNote, note);
                setParameter(ids::cutOn, 1.0);
            }
            else
            {
                setParameter(ids::boostNote, note);
            }
        };
        place(*key, 24.0f + static_cast<float>(pc) * 46.545f, 318, 42, 56);
    }

    // Function keys.
    octaveDown.onPress = [this](const juce::ModifierKeys&) { octave = juce::jmax(0, octave - 1); plate.repaint(kOctaveArea); };
    octaveUp.onPress = [this](const juce::ModifierKeys&) { octave = juce::jmin(7, octave + 1); plate.repaint(kOctaveArea); };
    matchCut.onPress = [this](const juce::ModifierKeys&) { processor.matchCutToHit(); };
    tuneToKey.onPress = [this](const juce::ModifierKeys&) { processor.tuneBoostToKey(); };
    keyLock.onPress = [this](const juce::ModifierKeys&) { toggleParameter(ids::keyLock); };
    matchCut.setTooltip("Put the cut on the detected hit");
    tuneToKey.setTooltip("Move the boost to the scale note nearest the hit");
    place(octaveDown, 24, 386, 62, 34);
    place(octaveUp, 90, 386, 62, 34);
    place(matchCut, 204, 386, 120, 34);
    place(tuneToKey, 328, 386, 120, 34);
    place(keyLock, 452, 386, 132, 34);

    // Page keys and encoders.
    for (int i = 0; i < kNumPages; ++i)
    {
        auto* key = pageKeys.add(new Key(pageName(i)));
        key->onPress = [this, i](const juce::ModifierKeys&) { selectPage(i); };
        place(*key, 604.0f + static_cast<float>(i) * 67.0f, 52, 62, 34);
    }
    for (int i = 0; i < kNumEncoders; ++i)
    {
        const float cx = kEncoderX + static_cast<float>(i % 4) * kEncoderDx;
        const float cy = kEncoderY + static_cast<float>(i / 4) * kEncoderDy;
        plate.addChildComponent(encoders[i]);
        encoders[i].setBounds(juce::roundToInt(cx - 40.0f), juce::roundToInt(cy - 30.0f), 80, 80);
    }

    // Series, Focus link and limiter.
    static const char* const series[] = { "harm", "oct", "drum" };
    for (int i = 0; i < 3; ++i)
    {
        auto* key = seriesKeys.add(new Key(series[i]));
        key->onPress = [this, i](const juce::ModifierKeys&) { setParameter(ids::series, i); };
        place(*key, 604.0f + static_cast<float>(i) * 66.0f, 390, 62, 30);
    }
    link.onPress = [this](const juce::ModifierKeys&) { toggleParameter(ids::focusLink); };
    limit.onPress = [this](const juce::ModifierKeys&) { toggleParameter(ids::limiter); };
    link.setTooltip("Focus follows the boost note");
    place(link, 810, 390, 62, 30);
    place(limit, 876, 390, 60, 30);

    octave = juce::jlimit(0, 7, juce::roundToInt(parameter(ids::boostNote)) / 12 - 1);
    bindEncoders();

    const auto stored = processor.storedEditorSize(); // read first: setting the limits already resizes
    setResizable(true, false); // hosts resize; the settings menu has sizes too
    setResizeLimits(kPlateWidth * 3 / 4, kPlateHeight * 3 / 4, kPlateWidth * 2, kPlateHeight * 2);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(kPlateWidth) / kPlateHeight);
    const int width = stored.x > 0 ? stored.x : kDefaultWidth;
    setSize(width, width * kPlateHeight / kPlateWidth);

    processor.setDisplayActive(true);
    startTimerHz(30);
    refresh();
}

RootnoteEditor::~RootnoteEditor()
{
    processor.setDisplayActive(false);
    setLookAndFeel(nullptr);
}

void RootnoteEditor::place(juce::Component& component, float x, float y, float w, float h)
{
    plate.addAndMakeVisible(component);
    // Keys get one extra pixel below for their drop line.
    const float extra = dynamic_cast<Key*>(&component) != nullptr ? 1.0f : 0.0f;
    component.setBounds(juce::Rectangle<float>(x, y, w, h + extra).toNearestInt());
}

void RootnoteEditor::resized()
{
    plate.setTransform(juce::AffineTransform::scale(static_cast<float>(getWidth()) / kPlateWidth,
                                                    static_cast<float>(getHeight()) / kPlateHeight));
    processor.storeEditorSize(getWidth(), getHeight());
}

double RootnoteEditor::parameter(const char* id) const
{
    return static_cast<double>(apvts.getRawParameterValue(id)->load());
}

void RootnoteEditor::setParameter(const char* id, double value)
{
    auto* p = apvts.getParameter(id);
    p->beginChangeGesture();
    p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(value)));
    p->endChangeGesture();
}

void RootnoteEditor::toggleParameter(const char* id)
{
    setParameter(id, parameter(id) >= 0.5 ? 0.0 : 1.0);
}

void RootnoteEditor::selectPage(int newPage)
{
    page = juce::jlimit(0, kNumPages - 1, newPage);
    bindEncoders();
    screen.setPage(page);
    refresh();
}

void RootnoteEditor::bindEncoders()
{
    // Detach everything first: a new attachment takes over its slider's range and value.
    for (auto& attachment : attachments)
        attachment.reset();
    centsAttachment.reset();

    const auto& slots = pageSlots(page);
    for (int i = 0; i < kNumEncoders; ++i)
    {
        const Slot& slot = slots[static_cast<size_t>(i)];
        Encoder& encoder = encoders[i];
        encoder.onValueChange = nullptr;
        encoder.onDragStart = nullptr;
        encoder.onDragEnd = nullptr;
        encoder.setVisible(slot.id != nullptr);
        if (slot.id == nullptr)
            continue;

        encoder.setCap(capColour(slot.cap), slot.label);
        auto& param = *apvts.getParameter(slot.id);
        if (slot.cents)
        {
            // The cents of the cut note: -50..+50 around the semitone it had when the drag began.
            encoder.snapToInteger = false;
            encoder.setNormalisableRange({ -50.0, 50.0, 0.1 });
            centsAttachment = std::make_unique<juce::ParameterAttachment>(param, [&encoder](float note) {
                encoder.setValue((static_cast<double>(note) - std::round(static_cast<double>(note))) * 100.0,
                                 juce::dontSendNotification);
            });
            encoder.onDragStart = [this] {
                centsBase = std::round(parameter(ids::cutNote));
                centsAttachment->beginGesture();
            };
            encoder.onValueChange = [this, &encoder] {
                centsAttachment->setValueAsPartOfGesture(static_cast<float>(centsBase + encoder.getValue() / 100.0));
            };
            encoder.onDragEnd = [this] { centsAttachment->endGesture(); };
            encoder.setDoubleClickReturnValue(true, 0.0);
            centsAttachment->sendInitialUpdate();
        }
        else
        {
            encoder.snapToInteger = std::strcmp(slot.id, ids::cutNote) == 0;
            attachments[i] = std::make_unique<juce::SliderParameterAttachment>(param, encoder);
            encoder.setDoubleClickReturnValue(true, static_cast<double>(param.convertFrom0to1(param.getDefaultValue())));
        }
    }
}

void RootnoteEditor::showSettings()
{
    juce::PopupMenu sizes;
    for (const int percent : { 75, 100, 125, 150, 200 })
        sizes.addItem(percent == 100 ? "100%" : juce::String(percent) + "%", true,
                      getWidth() == kPlateWidth * percent / 100, [this, percent] {
                          setSize(kPlateWidth * percent / 100, kPlateHeight * percent / 100);
                      });

    juce::PopupMenu menu;
    menu.addSectionHeader("Rootnote " JucePlugin_VersionString);
    menu.addSubMenu("UI size", sizes);
    const bool constantQ = parameter(ids::qMode) < 0.5;
    menu.addItem("Constant Q", true, constantQ, [this] { setParameter(ids::qMode, 0.0); });
    menu.addItem("Constant width", true, !constantQ, [this] { setParameter(ids::qMode, 1.0); });
    menu.addSeparator();
    const bool hold = parameter(ids::noteOff) < 0.5;
    menu.addItem("Note-off: hold", true, hold, [this] { setParameter(ids::noteOff, 0.0); });
    menu.addItem("Note-off: return to knob", true, !hold, [this] { setParameter(ids::noteOff, 1.0); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&settings));
}

void RootnoteEditor::refresh()
{
    const auto state = processor.displayState();
    const auto hit = processor.tunerReading();
    screen.update(state, hit);

    if (state.midiNoteOns != lastNoteOns)
        midiLit = 6;
    lastNoteOns = state.midiNoteOns;
    midiLit = juce::jmax(0, midiLit - 1);
    boostFromMidi = state.boostFromMidi;
    cutFromMidi = state.cutFromMidi;
    const float peak = juce::jmax(processor.takeOutputPeak(0), processor.takeOutputPeak(1));
    meterDb = juce::jmax(juce::Decibels::gainToDecibels(peak, -100.0f), meterDb - 1.5f);

    // Keys follow the parameters, whoever changed them (host, preset, MIDI).
    const double a4 = parameter(ids::a4);
    const int key = juce::roundToInt(parameter(ids::key));
    const auto scale = static_cast<Scale>(juce::roundToInt(parameter(ids::scale)));
    const int boostPc = (juce::roundToInt(hzToNote(state.boostHz, a4)) % 12 + 12) % 12;
    const bool cutOn = parameter(ids::cutOn) >= 0.5;
    const int cutPc = cutOn ? (juce::roundToInt(hzToNote(state.cutHz, a4)) % 12 + 12) % 12 : -1;
    for (int pc = 0; pc < 12; ++pc)
    {
        Key& k = *pitchKeys[pc];
        const bool dark = isSharp(pc);
        k.setLook(pc == boostPc ? Key::Look::filled
                                : (pc == cutPc ? Key::Look::outlined : (dark ? Key::Look::dark : Key::Look::light)));
        const bool light = !dark && pc != boostPc;
        k.setUnderline(isInScale(pc, key, scale) ? (light ? colours::ink : colours::white) : colours::outOfKey);
    }

    const auto led = [](bool on) { return on ? Key::Led::on : Key::Led::off; };
    matchCut.setLed(led(hit.valid));
    keyLock.setLed(led(parameter(ids::keyLock) >= 0.5));
    link.setLed(led(parameter(ids::focusLink) >= 0.5));
    limit.setLed(led(parameter(ids::limiter) >= 0.5));
    const int series = juce::roundToInt(parameter(ids::series));
    for (int i = 0; i < seriesKeys.size(); ++i)
    {
        seriesKeys[i]->setLook(i == series ? Key::Look::dark : Key::Look::light);
        seriesKeys[i]->setLed(i == series ? Key::Led::on : Key::Led::none);
    }
    for (int i = 0; i < pageKeys.size(); ++i)
    {
        pageKeys[i]->setLook(i == page ? Key::Look::dark : Key::Look::light);
        pageKeys[i]->setLed(i == page ? Key::Led::on : Key::Led::none);
    }

    const int presetIndex = processor.currentPreset();
    preset.set(presetIndex, RootnoteProcessor::presetName(presetIndex).toLowerCase());
    slotA.setLook(processor.abSlot() == 0 ? Key::Look::lcdOn : Key::Look::lcd);
    slotB.setLook(processor.abSlot() == 1 ? Key::Look::lcdOn : Key::Look::lcd);
    power.setLit(parameter(ids::bypass) < 0.5);

    plate.repaint(kFooterArea);
}

void RootnoteEditor::paintPlate(juce::Graphics& g)
{
    g.fillAll(colours::face);

    // Header.
    const float x = drawTextAt(g, "rootnote", mono(13.0f, Weight::semiBold), colours::ink, 24.0f, 22.0f);
    drawTextAt(g, "RN-1" + kDot + "NOTE RESONATOR", labelFont(), colours::sub, x + 10.0f, 22.0f);
    drawTextAt(g, "SETTINGS", labelFont(), colours::sub, 846.0f - textWidth(labelFont(), "SETTINGS"), 22.0f);
    drawTextAt(g, "ON", labelFont(), colours::sub, 892.0f, 22.0f);

    // The screen's lower edge catches the light.
    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.fillRoundedRectangle(juce::Rectangle<float>(24.0f, 53.0f, 560.0f, 252.0f), 10.0f);

    g.setFont(mono(12.0f));
    g.setColour(colours::ink);
    g.drawText(juce::String(octave), kOctaveArea, juce::Justification::centred, false);
    drawTextAt(g, "SERIES", labelFont(), colours::sub, 604.0f, 381.5f);

    paintFooter(g);

    drawTextAt(g, "DRIPWORKS", labelFont(), colours::sub, 24.0f, 521.5f);
    const juce::String hint = "CLICK A KEY = BOOST" + kDot + "ALT-CLICK = CUT";
    drawTextAt(g, hint, labelFont(), colours::sub, 936.0f - textWidth(labelFont(), hint), 521.5f);
}

void RootnoteEditor::paintFooter(juce::Graphics& g)
{
    const auto value = mono(11.0f);
    constexpr float labelY = 465.5f, rowY = 484.0f;
    const auto panel = [&g](float left, float width, const juce::String& label) {
        const juce::Rectangle<float> box { left, 448.0f, width, 56.0f };
        g.setColour(colours::panel.interpolatedWith(juce::Colours::black, 0.12f));
        g.fillRoundedRectangle(box, 8.0f);
        g.setColour(colours::panel);
        g.fillRoundedRectangle(box.withTrimmedTop(1.5f), 8.0f);
        drawTextAt(g, label, labelFont(), colours::sub, left + 12.0f, labelY);
        return left + 12.0f;
    };
    const auto dot = [&g](float at, bool lit) {
        g.setColour(lit ? colours::orange : colours::ledOff);
        g.fillEllipse(juce::Rectangle<float>(6.5f, 6.5f).withCentre({ at + 3.25f, rowY }));
        return at + 14.0f;
    };

    // MIDI in.
    float x = panel(24.0f, 411.0f, "MIDI IN");
    const int mode = juce::roundToInt(parameter(ids::midiMode));
    const bool active = midiLit > 0;
    if (mode == 1)
    {
        x = drawTextAt(g, "ch1" + kArrow + "boost", value, colours::ink, dot(x, boostFromMidi || active), rowY) + 14.0f;
        x = drawTextAt(g, "ch2" + kArrow + "cut", value, colours::ink, dot(x, cutFromMidi), rowY) + 14.0f;
    }
    else if (mode == 2)
    {
        x = drawTextAt(g, "omni" + kArrow + "boost", value, colours::ink, dot(x, boostFromMidi || active), rowY) + 14.0f;
    }
    else
    {
        x = drawTextAt(g, "midi off", value, colours::ink, x, rowY) + 14.0f;
    }
    drawTextAt(g, "glide " + number(parameter(ids::glide), 0) + " ms" + kDot
                      + (parameter(ids::noteOff) < 0.5 ? "hold" : "return"),
               value, colours::sub, x, rowY);

    // Mix and output, with the output meter: 14 LEDs, 3 dB each.
    x = panel(447.0f, 293.0f, "MIX" + kDot + "OUTPUT");
    x = drawTextAt(g, number(parameter(ids::mix), 0) + "%", value, colours::ink, x, rowY) + 14.0f;
    x = drawTextAt(g, number(parameter(ids::output), 1, 1) + " db", value, colours::ink, x, rowY) + 14.0f;
    const int lit = juce::jlimit(0, 14, juce::roundToInt((meterDb + 42.0f) / 3.0f));
    for (int i = 0; i < 14; ++i)
        drawLed(g, { x + 2.5f + static_cast<float>(i) * 8.0f, rowY }, i < lit ? colours::orange : colours::ledOff);

    // Tuning reference and Q mode.
    x = panel(752.0f, 184.0f, "A4" + kDot + "Q MODE");
    drawTextAt(g, number(parameter(ids::a4), 1) + " hz" + kDot + (parameter(ids::qMode) < 0.5 ? "const q" : "const width"),
               value, colours::ink, x, rowY);
}
