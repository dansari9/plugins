#include "Style.h"
#include "BinaryData.h"

using namespace juce;

namespace snap::ui
{
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size));
}

// Geist Mono's ascent + descent is 1.3 em, so a box that tall, centred, puts the baseline where the mockup's text has it.
void text(Graphics& g, const String& s, float x, float y, Colour c, float size, bool bold, float width, Justification j)
{
    g.setColour(c), g.setFont(mono(size, bold));
    const float w = width > 0 ? width : textWidth(s, size) + size;
    g.drawText(s, Rectangle<float>(x, y, w, size * 1.3f), j.getOnlyHorizontalFlags() | Justification::verticallyCentred, false);
}

void arc(Graphics& g, float x, float y, float size, const Arc& a)
{
    using namespace colours;
    const bool big = size > 60;
    const float dr = big ? 2 : 1.4f, step = big ? 7.5f : 11.25f, r = size / 2 - dr - 0.5f, cx = x + size / 2, cy = y + r + dr + 0.5f;
    const Colour off = a.dim ? rule : colours::dim, on = a.dim ? colours::dim : accent;
    auto ring = [&](float radius, float value, bool centred, float st) {
        const float v = 270 * jlimit(0.0f, 1.0f, value);
        for (float t = 0; t <= 270.01f; t += st)
        {
            const float angle = degreesToRadians(225 + t);
            const bool lit = centred ? jmin(135.0f, v) <= t + 0.01f && t <= jmax(135.0f, v) + 0.01f : t <= v + 0.01f;
            dot(g, cx + radius * std::sin(angle), cy - radius * std::cos(angle), dr, lit ? on : off);
        }
    };
    ring(r, a.norm, a.centred, step);
    if (a.inner >= 0)
        ring(r * 0.62f, a.inner, false, 11.25f);
    const bool inner = a.inner >= 0 && a.needleInner;
    const float nv = inner ? a.inner : a.norm, angle = degreesToRadians(225 + 270 * jlimit(0.0f, 1.0f, nv));
    const float len = (inner ? r * 0.62f : r) - (big ? 6 : 4.5f);
    const int n = big ? 6 : 4;
    for (int k = 0; k <= n; ++k)
        dot(g, cx + len * std::sin(angle) * (float) k / (float) n, cy - len * std::cos(angle) * (float) k / (float) n, dr, a.dim ? muted : primary);
}

void fader(Graphics& g, float x, float y, float w, FaderKind kind, float a, float b, Colour on, bool dim)
{
    const int n = faderDots(w);
    const auto index = [n](float v) { return roundToInt(jlimit(0.0f, 1.0f, v) * (float) (n - 1)); };
    const int vi = index(a), ci = kind == FaderKind::centred ? roundToInt((float) (n - 1) / 2) : 0;
    const int lo = kind == FaderKind::range ? index(a) : jmin(ci, vi), hi = kind == FaderKind::range ? index(b) : jmax(ci, vi);
    const auto handleAt = [&](int i) { return kind != FaderKind::meter && (i == lo || i == hi) && (kind == FaderKind::range || i == vi); };
    const Colour lit = dimmed(on, dim), unlit = dimmed(colours::dim, dim), white = dimmed(colours::primary, dim);
    for (int i = 0; i < n; ++i)
    {
        const float px = x + 2 + 4.0f * (float) i;
        if (handleAt(i))
            for (int j = 0; j < 4; ++j)
                dot(g, px, y + 1.5f + 3.0f * (float) j, 1.1f, white);
        else
            dot(g, px, y + 6, 1.1f, (kind == FaderKind::meter ? i <= vi && a > 0 : i >= lo && i <= hi) ? lit : unlit);
    }
}

