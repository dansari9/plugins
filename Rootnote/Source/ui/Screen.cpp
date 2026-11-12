#include "Screen.h"

#include "Parameters.h"
#include "ui/Theme.h"

namespace rootnote::ui
{
namespace
{
// The mockup's axis: nine octaves from a quarter tone below C0.
const double kLowHz = 16.3516 * std::exp2(-1.0 / 24.0);
const double kHighHz = kLowHz * 512.0;

constexpr float kGraphX = 16.0f, kGraphY = 70.0f, kGraphW = 528.0f, kGraphH = 120.0f;
constexpr float kLegendY = 205.0f, kLegendDx = (kGraphW - 62.0f) / 7.0f;

float xOf(double hz)
{
    return kGraphW * static_cast<float>(std::log2(hz / kLowHz) / std::log2(kHighHz / kLowHz));
}

double value(juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    return static_cast<double>(apvts.getRawParameterValue(id)->load());
}
} // namespace

Screen::Screen(RootnoteProcessor& p)
    : processor(p)
{
    std::fill(std::begin(spectrumDb), std::end(spectrumDb), kBottomDb);
    for (size_t i = 0; i < kFftSize; ++i)
        window[i] = 0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * static_cast<double>(i) / kFftSize);
    buildPaths();
    setOpaque(false);
    setInterceptsMouseClicks(true, false); // only the legend values: see hitTest()
}

void Screen::setPage(int newPage)
{
    if (page != newPage)
    {
        page = newPage;
        repaint();
    }
}

int Screen::legendSlotAt(int x) const
{
    const int slot = static_cast<int>(std::floor((static_cast<float>(x) - 16.0f + 4.0f) / kLegendDx));
    return juce::isPositiveAndBelow(slot, kNumEncoders) && pageSlots(page)[static_cast<size_t>(slot)].id != nullptr ? slot : -1;
}

bool Screen::hitTest(int x, int y)
{
    return static_cast<float>(y) > kLegendY && legendSlotAt(x) >= 0;
}

void Screen::mouseUp(const juce::MouseEvent& e)
{
    const int slot = legendSlotAt(e.x);
    if (slot >= 0 && onValueClick != nullptr && getLocalBounds().contains(e.getPosition()))
        onValueClick(slot);
}

bool Screen::sameFrame(const RootnoteProcessor::DisplayState& other) const
{
    const auto sameBands = [](const BandView* a, const BandView* b, int count) {
        for (int i = 0; i < count; ++i)
            if (a[i].hz != b[i].hz || a[i].gainDb != b[i].gainDb)
                return false;
        return true;
    };
    return state.boostHz == other.boostHz && state.cutHz == other.cutHz && state.boostFromMidi == other.boostFromMidi
        && state.numBoostBands == other.numBoostBands && state.numCutBands == other.numCutBands
        && sameBands(state.boostBands, other.boostBands, state.numBoostBands)
        && sameBands(state.cutBands, other.cutBands, state.numCutBands);
}

void Screen::update(const RootnoteProcessor::DisplayState& newState, const TunerReading& reading)
{
    float block[512];
    bool heard = false;
    for (size_t count = 0; (count = processor.readSpectrumSamples(block, std::size(block))) > 0; heard = true)
        for (size_t i = 0; i < count; ++i)
            recent[writeAt++ & (kFftSize - 1)] = block[i];

    // Without new audio (the host stopped) the picture falls away instead of freezing.
    bool changed = false;
    if (heard)
    {
        analyse(newState.sampleRate);
        changed = true;
    }
    else
    {
        for (float& db : spectrumDb)
        {
            changed = changed || db > kBottomDb;
            db = juce::jmax(kBottomDb, db - 3.0f);
        }
    }
    if (changed)
        buildPaths();

    // The text and the legend only change with the frame, the reading or a parameter.
    const auto& all = processor.getParameters();
    bool same = sameFrame(newState) && reading.valid == shownReading.valid && reading.hz == shownReading.hz
             && shownParameters.size() == static_cast<size_t>(all.size());
    shownParameters.resize(static_cast<size_t>(all.size()));
    for (int i = 0; i < all.size(); ++i)
    {
        const float v = all[i]->getValue();
        same = same && shownParameters[static_cast<size_t>(i)] == v;
        shownParameters[static_cast<size_t>(i)] = v;
    }
    state = newState;
    if (!same)
    {
        shownReading = reading;
        buildText(reading);
    }
    if (changed || !same)
        repaint();
}

