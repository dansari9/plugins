#include "Style.h"
#include "Braille.h"
#include "Riso.h"
#include "BinaryData.h"

using namespace juce;

namespace snap::ui
{
Font mono(float size, bool bold)
{
    static const Typeface::Ptr faces[] { Typeface::createSystemTypefaceFor(BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize),
                                         Typeface::createSystemTypefaceFor(BinaryData::GeistMonoSemiBold_ttf, BinaryData::GeistMonoSemiBold_ttfSize) };
    // Every glyph the UI draws is in the bundled faces, so system font fallback (fontconfig on Linux) is never needed.
    return Font(FontOptions(faces[bold ? 1 : 0]).withPointHeight(size).withFallbackEnabled(false));
}

// Geist Mono's ascent + descent is 1.3 em, so a box that tall, centred, puts the baseline where the mockup's text has it.
void text(Graphics& g, const String& s, float x, float y, Colour c, float size, bool bold, float width, Justification j)
{
    riso::use(g, c), g.setFont(mono(size, bold));
    const float w = width > 0 ? width : textWidth(s, size) + size;
    g.drawText(s, Rectangle<float>(x, y, w, size * 1.3f), j.getOnlyHorizontalFlags() | Justification::verticallyCentred, false);
}

void arc(Graphics& g, float x, float y, float size, const Arc& a)
{
    using namespace colours;
    const bool big = size > 60;
    const float dr = big ? 2 : 1.4f, step = big ? 7.5f : 11.25f, r = size / 2 - dr - 0.5f, cx = x + size / 2, cy = y + r + dr + 0.5f;
    const Colour off = a.dim ? rule : colours::dim, on = a.dim ? colours::dim : accent;
    Braille b(g);
    auto ring = [&](float radius, float value, bool centred, float st) {
        const float v = 270 * jlimit(0.0f, 1.0f, value);
        for (bool litPass : { false, true }) // the track, then the lit arc over it
            for (float t = 0; t <= 270.01f; t += st)
            {
                const float angle = degreesToRadians(225 + t);
                const bool lit = centred ? jmin(135.0f, v) <= t + 0.01f && t <= jmax(135.0f, v) + 0.01f : t <= v + 0.01f;
                if (lit == litPass)
                    b.dot(cx + radius * std::sin(angle), cy - radius * std::cos(angle), lit ? on : off);
            }
    };
    ring(r, a.norm, a.centred, step);
    if (a.inner >= 0)
        ring(r * 0.62f, a.inner, false, 11.25f);
    const bool inner = a.inner >= 0 && a.needleInner;
    const float nv = inner ? a.inner : a.norm, angle = degreesToRadians(225 + 270 * jlimit(0.0f, 1.0f, nv));
    const float len = (inner ? r * 0.62f : r) - (big ? 6 : 4.5f);
    const int n = big ? 6 : 4;
    b.stroke();
    for (int k = 0; k <= n; ++k)
        b.dot(cx + len * std::sin(angle) * (float) k / (float) n, cy - len * std::cos(angle) * (float) k / (float) n, a.dim ? muted : primary, Mode::over);
}

void fader(Graphics& g, float x, float y, float w, FaderKind kind, float a, float b, Colour on, bool dim)
{
    const int n = faderDots(w);
    const auto index = [n](float v) { return roundToInt(jlimit(0.0f, 1.0f, v) * (float) (n - 1)); };
    const int vi = index(a), ci = kind == FaderKind::centred ? roundToInt((float) (n - 1) / 2) : 0;
    const int lo = kind == FaderKind::range ? index(a) : jmin(ci, vi), hi = kind == FaderKind::range ? index(b) : jmax(ci, vi);
    const auto handleAt = [&](int i) { return kind != FaderKind::meter && (i == lo || i == hi) && (kind == FaderKind::range || i == vi); };
    const auto lit = [&](int i) { return kind == FaderKind::meter ? i <= vi && a > 0 : i >= lo && i <= hi; };
    Braille dots(g);
    for (int pass = 0; pass < 3; ++pass, dots.stroke()) // unlit, lit, handles
        for (int i = 0; i < n; ++i)
        {
            const float px = x + 2 + 4.0f * (float) i;
            if (handleAt(i) && pass == 2)
                for (int j = 0; j < 4; ++j)
                    dots.dot(px, y + 1.5f + 3.0f * (float) j, dimmed(colours::primary, dim), Mode::over);
            else if (!handleAt(i) && pass == (lit(i) ? 1 : 0))
                dots.dot(px, y + 6, dimmed(pass == 1 ? on : colours::dim, dim));
        }
}

void vmeter(Graphics& g, float x, float y, int n, float norm, bool fromTop, Colour on, bool dim)
{
    const int lit = roundToInt(jlimit(0.0f, 1.0f, norm) * (float) n);
    Braille b(g);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i)
            if (((fromTop ? i : n - 1 - i) < lit) == (pass == 1))
                b.dot(x + 3, y + 2 + 4.0f * (float) i, dimmed(pass == 1 ? on : colours::dim, dim));
}

