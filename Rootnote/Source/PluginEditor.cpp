#include "PluginEditor.h"

using namespace rootnote;

namespace
{
constexpr float kLowHz = 16.0f;
constexpr float kHighHz = 20000.0f;
constexpr float kBandRangeDb = 30.0f; // a band of this gain spans half the plot

const juce::Colour kBackground { 0xff0b0b0f };
const juce::Colour kPanel { 0xff0f0f14 };
const juce::Colour kBoost { 0xffffb547 };
const juce::Colour kCut { 0xffff5d6e };

float toDb(float gain)
{
    return juce::Decibels::gainToDecibels(gain, -90.0f);
}

juce::String describe(double hz, double a4)
{
    return juce::String(formatNote(std::round(hzToNote(hz, a4) * 100.0) / 100.0)) + "  " + juce::String(hz, 1) + " Hz";
}
} // namespace

FeedView::FeedView(RootnoteProcessor& p)
    : processor(p)
{
    std::fill(std::begin(spectrumDb), std::end(spectrumDb), kFloorDb);
    setOpaque(true);
}

void FeedView::refresh(const RootnoteProcessor::DisplayState& newState)
{
    state = newState;

    float block[512];
    bool heard = false;
    for (size_t count = 0; (count = processor.readSpectrumSamples(block, std::size(block))) > 0; heard = true)
        for (size_t i = 0; i < count; ++i)
            recent[writeAt++ & (kFftSize - 1)] = block[i];

    // Without new audio (the host stopped) the old picture fades instead of freezing.
    if (heard)
        analyse();
    else
        for (float& db : spectrumDb)
            db = juce::jmax(kFloorDb, db - 3.0f);

    for (int ch = 0; ch < 2; ++ch)
        peakDb[ch] = juce::jmax(toDb(processor.takeOutputPeak(ch)), peakDb[ch] - 1.5f);

    repaint();
}

void FeedView::analyse()
{
    for (size_t i = 0; i < kFftSize; ++i)
    {
        const double hann = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * static_cast<double>(i) / kFftSize);
        re[i] = hann * static_cast<double>(recent[(writeAt + i) & (kFftSize - 1)]);
        im[i] = 0.0;
    }
    fft.forward(re.data(), im.data());

    // Each point takes the loudest bin of its slice of the log axis; a full-scale sine reads 0 dB.
    const double binHz = processor.getSampleRate() / kFftSize;
    const double step = std::pow(static_cast<double>(kHighHz / kLowHz), 1.0 / kPoints);
    double lowHz = kLowHz;
    for (float& db : spectrumDb)
    {
        const int first = juce::jlimit(1, kFftSize / 2 - 1, juce::roundToInt(lowHz / binHz));
        const int last = juce::jlimit(first, kFftSize / 2 - 1, juce::roundToInt(lowHz * step / binHz));
        double power = 0.0;
        for (int bin = first; bin <= last; ++bin)
            power = juce::jmax(power, re[static_cast<size_t>(bin)] * re[static_cast<size_t>(bin)] + im[static_cast<size_t>(bin)] * im[static_cast<size_t>(bin)]);
        const float now = toDb(static_cast<float>(std::sqrt(power) * 4.0 / kFftSize));
        db = juce::jmax(now, db - 1.5f);
        lowHz *= step;
    }
}

