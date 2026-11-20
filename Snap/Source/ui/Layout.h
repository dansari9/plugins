#pragma once

// The approved v3 grid, in 1440 x 840 logical px. Column bands are column-relative y; master positions are absolute.
namespace snap::ui::layout
{
constexpr float margin = 24, colW = 217, colGap = 18;
constexpr float bandVisual = 32, bandSelector = 148, bandKnobs = 204, bandDetail = 292, bandTarget = 380, bandFocus = 398, bandReadout = 412;
constexpr float slotX[] { 0, 76, 153 }, slot2X[] { 30, 123 }; // three dial slots; two spaced evenly
constexpr float cellH = 20, cellGapX = 5, cellGapY = 4;      // segmented selectors
constexpr float faderX = 48, faderEndX = 86, faderRightX = 185; // fader lines: start, start after a left end label, end before a right one
constexpr float masterYc = 716, masterGutter = 28, bigKnob = 72, knob = 44, amountKnob = 88;
constexpr float columnTop = 64, columnH = 432, amountTop = 502, masterTop = 636, bottomTop = 776;
constexpr float colX(int slot) { return margin + (float) slot * (colW + colGap); }
} // namespace snap::ui::layout