void vbipolar(Graphics& g, float x, float y, float v, Colour on, bool dim)
{
    const int k = roundToInt(jlimit(0.0f, 1.0f, std::abs(v)) * 4);
    Braille b(g);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < 9; ++i)
        {
            const bool lit = i == 4 || (v > 0 ? i < 4 && i >= 4 - k : i > 4 && i <= 4 + k);
            if (lit == (pass == 1))
                b.dot(x + 3, y + 2 + 4.0f * (float) i, dimmed(i == 4 ? colours::primary : lit ? on : colours::dim, dim));
        }
}

void square(Graphics& g, float x, float y, bool on, Colour onColour, bool dim)
{
    Braille b(g);
    if (on)
        b.fill({ x, y, 8, 8 }, dimmed(onColour, dim));
    else
        b.box({ x, y, 8, 8 }, dimmed(colours::muted, dim));
}

void toggle(Graphics& g, float x, float y, bool on, const String& label, bool dim)
{
    square(g, x, y + 4, on, colours::accent, dim);
    text(g, label, x + 14, y, dimmed(on ? colours::primary : colours::muted, dim));
}

void box(Graphics& g, Rectangle<float> r, Colour c) { Braille(g).box(r, c); }

void led(Graphics& g, Rectangle<float> r, Colour c) { Braille(g).fill(r, c, true); }

void moduleGlyph(Graphics& g, Module m, float x, float y, Colour c)
{
    riso::use(g, c);
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
                    g.fillEllipse(cx - 3.75f + 3.0f * (float) i, cy - 7.75f + 2.4f * (float) j, 1.5f, 1.5f);
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
    Braille b(g);
    b.path(shackle, c), b.fill({ x + 1, y + 5.5f, 8, 6 }, c);
}

void handle(Graphics& g, float x, float y, Colour c)
{
    Braille b(g);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j)
            b.dot(x + 2 + 4.0f * (float) i, y + 2 + 4.0f * (float) j, c);
}

void arrow(Graphics& g, float x, float y, Colour c)
{
    Path p;
    p.startNewSubPath(x + 1, y + 8.5f), p.lineTo(x + 11, y + 8.5f);
    p.startNewSubPath(x + 7.5f, y + 5), p.lineTo(x + 11, y + 8.5f), p.lineTo(x + 7.5f, y + 12);
    riso::use(g, c), g.strokePath(p, PathStrokeType(1.1f));
}

void swapArrows(Graphics& g, float x, float y, Colour c)
{
    Path p;
    p.startNewSubPath(x + 1, y + 6), p.lineTo(x + 11, y + 6), p.startNewSubPath(x + 8.5f, y + 3.5f), p.lineTo(x + 11, y + 6), p.lineTo(x + 8.5f, y + 8.5f);
    p.startNewSubPath(x + 11, y + 11), p.lineTo(x + 1, y + 11), p.startNewSubPath(x + 3.5f, y + 8.5f), p.lineTo(x + 1, y + 11), p.lineTo(x + 3.5f, y + 13.5f);
    riso::use(g, c), g.strokePath(p, PathStrokeType(1.1f));
}

void menuIcon(Graphics& g, float x, float y, Colour c)
{
    Braille b(g);
    for (int i = 0; i < 3; ++i)
        b.line(x, y + 1 + 4.0f * (float) i, x + 14, y + 1 + 4.0f * (float) i, c);
}

void dice(Graphics& g, float x, float y, Colour c)
{
    Braille b(g);
    b.box({ x, y, 14, 14 }, c);
    for (int i = 0; i < 3; ++i)
        b.dot(x + 4 + 3.0f * (float) i, y + 4 + 3.0f * (float) i, c);
}

void heart(Graphics& g, float x, float y, bool filled)
{
    Path p;
    p.startNewSubPath(x + 6, y + 10.6f);
    p.lineTo(x + 1.7f, y + 6.3f);
    p.cubicTo(x + 0.2f, y + 4.6f, x + 1.2f, y + 1.8f, x + 3.4f, y + 1.6f), p.cubicTo(x + 4.6f, y + 1.5f, x + 5.5f, y + 2.3f, x + 6, y + 3.4f);
    p.cubicTo(x + 6.5f, y + 2.3f, x + 7.4f, y + 1.5f, x + 8.6f, y + 1.6f), p.cubicTo(x + 10.8f, y + 1.8f, x + 11.8f, y + 4.6f, x + 10.3f, y + 6.3f);
    p.closeSubPath();
    Braille(g).path(p, filled ? colours::accent : colours::muted, filled);
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
    Braille(g).path(p, c);
}

void gridDots(Graphics& g, float w, float h)
{
    Braille b(g);
    for (float x = 4; x < w; x += 8)
        for (float y = 4; y < h; y += 8)
            b.dot(x, y, colours::rule);
}
} // namespace snap::ui
