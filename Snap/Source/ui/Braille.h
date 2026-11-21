#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <bitset>

// The ascii look: every graphic is braille on one 8 x 16 px character grid laid over the whole 1440 x 840 layer. A cell
// holds 2 x 4 dots (x at .3 / .7 of its width, y at .125 / .375 / .625 / .875 of its height) in one colour. Dots are
// vectors, so no braille glyph is ever drawn from a font.
namespace snap::ui
{
constexpr int charW = 8, charH = 16;
constexpr float dotRadius = 1.2f;
constexpr juce::uint8 brailleBit(int dx, int dy) { return (juce::uint8) (dy == 3 ? 0x40 << dx : 1 << (dy + 3 * dx)); } // U+2800 bit order
// Onto a cell an earlier stroke drew: or the dots in, keep the old cell, replace it. Within one stroke dots always or in.
enum class Mode { merge, under, over };

// Where a component sits in the 1440 x 840 layer (the editor's one child).
juce::Point<float> layerOffset(const juce::Component&);

// While a component paints, the lattice is pinned to the layer's origin, so dots line up across components.
struct Lattice
{
    explicit Lattice(const juce::Component&);
    ~Lattice();

private:
    juce::Point<float> saved;
};

// The dots of one drawing, in the painting component's coordinates; they are drawn when it goes out of scope. A point
// lands on the dot whose 4 x 4 px square holds it. No allocation: up to 1024 cells.
class Braille
{
public:
    explicit Braille(juce::Graphics& graphics) : g(graphics) {}
    ~Braille();
    void cell(int col, int row, juce::uint8 bits, juce::Colour, Mode = Mode::merge); // the cell writer, layer cell coordinates
    void dot(float x, float y, juce::Colour, Mode = Mode::merge);
    void line(float x0, float y0, float x1, float y1, juce::Colour, float step = 2.5f);
    void box(juce::Rectangle<float>, juce::Colour);                  // a dotted outline
    void fill(juce::Rectangle<float>, juce::Colour, bool ellipse = false); // every dot inside (at least the centre one)
    void path(const juce::Path&, juce::Colour, bool filled = false); // tiny subpaths become one dot
    void stroke() { ++current; } // the dots after this are one new stroke (gulp's braille() call)

    static juce::Point<float> origin; // the painting component's offset in the layer (Lattice)
    static constexpr int capacity = 1024;

private:
    struct Cell
    {
        int col, row;
        juce::uint8 bits;
        juce::uint32 argb;
        int stroke;
    };
    juce::Graphics& g;
    Cell cells[capacity]; // only the taken ones are read
    std::bitset<capacity> taken;
    int current = 0;
};
} // namespace snap::ui
