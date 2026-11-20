#include "Braille.h"
#include <juce_audio_processors/juce_audio_processors.h>

using namespace juce;

namespace snap::ui
{
Point<float> Braille::origin;

Lattice::Lattice(const Component& c) : saved(Braille::origin)
{
    auto* layer = &c; // the editor's one child, which holds everything in 1440 x 840 logical px
    while (auto* p = layer->getParentComponent())
    {
        if (dynamic_cast<const AudioProcessorEditor*>(p) != nullptr)
            break;
        layer = p;
    }
    Braille::origin = layer->getLocalPoint(&c, Point<float>());
}

Lattice::~Lattice() { Braille::origin = saved; }

Braille::~Braille()
{
    for (int i = 0; i < capacity; ++i)
        if (taken[(size_t) i])
        {
            const auto& c = cells[i];
            g.setColour(Colour(c.argb));
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                    if (c.bits & brailleBit(dx, dy))
                    {
                        const float x = (float) (c.col * charW) + (dx == 0 ? 0.3f : 0.7f) * charW - origin.x;
                        const float y = (float) (c.row * charH) + (0.125f + 0.25f * (float) dy) * charH - origin.y;
                        g.fillEllipse(x - dotRadius, y - dotRadius, 2 * dotRadius, 2 * dotRadius);
                    }
        }
}

void Braille::cell(int col, int row, uint8 bits, Colour colour, Mode mode)
{
    if (bits == 0)
        return;
    int i = (int) (((unsigned) col * 73856093u ^ (unsigned) row * 19349663u) % (unsigned) capacity);
    for (int probe = 0; taken[(size_t) i] && (cells[i].col != col || cells[i].row != row); ++probe, i = (i + 1) % capacity)
        if (probe == capacity)
        {
            jassertfalse; // more cells than one drawing should ever need
            return;
        }
    auto& c = cells[i];
    if (!taken[(size_t) i])
        taken[(size_t) i] = true, c = { col, row, bits, colour.getARGB(), current };
    else if (c.stroke == current || mode == Mode::merge)
        c = { col, row, (uint8) (bits | c.bits), colour.getARGB(), current };
    else if (mode == Mode::over)
        c = { col, row, bits, colour.getARGB(), current };
}

void Braille::dot(float x, float y, Colour colour, Mode mode)
{
    const int dx = (int) std::floor((x + origin.x) / 4), dy = (int) std::floor((y + origin.y) / 4);
    const int col = (int) std::floor((float) dx / 2), row = (int) std::floor((float) dy / 4);
    cell(col, row, brailleBit(dx - 2 * col, dy - 4 * row), colour, mode);
}

void Braille::line(float x0, float y0, float x1, float y1, Colour colour, float step)
{
    const int n = jmax(1, (int) std::ceil(std::hypot(x1 - x0, y1 - y0) / step));
    for (int k = 0; k <= n; ++k)
        dot(x0 + (x1 - x0) * (float) k / (float) n, y0 + (y1 - y0) * (float) k / (float) n, colour);
}

void Braille::box(Rectangle<float> r, Colour colour)
{
    const float x0 = r.getX() + 0.5f, y0 = r.getY() + 0.5f, x1 = r.getRight() - 0.5f, y1 = r.getBottom() - 0.5f;
    line(x0, y0, x1, y0, colour, 3.2f), line(x1, y0, x1, y1, colour, 3.2f), line(x1, y1, x0, y1, colour, 3.2f), line(x0, y1, x0, y0, colour, 3.2f);
}

// Calls f(x, y) at every lattice dot whose position lies in r (local coordinates, 0.5 px slack).
template <typename F>
static void eachDot(Rectangle<float> r, F&& f)
{
    const auto o = Braille::origin;
    for (int dy = (int) std::floor((r.getY() + o.y) / 4); dy <= (int) std::floor((r.getBottom() + o.y) / 4); ++dy)
        for (int dx = (int) std::floor((r.getX() + o.x) / 4) - 1; dx <= (int) std::floor((r.getRight() + o.x) / 4) + 1; ++dx)
        {
            const float x = (float) dx * 4 + (dx % 2 == 0 ? 2.4f : 1.6f) - o.x, y = (float) dy * 4 + 2 - o.y;
            if (r.expanded(0.5f).contains(x, y))
                f(x, y);
        }
}

void Braille::fill(Rectangle<float> r, Colour colour, bool ellipse)
{
    const auto c = r.getCentre();
    const float rx = r.getWidth() / 2 + 0.6f, ry = r.getHeight() / 2 + 0.6f;
    bool any = false;
    eachDot(r, [&](float x, float y) {
        if (!ellipse || square((x - c.x) / rx) + square((y - c.y) / ry) <= 1)
            dot(x, y, colour), any = true;
    });
    if (!any)
        dot(c.x, c.y, colour);
}

void Braille::path(const Path& p, Colour colour, bool filled)
{
    if (const auto bounds = p.getBounds(); filled && jmax(bounds.getWidth(), bounds.getHeight()) <= 40)
        eachDot(bounds, [&](float x, float y) {
            if (p.contains(x, y))
                dot(x, y, colour);
        });
    // Each subpath: one dot when it is tiny, else dots along it.
    for (int sub = 0;; ++sub)
    {
        Rectangle<float> b;
        bool found = false;
        for (PathFlatteningIterator it(p); it.next();)
            if (it.subPathIndex == sub)
                b = found ? b.getUnion({ it.x1, it.y1, 0, 0 }).getUnion({ it.x2, it.y2, 0, 0 }) : Rectangle<float>::leftTopRightBottom(jmin(it.x1, it.x2), jmin(it.y1, it.y2), jmax(it.x1, it.x2), jmax(it.y1, it.y2)), found = true;
        if (!found)
            break;
        if (jmax(b.getWidth(), b.getHeight()) <= 6.5f)
            dot(b.getCentreX(), b.getCentreY(), colour);
        else
            for (PathFlatteningIterator it(p); it.next();)
                if (it.subPathIndex == sub)
                    line(it.x1, it.y1, it.x2, it.y2, colour, filled ? 3.0f : 2.5f);
    }
}
} // namespace snap::ui