void vmeter(Graphics& g, float x, float y, int n, float norm, bool fromTop, Colour on, bool dim)
{
    const int lit = roundToInt(jlimit(0.0f, 1.0f, norm) * (float) n);
    for (int i = 0; i < n; ++i)
        dot(g, x + 3, y + 2 + 4.0f * (float) i, 1.1f, (fromTop ? i : n - 1 - i) < lit ? dimmed(on, dim) : dimmed(colours::dim, dim));
}

void square(Graphics& g, float x, float y, bool on, Colour onColour, bool dim)
{
    if (on)
        g.setColour(dimmed(onColour, dim)), g.fillRect(x, y, 8.0f, 8.0f);
    else
        g.setColour(dimmed(colours::muted, dim)), g.drawRect(x, y, 8.0f, 8.0f, 1.0f);
}

void toggle(Graphics& g, float x, float y, bool on, const String& label, bool dim)
{
    square(g, x, y + 4, on, colours::accent, dim);
    text(g, label, x + 14, y, dimmed(on ? colours::primary : colours::muted, dim));
}

void moduleGlyph(Graphics& g, Module m, float x, float y, Colour c)
{
    g.setColour(c);
    const float cx = x + 6, cy = y + 9;
    switch (m)
    {
        case Module::subHiss: g.fillRect(cx - 3, cy - 3, 6.0f, 6.0f); break;
        case Module::shift:
        {
            Path p;
            p.startNewSubPath(cx, cy - 6), p.lineTo(cx, cy + 6);
            p.startNewSubPath(cx - 3, cy - 3), p.lineTo(cx, cy - 6), p.lineTo(cx + 3, cy - 3);
            p.startNewSubPath(cx - 3, cy + 3), p.lineTo(cx, cy + 6), p.lineTo(cx + 3, cy + 3);
            g.strokePath(p, PathStrokeType(1.2f));
            break;
        }
        case Module::space:
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 7; ++j)
                    dot(g, cx - 3 + 3.0f * (float) i, cy - 7 + 2.4f * (float) j, 0.75f, c);
            break;
        case Module::press:
            for (int i = 0; i < 3; ++i)
                g.fillRect(cx - 4, cy - 3.5f + 3.0f * (float) i, 8.0f, 1.2f);
            break;
        case Module::drive:
        {
            Path p;
            p.startNewSubPath(cx - 4.5f, cy + 1);
            p.cubicTo(cx - 3, cy - 2.5f, cx - 1.5f, cy - 2.5f, cx, cy), p.cubicTo(cx + 1.5f, cy + 2.5f, cx + 3, cy + 2.5f, cx + 4.5f, cy - 1);
            g.strokePath(p, PathStrokeType(1.2f));
            break;
        }
        case Module::push:
        {
            Path p;
            p.addQuadrilateral(cx, cy - 4.5f, cx + 4.5f, cy, cx, cy + 4.5f, cx - 4.5f, cy);
            g.fillPath(p);
            break;
        }
    }
}

void padlock(Graphics& g, float x, float y, Colour c)
{
    Path shackle;
    shackle.startNewSubPath(x + 2.5f, y + 5.5f), shackle.lineTo(x + 2.5f, y + 3.8f);
    shackle.addCentredArc(x + 5, y + 3.8f, 2.5f, 2.5f, 0, -MathConstants<float>::halfPi, MathConstants<float>::halfPi);
    shackle.lineTo(x + 7.5f, y + 5.5f);
    g.setColour(c), g.strokePath(shackle, PathStrokeType(1.2f)), g.fillRoundedRectangle(x + 1, y + 5.5f, 8, 6, 1);
}

void handle(Graphics& g, float x, float y, Colour c)
{
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j)
            dot(g, x + 2 + 4.0f * (float) i, y + 2 + 4.0f * (float) j, 1.1f, c);
}

void arrow(Graphics& g, float x, float y, Colour c)
{
    Path p;
    p.startNewSubPath(x + 1, y + 8.5f), p.lineTo(x + 11, y + 8.5f);
    p.startNewSubPath(x + 7.5f, y + 5), p.lineTo(x + 11, y + 8.5f), p.lineTo(x + 7.5f, y + 12);
    g.setColour(c), g.strokePath(p, PathStrokeType(1.1f));
}

