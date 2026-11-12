"""Builds the embedded UI fonts from the Geist Mono variable font (OFL 1.1).

    python3 make_fonts.py GeistMono-VariableFont_wght.ttf <out dir>

Static Regular/Medium/SemiBold instances, subset to the characters the editor
draws, without OpenType features (no ligatures: "->" stays two characters).
Geist Mono has no music sharp, so a drawn U+266F is added.
"""
import string
import sys

from fontTools import subset
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

TEXT = string.printable + "·→←−●○◀▶…°"
WEIGHTS = {"Regular": 400, "Medium": 500, "SemiBold": 600}


def add_sharp(font, weight):
    """A music sharp: two stems, the right one higher, and two bars rising to the right."""
    stem = 30 + (weight - 400) * 0.08
    bar = 2.0 * stem
    pen = TTGlyphPen(None)

    def quad(points):  # clockwise, as TrueType wants for filled outlines
        pen.moveTo(points[0])
        for p in points[1:]:
            pen.lineTo(p)
        pen.closePath()

    for x, low, high in ((230, -60, 650), (350, -20, 690)):
        quad([(round(x), low), (round(x), high), (round(x + stem), high), (round(x + stem), low)])
    for y in (170, 400):
        rise = 90
        quad([(125, round(y)), (125, round(y + bar)), (475, round(y + bar + rise)), (475, round(y + rise))])

    glyph = pen.glyph()
    name = "uni266F"
    font["glyf"].glyphs[name] = glyph
    order = font.getGlyphOrder() + [name]
    font.setGlyphOrder(order)
    font["glyf"].glyphOrder = order
    glyph.recalcBounds(font["glyf"])
    font["hmtx"].metrics[name] = (font["hmtx"].metrics["numbersign"][0], glyph.xMin)
    font["maxp"].numGlyphs = len(order)
    for table in font["cmap"].tables:
        if table.isUnicode():
            table.cmap[0x266F] = name


def main(source, out_dir):
    for style, weight in WEIGHTS.items():
        font = instancer.instantiateVariableFont(TTFont(source), {"wght": weight}, updateFontNames=True)
        options = subset.Options()
        options.layout_features = []
        options.name_IDs = ["*"]
        options.notdef_outline = True
        subsetter = subset.Subsetter(options)
        subsetter.populate(text=TEXT)
        subsetter.subset(font)
        add_sharp(font, weight)
        font.save(f"{out_dir}/GeistMono-{style}.ttf")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