void Screen::buildText(const TunerReading& reading)
{
    auto& apvts = processor.getApvts();
    const double a4 = value(apvts, ids::a4);
    const double boostNote = hzToNote(state.boostHz, a4);
    note = noteText(juce::roundToInt(boostNote));
    noteDetail = number(state.boostHz, 1, 1) + " hz " + juce::String::fromUTF8("\xc2\xb7") + " "
                 + signedNumber(value(apvts, ids::boostGain), 1, 1) + " db" + (state.boostFromMidi ? "  midi" : "");

    const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");
    hitValid = reading.valid;
    hitLine = reading.valid ? "hit " + noteWithCents(hzToNote(reading.hz, a4)) + dot + number(reading.hz, 1, 1) + " hz"
                            : juce::String("no hit");

    static const char* const series[] = { "harm", "oct", "drum" };
    const int key = juce::roundToInt(value(apvts, ids::key));
    seriesLine = juce::String("series ") + series[juce::jlimit(0, 2, juce::roundToInt(value(apvts, ids::series)))] + dot
                 + "key " + noteText(key + 12, true).dropLastCharacters(1) + " "
                 + scaleShortName(juce::roundToInt(value(apvts, ids::scale)))
                 + (value(apvts, ids::keyLock) >= 0.5 ? " lock" : "");

    if (value(apvts, ids::cutOn) >= 0.5)
        cutLine = "cut " + noteText(juce::roundToInt(hzToNote(state.cutHz, a4)), true) + " "
                  + number(value(apvts, ids::cutDepth), 1) + "db q" + number(value(apvts, ids::cutQ), 1) + " ovt"
                  + number(value(apvts, ids::cutOvertones), 0) + " " + juce::String(juce::roundToInt(value(apvts, ids::cutBands)))
                  + "b";
    else
        cutLine = "cut off";

    const double ring = value(apvts, ids::ring);
    ringLine = ring > 0.0 ? "ring " + number(ring, 0) + "% " + number(value(apvts, ids::ringDecay), 0) + "ms drop"
                                + number(value(apvts, ids::ringDrop), 1)
                          : juce::String("ring off");
    repaint();
}

void Screen::analyse(double sampleRate)
{
    for (size_t i = 0; i < kFftSize; ++i)
    {
        re[i] = window[i] * static_cast<double>(recent[(writeAt + i) & (kFftSize - 1)]);
        im[i] = 0.0;
    }
    fft.forward(re.data(), im.data());

    // Below one bin per point (the low end) the power is interpolated between
    // bins, so the curve stays smooth; above, each point takes its loudest bin.
    // A full-scale sine reads 0 dB.
    const auto power = [this](size_t bin) { return re[bin] * re[bin] + im[bin] * im[bin]; };
    const double binHz = sampleRate / kFftSize;
    const double step = std::pow(kHighHz / kLowHz, 1.0 / (kPoints - 1));
    double hz = kLowHz;
    for (float& db : spectrumDb)
    {
        const double centre = juce::jlimit(1.0, kFftSize / 2.0 - 2.0, hz / binHz);
        double p = 0.0;
        if (centre * (step - 1.0) < 1.0)
        {
            const auto below = static_cast<size_t>(centre);
            const double t = centre - static_cast<double>(below);
            p = (1.0 - t) * power(below) + t * power(below + 1);
        }
        else
        {
            const auto first = static_cast<size_t>(centre / std::sqrt(step));
            const auto last = juce::jmin(static_cast<size_t>(kFftSize / 2 - 1), static_cast<size_t>(centre * std::sqrt(step)));
            for (size_t bin = first; bin <= last; ++bin)
                p = juce::jmax(p, power(bin));
        }
        const float now = juce::Decibels::gainToDecibels(static_cast<float>(std::sqrt(p) * 4.0 / kFftSize), -100.0f);
        db = juce::jmax(now, db - 1.5f);
        hz *= step;
    }
}

void Screen::buildPaths()
{
    line.clear();
    for (int i = 0; i < kPoints; ++i)
    {
        const float x = kGraphX + kGraphW * static_cast<float>(i) / (kPoints - 1);
        const float y = kGraphY + kGraphH * juce::jlimit(0.0f, 1.0f, (kTopDb - spectrumDb[i]) / (kTopDb - kBottomDb));
        i == 0 ? line.startNewSubPath(x, y) : line.lineTo(x, y);
    }
    area = line;
    area.lineTo(kGraphX + kGraphW, kGraphY + kGraphH);
    area.lineTo(kGraphX, kGraphY + kGraphH);
    area.closeSubPath();
}

