#include "PluginEditor.h"

#include "Parameters.h"

using namespace rootnote;
using namespace rootnote::ui;

namespace
{
constexpr int kBlinkFrames = 24; // three blinks at 30 frames a second
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
    : AudioProcessorEditor(p), plugin(p), apvts(p.getApvts()), screen(p)
{
    setLookAndFeel(&lookAndFeel);
    addAndMakeVisible(plate);
    plate.setBounds(0, 0, kPlateWidth, kPlateHeight);

    // Header.
    preset.onStep = [this](int by) {
        const int count = RootnoteProcessor::numPresets();
        plugin.loadPreset((plugin.currentPreset() + by + count) % count);
    };
    preset.onMenu = [this] {
        juce::PopupMenu menu;
        for (int i = 0; i < RootnoteProcessor::numPresets(); ++i)
            menu.addItem(i + 1, RootnoteProcessor::presetName(i), true, i == plugin.currentPreset());
        showMenu(menu, preset, [this](int chosen) { plugin.loadPreset(chosen - 1); });
    };
    slotA.onPress = [this](const juce::ModifierKeys&) { plugin.selectAbSlot(0); };
    slotB.onPress = [this](const juce::ModifierKeys&) { plugin.selectAbSlot(1); };
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
                plugin.setKnobNote(NoteControl::cut, note);
                setParameter(ids::cutOn, 1.0);
            }
            else
            {
                plugin.setKnobNote(NoteControl::boost, note);
            }
        };
        place(*key, 24.0f + static_cast<float>(pc) * 46.545f, 318, 42, 56);
    }

    // Function keys.
    // Octave keys move the boost note (Alt: the cut note) by an octave, as Alt-click on a pitch key sets the cut.
    octaveDown.onPress = [this](const juce::ModifierKeys& mods) { shiftOctave(mods.isAltDown() ? NoteControl::cut : NoteControl::boost, -12); };
    octaveUp.onPress = [this](const juce::ModifierKeys& mods) { shiftOctave(mods.isAltDown() ? NoteControl::cut : NoteControl::boost, 12); };
    // Match cut toggles the cut: off when it is on, otherwise on at the detected hit. Without a hit it blinks.
    matchCut.onPress = [this](const juce::ModifierKeys&) {
        if (parameter(ids::cutOn) >= 0.5)
            setParameter(ids::cutOn, 0.0);
        else if (!plugin.matchCutToHit())
            matchBlink = kBlinkFrames;
    };
    tuneToKey.onPress = [this](const juce::ModifierKeys&) {
        if (!plugin.tuneBoostToKey())
            tuneBlink = kBlinkFrames;
    };
    keyLock.onPress = [this](const juce::ModifierKeys&) { toggleParameter(ids::keyLock); };
    octaveDown.setTooltip("Boost note an octave down (Alt: the cut note)");
    octaveUp.setTooltip("Boost note an octave up (Alt: the cut note)");
    matchCut.setTooltip("Cut on at the detected hit, or off. Blinks if no hit has been detected yet");
    tuneToKey.setTooltip("Move the boost to the scale note nearest the hit. Blinks if no hit has been detected yet");
    keyLock.setTooltip("MIDI notes snap to the key and scale");
    place(octaveDown, 24, 386, 62, 34);
    place(octaveUp, 90, 386, 62, 34);
    place(matchCut, 204, 386, 120, 34);
    place(tuneToKey, 328, 386, 120, 34);
    place(keyLock, 452, 386, 132, 34);

    // Page keys and encoders.
    for (int i = 0; i < kNumPages; ++i)
    {
        // A click selects the page; holding the key only peeks at it on the display.
        auto* key = pageKeys.add(new Key(pageName(i)));
        key->onStateChange = [this, i] { pageKeyChanged(i); };
        key->onPress = [this, i](const juce::ModifierKeys&) {
            if (!std::exchange(peeked, false))
                selectPage(i);
        };
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

    page = juce::jlimit(0, kNumPages - 1, plugin.storedEditorPage());
    screen.setPage(page);
    bindEncoders();

    const auto stored = plugin.storedEditorSize(); // read first: setting the limits already resizes
    setResizable(true, false); // hosts resize; the settings menu has sizes too
    setResizeLimits(kPlateWidth * 3 / 4, kPlateHeight * 3 / 4, kPlateWidth * 2, kPlateHeight * 2);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(kPlateWidth) / kPlateHeight);
    // An older editor may have stored a size below today's limits.
    const int width = juce::jlimit(getConstrainer()->getMinimumWidth(), getConstrainer()->getMaximumWidth(),
                                   stored.x > 0 ? stored.x : kDefaultWidth);
    setSize(width, width * kPlateHeight / kPlateWidth);

    seenRestores = plugin.restoreCount();
    plugin.setDisplayActive(true);
    startTimerHz(30);
    refresh();
}

