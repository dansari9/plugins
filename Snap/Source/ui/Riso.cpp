#include "Riso.h"
#include "Braille.h"
#include "Layout.h"
#include "RisoData.h"
#include "Style.h"

using namespace juce;

namespace snap::ui::riso
{
namespace
{
bool isAccent(Colour c) { return (c.getARGB() & 0xffffff) == (colours::accent.getARGB() & 0xffffff); }

// The bright neutrals print in paper ink, as far up from the background as they were.
Colour paperFor(Colour c)
{
    const auto rgb = c.getARGB() & 0xffffff;
    if (rgb != (colours::primary.getARGB() & 0xffffff) && rgb != (colours::secondary.getARGB() & 0xffffff))
        return c;
    const float t = jmin(1.0f, (float) (c.getRed() - 13) / (236 - 13));
    return colours::bg.interpolatedWith(paper, t).withAlpha(c.getAlpha());
}

constexpr int bayer4[4][4] { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

// A halftone radial glow, size x size, in one ink: 4 px dots on a 30 degree screen, coverage falling off from the centre,
// cut off at `edge` px from the centre (the dial ring's outer edge) so no ink spills past the ring.
void glow(Image::BitmapData& d, int x0, int y0, int size, Colour c, float edge)
{
    const float a = degreesToRadians(30.0f), half = (float) size / 2;
    const auto add = [&](uint8 v) { return (uint8) (13 + roundToInt((float) v * glowOpacity)); }; // the dots light the background
    const Colour dot { add(c.getRed()), add(c.getGreen()), add(c.getBlue()) };
    const auto wrap = [](float v) { return v - 4 * std::floor(v / 4); };
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const float distance = std::hypot((float) x - half, (float) y - half);
            if (distance > edge)
                continue;
            const float v = std::pow(jmax(0.0f, 1 - distance / half), 1.6f);
            const float xr = (float) x * std::cos(a) + (float) y * std::sin(a), yr = -(float) x * std::sin(a) + (float) y * std::cos(a);
            if (std::hypot(wrap(xr) - 2, wrap(yr) - 2) / (4 / 1.414f) < std::sqrt(v) * 0.95f)
                d.setPixelColour(x0 + x, y0 + y, dot);
        }
}
} // namespace

const Image& field()
{
    static const Image image = ImageFileFormat::loadFrom(RisoData::field_png, (size_t) RisoData::field_pngSize);
    return image;
}

Colour ink(Colour c, Point<float> p)
{
    if (!isAccent(c))
        return paperFor(c);
    const auto& f = field();
    return f.getPixelAt(jlimit(0, f.getWidth() - 1, (int) p.x), jlimit(0, f.getHeight() - 1, (int) p.y)).withAlpha(c.getAlpha());
}

Colour inkAt(const Component& c) { return ink(colours::accent, layerOffset(c) + c.getLocalBounds().getCentre().toFloat()); }

void use(Graphics& g, Colour c)
{
    if (!isAccent(c))
        return g.setColour(paperFor(c));
    g.setFillType(FillType(field(), AffineTransform::translation(-Braille::origin.x, -Braille::origin.y)));
    g.setOpacity(c.getFloatAlpha());
}

const Image& backdrop()
{
    static const Image image = [] {
        Image im(Image::RGB, 1440, 840, false);
        im.clear(im.getBounds(), colours::bg);
        Image::BitmapData d(im, Image::BitmapData::readWrite);
        // Tactile grain: a sparse ordered-dither / hash pick of background pixels, lifted a few levels.
        const auto lift = (uint8) (13 + grainLift);
        const Colour lifted { lift, lift, lift };
        for (int y = 0; y < im.getHeight(); ++y)
            for (int x = 0; x < im.getWidth(); ++x)
                if (bayer4[y % 4][x % 4] == 15 && ((uint32) x * 73856093u ^ (uint32) y * 19349663u) % 100 < 45)
                    d.setPixelColour(x, y, lifted);
        // The amount dials' glows, centred on each column's 88 px dial (push stays plain).
        for (int k = 0; k < 5; ++k)
            glow(d, (int) layout::colX(k) + 108 - 60, (int) layout::amountTop + 50 - 60, 120, inks[k], dialGlowEdge);
        return im;
    }();
    return image;
}

const Image& texture(Module m)
{
    static const std::array<Image, 6> images = [] {
        const std::pair<const char*, int> data[] { { RisoData::viz0_png, RisoData::viz0_pngSize }, { RisoData::viz1_png, RisoData::viz1_pngSize },
                                                   { RisoData::viz2_png, RisoData::viz2_pngSize }, { RisoData::viz3_png, RisoData::viz3_pngSize },
                                                   { RisoData::viz4_png, RisoData::viz4_pngSize }, { RisoData::viz5_png, RisoData::viz5_pngSize } };
        std::array<Image, 6> a;
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = ImageFileFormat::loadFrom(data[i].first, (size_t) data[i].second);
        return a;
    }();
    return images[(size_t) m];
}
} // namespace snap::ui::riso
