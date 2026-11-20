#pragma once
#include "../dsp/modules/Modules.h"
#include <juce_gui_basics/juce_gui_basics.h>

// The v1 look: monochrome plus one red, Geist Mono, dotted controls. Every coordinate in the UI is in the 1440 x 840
// logical layout of the approved mockup; the editor scales the whole layer. Symbols Geist Mono lacks are vector paths.
namespace snap::ui
{
namespace colours
{
inline const juce::Colour bg { 0xff0d0d0d }, primary { 0xffececec }, muted { 0xff7a7a7a }, secondary { 0xffbcbcbc }, rule { 0xff2a2a2a },
    dim { 0xff454545 }, accent { 0xffff3d1f }, tag { 0xff1c1c1c };
} // namespace colours

// A switched-off module draws one step darker.
inline juce::Colour dimmed(juce::Colour c, bool dim)
{
    using namespace colours;
    if (!dim)
        return c;
    if (c == primary)
        return muted;
    if (c == accent || c == secondary || c == muted)
        return colours::dim;
    return c == colours::dim ? rule : c;
}

juce::Font mono(float size, bool bold = false); // Geist Mono Regular / SemiBold, size in px (em)
inline juce::String u(const char* utf8) { return juce::String::fromUTF8(utf8); }
inline float textWidth(const juce::String& s, float size = 12) { return (float) s.length() * size * 0.6f; } // monospaced advance

// Text with its box's top-left at (x, y), as the mockup places it; width 0 = as wide as the text, left-aligned.
void text(juce::Graphics&, const juce::String&, float x, float y, juce::Colour, float size = 12, bool bold = false, float width = 0,
          juce::Justification = juce::Justification::left);
inline void centred(juce::Graphics& g, const juce::String& s, float x, float y, float w, juce::Colour c, float size = 12, bool bold = false)
{
    text(g, s, x, y, c, size, bold, w, juce::Justification::centred);
}
inline void right(juce::Graphics& g, const juce::String& s, float x, float y, float w, juce::Colour c, float size = 12, bool bold = false)
{
    text(g, s, x, y, c, size, bold, w, juce::Justification::right);
}

inline void dot(juce::Graphics& g, float x, float y, float r, juce::Colour c) { g.setColour(c), g.fillEllipse(x - r, y - r, 2 * r, 2 * r); }
inline void hline(juce::Graphics& g, float x, float y, float w, juce::Colour c = colours::rule) { g.setColour(c), g.fillRect(x, y, w, 1.0f); }
inline void vline(juce::Graphics& g, float x, float y, float h, juce::Colour c = colours::rule) { g.setColour(c), g.fillRect(x, y, 1.0f, h); }

// Dotted 270 degree dial drawn in a size x size box at (x, y): off dots, the lit arc (from the start, or from the centre
// when centred) and a dotted needle. inner >= 0 adds a second ring at 62 % for a two-value dial; the needle follows the
// ring named by needleInner.
struct Arc
{
    float norm = 0;
    bool centred = false, dim = false;
    float inner = -1;
    bool needleInner = false;
};
void arc(juce::Graphics&, float x, float y, float size, const Arc&);
inline float arcHeight(float size) // the box the mockup gives a dial of this size
{
    const bool big = size > 60;
    const float dr = big ? 2 : 1.4f, r = size / 2 - dr - 0.5f;
    return std::ceil(r + dr + 0.5f + r * 0.7072f + dr + 1);
}

// A row of dots every 4 px, 12 px tall, at (x, y), w wide.
enum class FaderKind { value, centred, meter, range };
void fader(juce::Graphics&, float x, float y, float w, FaderKind, float a, float b = 0, juce::Colour on = colours::accent, bool dim = false);
inline int faderDots(float w) { return (int) std::floor((w - 2) / 4) + 1; }

// A vertical meter, n dots 4 px apart, 6 px wide, lit from the top (downward) or the bottom.
void vmeter(juce::Graphics&, float x, float y, int n, float norm, bool fromTop, juce::Colour on, bool dim = false);
// A 9-dot vertical meter around a white centre dot: v > 0 lights upward, v < 0 downward, |v| = 1 lights 4 dots.
void vbipolar(juce::Graphics&, float x, float y, float v, juce::Colour on, bool dim = false);

// ■ / □ 8 px square at (x, y).
void square(juce::Graphics&, float x, float y, bool on, juce::Colour onColour = colours::accent, bool dim = false);
// A toggle: the square at (x, y + 4) and its label at x + 14.
void toggle(juce::Graphics&, float x, float y, bool on, const juce::String& label, bool dim = false);

// Vector symbols.
void moduleGlyph(juce::Graphics&, Module, float x, float y, juce::Colour); // ▪ ↕ ░ ≡ ~ ◆ in a 12 x 16 box at (x, y)
void padlock(juce::Graphics&, float x, float y, juce::Colour);            // 10 x 12
void handle(juce::Graphics&, float x, float y, juce::Colour = colours::muted); // ⋮⋮ 8 x 12
void arrow(juce::Graphics&, float x, float y, juce::Colour = colours::muted);  // → in a 12 x 16 text box
void swapArrows(juce::Graphics&, float x, float y, juce::Colour);             // ⇄ in a 12 x 16 text box
void menuIcon(juce::Graphics&, float x, float y, juce::Colour);               // ≡ 14 x 10
void dice(juce::Graphics&, float x, float y, juce::Colour);                   // 14 x 14
void heart(juce::Graphics&, float x, float y, bool filled);                   // 12 x 12
enum class Slope { lowGentle, lowSteep, highGentle, highSteep };
void slope(juce::Graphics&, Slope, float x, float y, juce::Colour);           // 14 x 10

// The mockup's background dot grid for a visualizer, every 8 px.
void gridDots(juce::Graphics&, float w, float h);
} // namespace snap::ui