RootnoteEditor::~RootnoteEditor()
{
    plugin.setDisplayActive(false);
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
    plugin.storeEditorSize(getWidth(), getHeight());
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
    newPage = juce::jlimit(0, kNumPages - 1, newPage);
    if (newPage == page)
        return;
    page = newPage;
    plugin.storeEditorPage(page);
    bindEncoders();
    screen.setPage(page);
    refresh();
}

void RootnoteEditor::pageKeyChanged(int index)
{
    const bool down = pageKeys[index]->isDown();
    if (down && peekKey != index)
    {
        peekKey = index;
        peekStartMs = juce::Time::getMillisecondCounter();
        screen.setPage(index);
    }
    else if (!down && peekKey == index)
    {
        // Comes before the click of the same release.
        peekKey = -1;
        peeked = juce::Time::getMillisecondCounter() - peekStartMs >= kPagePeekMs;
        // The click of a release over the key comes in this same mouse-up; released elsewhere
        // there is none, and the flag must not swallow the next click.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<RootnoteEditor>(this)] {
            if (safe != nullptr)
                safe->peeked = false;
        });
        screen.setPage(page);
    }
}

void RootnoteEditor::bindEncoders()
{
    // Unbind everything first: a binding ends its drag, and a new one takes over the range and texts.
    for (auto& binding : bindings)
        binding.reset();

    // The encoders take the page's slots that have a knob, in order.
    const auto& slots = pageSlots(page);
    int next = 0;
    for (int i = 0; i < kNumEncoders; ++i)
    {
        Encoder& encoder = encoders[i];
        while (next < kNumEncoders && (slots[static_cast<size_t>(next)].id == nullptr || slots[static_cast<size_t>(next)].readoutOnly))
            ++next;
        encoder.setVisible(next < kNumEncoders);
        encoder.onValueEntry = nullptr;
        if (next == kNumEncoders)
            continue;

        const Slot& slot = slots[static_cast<size_t>(next)];
        encoderSlot[i] = next++;
        encoder.setCap(capColour(slot.cap), slot.label);
        bindings[i] = std::make_unique<EncoderBinding>(*apvts.getParameter(slot.id), encoder, slot.cents);
        encoder.onValueEntry = [this, i] { screen.showEntry(encoderSlot[i]); };
        if (std::strcmp(slot.id, ids::focus) == 0)
        {
            // Focus does nothing while it follows the note: turning it takes over from the link.
            encoder.onValueChange = [this, bound = encoder.onValueChange] {
                if (parameter(ids::focusLink) >= 0.5)
                    setParameter(ids::focusLink, 0.0);
                bound();
            };
        }
    }
}

void RootnoteEditor::shiftOctave(NoteControl::Target bank, int semitones)
{
    // From the note that sounds: a held MIDI note, or else the knob. The press takes the bank back from MIDI.
    Played& p = played[bank];
    const double from = p.midi ? p.note : parameter(bank == NoteControl::boost ? ids::boostNote : ids::cutNote);
    p.midi = false;
    // Until a frame shows the knob back (or new MIDI), older frames still show the MIDI note: refresh() keeps this.
    p.takeoverAtNoteOns = lastNoteOns;
    p.takeoverPending = true;
    plugin.setKnobNote(bank, juce::jlimit(kMinNote, kMaxNote, from + semitones));
}

