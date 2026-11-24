#pragma once

// The approved v3 grid, in 1440 x 784 logical px (0.5.1: the help line is gone and the columns' lower bands are 32 px
// taller; 0.5.2: one selector row, the bands under it 24 px higher). Column bands are column-relative y; master
// positions are absolute.
namespace snap::ui::layout
{
constexpr float margin = 24, colW = 217, colGap = 18;
constexpr float bandVisual = 32, bandSelector = 148, bandKnobs = 180, bandDetail = 268, bandLow = 376, bandLow2 = 408; // lower bands: name row, bar row under it
constexpr float slotX[] { 0, 76, 153 }, slot2X[] { 30, 123 }; // three dial slots; two spaced evenly
constexpr float cellH = 20, cellGapX = 5, cellGapY = 4;      // segmented selectors
constexpr float faderX = 48;                                 // the push limit meter's start
constexpr float masterYc = 724, bigKnob = 72, knob = 44, amountKnob = 88;
constexpr float columnTop = 64, columnH = 440, amountTop = 510, masterTop = 644;
constexpr int width = 1440, height = 784;
constexpr float colX(int slot) { return margin + (float) slot * (colW + colGap); }
// A centred fader line's margin: its end words sit at the column's edges, the fader between them with equal margins
// (8 px a character, the site's lattice, so both builds place it alike).
constexpr float lineMargin(int leftChars, int rightChars) { return 8.0f * (float) (leftChars > rightChars ? leftChars : rightChars) + 8; }
} // namespace snap::ui::layout