void FeedView::paint(juce::Graphics& g)
{
    g.fillAll(kPanel);
    auto area = getLocalBounds().toFloat().reduced(6.0f);
    auto meters = area.removeFromRight(30.0f);
    area.removeFromRight(8.0f);

    const auto xOf = [&](double hz) {
        return area.getX() + area.getWidth() * static_cast<float>(std::log(hz / kLowHz) / std::log(kHighHz / kLowHz));
    };

    g.setFont(juce::FontOptions(10.0f));
    for (const auto& [hz, label] : { std::pair { 20.0, "20" }, { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                     { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" } })
    {
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(xOf(hz), area.getY(), 1.0f, area.getHeight());
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.drawText(label, juce::Rectangle<float>(xOf(hz) + 3.0f, area.getBottom() - 12.0f, 30.0f, 12.0f), juce::Justification::left);
    }

    curve.clear();
    for (int i = 0; i < kPoints; ++i)
    {
        const float x = area.getX() + area.getWidth() * (static_cast<float>(i) + 0.5f) / kPoints;
        const float y = area.getY() + area.getHeight() * spectrumDb[i] / kFloorDb;
        i == 0 ? curve.startNewSubPath(x, y) : curve.lineTo(x, y);
    }
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.strokePath(curve, juce::PathStrokeType(1.0f));

    // Bands: boosts rise from the middle line, cuts hang from it.
    const float middle = area.getCentreY();
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.fillRect(area.getX(), middle, area.getWidth(), 1.0f);
    const auto drawBands = [&](const BandView* bands, int count, juce::Colour colour) {
        g.setColour(colour);
        for (int i = 0; i < count; ++i)
        {
            if (bands[i].hz < kLowHz || bands[i].hz > kHighHz)
                continue;
            const float length = 0.5f * area.getHeight() * static_cast<float>(bands[i].gainDb) / kBandRangeDb;
            g.fillRect(juce::Rectangle<float>(xOf(bands[i].hz) - 1.5f, middle - juce::jmax(0.0f, length), 3.0f, std::abs(length)));
        }
    };
    drawBands(state.boostBands, state.numBoostBands, kBoost);
    drawBands(state.cutBands, state.numCutBands, kCut);

    // Output peaks (0 dB at the top, -60 dB at the bottom) and the limiter's reduction from the top.
    const auto bar = [&](float fraction, bool fromTop, juce::Colour colour) {
        auto slot = meters.removeFromLeft(8.0f);
        meters.removeFromLeft(3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.fillRect(slot);
        const float length = slot.getHeight() * juce::jlimit(0.0f, 1.0f, fraction);
        g.setColour(colour);
        g.fillRect(fromTop ? slot.removeFromTop(length) : slot.removeFromBottom(length));
    };
    bar(1.0f + peakDb[0] / 60.0f, false, juce::Colours::white.withAlpha(0.8f));
    bar(1.0f + peakDb[1] / 60.0f, false, juce::Colours::white.withAlpha(0.8f));
    bar(-toDb(state.limiterGain) / 24.0f, true, kCut);
}

RootnoteEditor::RootnoteEditor(RootnoteProcessor& p)
    : AudioProcessorEditor(p), processor(p), feed(p), controls(p)
{
    for (int i = 0; i < RootnoteProcessor::numPresets(); ++i)
        presets.addItem(RootnoteProcessor::presetName(i), i + 1);
    presets.onChange = [this] { processor.loadPreset(presets.getSelectedItemIndex()); };

    const auto step = [this](int by) {
        const int count = RootnoteProcessor::numPresets();
        processor.loadPreset((processor.currentPreset() + by + count) % count);
    };
    previous.onClick = [step] { step(-1); };
    next.onClick = [step] { step(1); };
    slotA.onClick = [this] { processor.selectAbSlot(0); };
    slotB.onClick = [this] { processor.selectAbSlot(1); };
    matchCut.onClick = [this] { processor.matchCutToHit(); };
    tuneToKey.onClick = [this] { processor.tuneBoostToKey(); };

    status.setJustificationType(juce::Justification::centredLeft);
    status.setFont(juce::FontOptions(14.0f));

    for (juce::Component* child : std::initializer_list<juce::Component*> {
             &presets, &previous, &next, &slotA, &slotB, &status, &matchCut, &tuneToKey, &feed, &controls })
        addAndMakeVisible(child);

    const auto stored = processor.storedEditorSize(); // read first: setting the limits already resizes
    setResizable(true, true);
    setResizeLimits(480, 420, 1400, 1600);
    setSize(stored.x > 0 ? stored.x : kDefaultWidth, stored.y > 0 ? stored.y : kDefaultHeight);

    processor.setDisplayActive(true);
    startTimerHz(30);
    refresh();
}

RootnoteEditor::~RootnoteEditor()
{
    processor.setDisplayActive(false);
}

void RootnoteEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
}

void RootnoteEditor::resized()
{
    auto area = getLocalBounds().reduced(8);

    auto top = area.removeFromTop(28);
    previous.setBounds(top.removeFromLeft(28));
    next.setBounds(top.removeFromRight(28));
    slotB.setBounds(top.removeFromRight(36).reduced(2, 0));
    slotA.setBounds(top.removeFromRight(36).reduced(2, 0));
    presets.setBounds(top.reduced(4, 0));

    area.removeFromTop(6);
    auto readout = area.removeFromTop(44);
    tuneToKey.setBounds(readout.removeFromRight(100).reduced(0, 8));
    readout.removeFromRight(6);
    matchCut.setBounds(readout.removeFromRight(100).reduced(0, 8));
    status.setBounds(readout);

    area.removeFromTop(6);
    feed.setBounds(area.removeFromTop(juce::jmax(100, area.getHeight() / 4)));
    area.removeFromTop(6);
    controls.setBounds(area);

    processor.storeEditorSize(getWidth(), getHeight());
}

void RootnoteEditor::refresh()
{
    const double a4 = static_cast<double>(processor.getApvts().getRawParameterValue(ids::a4)->load());
    const auto state = processor.displayState();
    const auto hit = processor.tunerReading();

    if (state.midiNoteOns != lastNoteOns)
        midiLit = 6;
    lastNoteOns = state.midiNoteOns;
    midiLit = juce::jmax(0, midiLit - 1);

    juce::String text;
    text << "Boost  " << describe(state.boostHz, a4) << (state.boostFromMidi ? "  (MIDI)" : "")
         << "      Cut  " << describe(state.cutHz, a4) << (state.cutFromMidi ? "  (MIDI)" : "") << "\n"
         << "Hit  " << (hit.valid ? describe(hit.hz, a4) : juce::String("-"))
         << "      Limiter  " << juce::String(toDb(state.limiterGain), 1) << " dB"
         << (midiLit > 0 ? "      MIDI in" : "");
    status.setText(text, juce::dontSendNotification);

    matchCut.setEnabled(hit.valid);
    tuneToKey.setEnabled(hit.valid);
    presets.setSelectedItemIndex(processor.currentPreset(), juce::dontSendNotification);
    slotA.setToggleState(processor.abSlot() == 0, juce::dontSendNotification);
    slotB.setToggleState(processor.abSlot() == 1, juce::dontSendNotification);

    feed.refresh(state);
}