void RootnoteEditor::showMenu(juce::PopupMenu menu, juce::Component& target, std::function<void(int)> onChosen)
{
    // The choice arrives asynchronously, possibly after the editor has gone.
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [safe = juce::Component::SafePointer<RootnoteEditor>(this), apply = std::move(onChosen)](int chosen) {
                           if (safe != nullptr && chosen != 0)
                               apply(chosen);
                       });
}

void RootnoteEditor::showSettings()
{
    static constexpr int kSizes[] = { 75, 100, 125, 150, 200 };
    enum : int { constantQId = 100, constantWidthId, holdId, returnId };

    juce::PopupMenu sizes;
    for (int i = 0; i < static_cast<int>(std::size(kSizes)); ++i)
        sizes.addItem(i + 1, juce::String(kSizes[i]) + "%", true, getWidth() == kPlateWidth * kSizes[i] / 100);

    juce::PopupMenu menu;
    menu.addSectionHeader("Rootnote " JucePlugin_VersionString);
    menu.addSubMenu("UI size", sizes);
    const bool constantQ = parameter(ids::qMode) < 0.5;
    menu.addItem(constantQId, "Constant Q", true, constantQ);
    menu.addItem(constantWidthId, "Constant width", true, !constantQ);
    menu.addSeparator();
    const bool hold = parameter(ids::noteOff) < 0.5;
    menu.addItem(holdId, "Note-off: hold", true, hold);
    menu.addItem(returnId, "Note-off: return to knob", true, !hold);

    showMenu(menu, settings, [this](int chosen) {
        switch (chosen)
        {
            case constantQId:     setParameter(ids::qMode, 0.0); break;
            case constantWidthId: setParameter(ids::qMode, 1.0); break;
            case holdId:          setParameter(ids::noteOff, 0.0); break;
            case returnId:        setParameter(ids::noteOff, 1.0); break;
            default:
            {
                const int percent = kSizes[juce::jlimit(0, static_cast<int>(std::size(kSizes)) - 1, chosen - 1)];
                setSize(kPlateWidth * percent / 100, kPlateHeight * percent / 100);
            }
        }
    });
}

void RootnoteEditor::applyRestoredState()
{
    // A text field or a drag from before belongs to the old settings: drop it, then show the restored page and size.
    screen.cancelEntry();
    page = juce::jlimit(0, kNumPages - 1, plugin.storedEditorPage());
    screen.setPage(page);
    bindEncoders();
    const auto stored = plugin.storedEditorSize();
    if (stored.x > 0)
    {
        const int width = juce::jlimit(getConstrainer()->getMinimumWidth(), getConstrainer()->getMaximumWidth(), stored.x);
        setSize(width, width * kPlateHeight / kPlateWidth);
    }
}