void swapArrows(Graphics& g, float x, float y, Colour c)
{
    Path p;
    p.startNewSubPath(x + 1, y + 6), p.lineTo(x + 11, y + 6), p.startNewSubPath(x + 8.5f, y + 3.5f), p.lineTo(x + 11, y + 6), p.lineTo(x + 8.5f, y + 8.5f);
    p.startNewSubPath(x + 11, y + 11), p.lineTo(x + 1, y + 11), p.startNewSubPath(x + 3.5f, y + 8.5f), p.lineTo(x + 1, y + 11), p.lineTo(x + 3.5f, y + 13.5f);
    g.setColour(c), g.strokePath(p, PathStrokeType(1.1f));
}

void menuIcon(Graphics& g, float x, float y, Colour c)
{
    g.setColour(c);
    for (int i = 0; i < 3; ++i)
        g.fillRect(x, y + 0.4f + 4.0f * (float) i, 14.0f, 1.2f);
}

void dice(Graphics& g, float x, float y, Colour c)
{
    g.setColour(c), g.drawRoundedRectangle(x + 0.6f, y + 0.6f, 12.8f, 12.8f, 2, 1.2f);
    for (int i = 0; i < 3; ++i)
        dot(g, x + 4 + 3.0f * (float) i, y + 4 + 3.0f * (float) i, 1.2f, c);
}

void heart(Graphics& g, float x, float y, bool filled)
{
    Path p;
    p.startNewSubPath(x + 6, y + 10.6f);
    p.lineTo(x + 1.7f, y + 6.3f);
    p.cubicTo(x + 0.2f, y + 4.6f, x + 1.2f, y + 1.8f, x + 3.4f, y + 1.6f), p.cubicTo(x + 4.6f, y + 1.5f, x + 5.5f, y + 2.3f, x + 6, y + 3.4f);
    p.cubicTo(x + 6.5f, y + 2.3f, x + 7.4f, y + 1.5f, x + 8.6f, y + 1.6f), p.cubicTo(x + 10.8f, y + 1.8f, x + 11.8f, y + 4.6f, x + 10.3f, y + 6.3f);
    p.closeSubPath();
    if (filled)
        g.setColour(colours::accent), g.fillPath(p);
    else
        g.setColour(colours::muted), g.strokePath(p, PathStrokeType(1.0f));
}

void slope(Graphics& g, Slope s, float x, float y, Colour c)
{
    Path p;
    switch (s)
    {
        case Slope::lowGentle: p.startNewSubPath(x + 1, y + 9), p.cubicTo(x + 4, y + 2, x + 7, y + 1, x + 13, y + 1); break;
        case Slope::lowSteep:
            p.startNewSubPath(x + 3, y + 9), p.lineTo(x + 5, y + 3), p.cubicTo(x + 6, y, x + 8, y, x + 9, y + 1), p.lineTo(x + 13, y + 1);
            break;
        case Slope::highGentle: p.startNewSubPath(x + 1, y + 1), p.cubicTo(x + 7, y + 1, x + 10, y + 2, x + 13, y + 9); break;
        case Slope::highSteep:
            p.startNewSubPath(x + 1, y + 1), p.lineTo(x + 5, y + 1), p.cubicTo(x + 6, y, x + 8, y, x + 9, y + 3), p.lineTo(x + 11, y + 9);
            break;
    }
    g.setColour(c), g.strokePath(p, PathStrokeType(1.2f));
}

void gridDots(Graphics& g, float w, float h)
{
    for (float x = 4; x < w; x += 8)
        for (float y = 4; y < h; y += 8)
            dot(g, x, y, 0.6f, colours::rule);
}
} // namespace snap::ui