void Screen::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::screen);
    g.fillRoundedRectangle(bounds, 10.0f);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 9.5f, 1.0f);

    // Graph: grid, spectrum, pillars, axis.
    for (int o = 1; o < 9; ++o)
    {
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.fillRect(kGraphX + kGraphW * static_cast<float>(o) / 9.0f, kGraphY, 1.0f, kGraphH);
    }

    g.setColour(juce::Colours::white.withAlpha(0.04f));
    g.fillPath(area);
    g.setColour(juce::Colours::white.withAlpha(0.28f));
    g.strokePath(line, juce::PathStrokeType(1.0f));

    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(juce::Rectangle<float>(kGraphX, kGraphY - 12.0f, kGraphW, kGraphH + 12.0f).toNearestInt());
        g.setColour(colours::orange);
        for (int i = 0; i < state.numCutBands; ++i)
        {
            const float depth = juce::jmin(kGraphH, static_cast<float>(-state.cutBands[i].gainDb) * 5.0f);
            g.fillRect(kGraphX + xOf(state.cutBands[i].hz) - 1.0f, kGraphY, 2.0f, depth);
        }
        g.setColour(colours::white);
        for (int i = 0; i < state.numBoostBands; ++i)
        {
            const float height = juce::jmin(kGraphH - 12.0f, static_cast<float>(state.boostBands[i].gainDb) * 8.0f);
            const float x = kGraphX + xOf(state.boostBands[i].hz);
            g.fillRect(x - 1.5f, kGraphY + kGraphH - height, 3.0f, height);
            if (i == 0) // the root's cap
                g.fillRect(x - 5.0f, kGraphY + kGraphH - height - 10.0f, 10.0f, 5.0f);
        }
    }

    g.setFont(mono(8.0f, Weight::regular, 0.0f));
    g.setColour(colours::axis);
    for (const auto& [hz, label] : { std::pair { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                     { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" } })
        g.drawSingleLineText(label, juce::roundToInt(kGraphX + xOf(hz) + 3.0f), juce::roundToInt(kGraphY + kGraphH + 12.0f));

    // Note and status.
    const auto big = mono(40.0f, Weight::medium);
    g.setFont(big);
    g.setColour(colours::white);
    g.drawSingleLineText(note, 16, 48);
    const float noteWidth = textWidth(big, note);
    g.setFont(mono(12.0f));
    g.setColour(colours::screenText);
    g.drawSingleLineText(noteDetail, juce::roundToInt(16.0f + noteWidth + 10.0f), 48);

    const auto small = mono(10.0f);
    g.setFont(small);
    const int right = getWidth() - 16;
    int baseline = 24;
    for (const auto* text : { &hitLine, &seriesLine, &cutLine, &ringLine })
    {
        g.setColour(colours::screenText);
        g.drawSingleLineText(*text, right, baseline, juce::Justification::right);
        if (text == &hitLine)
        {
            const float x = static_cast<float>(right) - textWidth(small, hitLine) - 9.0f;
            g.setColour(hitValid ? colours::orange : colours::outOfKey);
            g.fillEllipse(juce::Rectangle<float>(6.5f, 6.5f).withCentre({ x, static_cast<float>(baseline) - 3.5f }));
        }
        baseline += 16;
    }

    // Legend: the eight encoder values, coloured like the caps.
    g.setColour(colours::rule);
    g.fillRect(16.0f, kLegendY, kGraphW, 1.0f);
    const auto& slots = pageSlots(page);
    const auto labelType = mono(8.0f, Weight::regular, 0.02f);
    const auto valueType = mono(11.0f);
    for (int i = 0; i < kNumEncoders; ++i)
    {
        const Slot& slot = slots[static_cast<size_t>(i)];
        if (slot.id == nullptr)
            continue;
        const float x = 16.0f + static_cast<float>(i) * kLegendDx;
        g.setColour(capColour(slot.cap));
        g.fillEllipse(x, 215.5f, 5.0f, 5.0f);
        g.setFont(labelType);
        g.setColour(colours::legendLabel);
        g.drawSingleLineText(juce::String(slot.label).toUpperCase(), juce::roundToInt(x + 9.0f), 221);
        g.setFont(valueType);
        g.setColour(colours::white);
        g.drawSingleLineText(legendValue(slot, processor.getApvts()), juce::roundToInt(x), 236);
    }
}
} // namespace rootnote::ui