void RootnoteEditor::refresh()
{
    if (const auto restores = plugin.restoreCount(); restores != seenRestores)
    {
        seenRestores = restores;
        applyRestoredState();
    }

    const auto state = plugin.displayState();
    const auto hit = plugin.tunerReading();
    screen.update(state, hit);

    if (state.midiNoteOns != lastNoteOns)
        midiLit = 6;
    midiLit = juce::jmax(0, midiLit - 1);
    const float peak = juce::jmax(plugin.takeOutputPeak(0), plugin.takeOutputPeak(1));
    meterDb = juce::jmax(juce::Decibels::gainToDecibels(peak, -100.0f), meterDb - 1.5f);

    // Keys follow the parameters, whoever changed them (host, preset, MIDI).
    // An octave press stays until the audio thread has taken the bank back, or newer MIDI arrives.
    const auto follow = [&state](Played& p, double note, bool midi) {
        if (p.takeoverPending && midi && state.midiNoteOns == p.takeoverAtNoteOns)
            return;
        p = { note, midi };
    };
    follow(played[NoteControl::boost], state.boostNote, state.boostFromMidi);
    follow(played[NoteControl::cut], state.cutNote, state.cutFromMidi);
    lastNoteOns = state.midiNoteOns;
    const int boostNote = juce::roundToInt(state.boostNote);
    const int newOctave = juce::jlimit(0, 7, boostNote / 12 - 1);
    if (newOctave != octave)
    {
        octave = newOctave;
        plate.repaint(kOctaveArea);
    }
    const int key = juce::roundToInt(parameter(ids::key));
    const auto scale = static_cast<Scale>(juce::roundToInt(parameter(ids::scale)));
    const int boostPc = boostNote % 12;
    const bool cutOn = parameter(ids::cutOn) >= 0.5;
    const int cutPc = cutOn ? juce::roundToInt(state.cutNote) % 12 : -1;
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
    const auto blink = [](int& frames, Key::Led steady) {
        if (frames == 0)
            return steady;
        return --frames / 4 % 2 == 0 ? Key::Led::off : Key::Led::on;
    };
    matchCut.setLed(blink(matchBlink, led(cutOn)));
    tuneToKey.setLed(blink(tuneBlink, Key::Led::none));
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

    const int presetIndex = plugin.currentPreset();
    preset.set(presetIndex, RootnoteProcessor::presetName(presetIndex).toLowerCase());
    slotA.setLook(plugin.abSlot() == 0 ? Key::Look::lcdOn : Key::Look::lcd);
    slotB.setLook(plugin.abSlot() == 1 ? Key::Look::lcdOn : Key::Look::lcd);
    power.setLit(parameter(ids::bypass) < 0.5);

    Footer now;
    now.midiMode = juce::roundToInt(parameter(ids::midiMode));
    now.boostLit = state.boostFromMidi || midiLit > 0;
    now.cutLit = state.cutFromMidi;
    now.hold = parameter(ids::noteOff) < 0.5;
    now.constantQ = parameter(ids::qMode) < 0.5;
    now.glide = parameter(ids::glide);
    now.mix = parameter(ids::mix);
    now.output = parameter(ids::output);
    now.a4 = parameter(ids::a4);
    now.meterLeds = juce::jlimit(0, 14, juce::roundToInt((meterDb + 42.0f) / 3.0f)); // 14 LEDs, 3 dB each
    if (now != footer)
    {
        footer = now;
        plate.repaint(kFooterArea);
    }
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
    const Footer& f = footer;
    float x = panel(24.0f, 411.0f, "MIDI IN");
    if (f.midiMode == 1)
    {
        x = drawTextAt(g, "ch1" + kArrow + "boost", value, colours::ink, dot(x, f.boostLit), rowY) + 14.0f;
        x = drawTextAt(g, "ch2" + kArrow + "cut", value, colours::ink, dot(x, f.cutLit), rowY) + 14.0f;
    }
    else if (f.midiMode == 2)
    {
        x = drawTextAt(g, "omni" + kArrow + "boost", value, colours::ink, dot(x, f.boostLit), rowY) + 14.0f;
    }
    else
    {
        x = drawTextAt(g, "midi off", value, colours::ink, x, rowY) + 14.0f;
    }
    drawTextAt(g, "glide " + number(f.glide, 0) + " ms" + kDot + (f.hold ? "hold" : "return"), value, colours::sub, x, rowY);

    // Mix and output, with the output meter.
    x = panel(447.0f, 293.0f, "MIX" + kDot + "OUTPUT");
    x = drawTextAt(g, number(f.mix, 0) + "%", value, colours::ink, x, rowY) + 14.0f;
    x = drawTextAt(g, number(f.output, 1, 1) + " db", value, colours::ink, x, rowY) + 14.0f;
    for (int i = 0; i < 14; ++i)
        drawLed(g, { x + 2.5f + static_cast<float>(i) * 8.0f, rowY }, i < f.meterLeds ? colours::orange : colours::ledOff);

    // Tuning reference and Q mode.
    x = panel(752.0f, 184.0f, "A4" + kDot + "Q MODE");
    drawTextAt(g, number(f.a4, 1) + " hz" + kDot + (f.constantQ ? "const q" : "const width"), value, colours::ink, x, rowY);
}
