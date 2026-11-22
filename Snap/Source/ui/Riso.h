#pragma once
#include "../dsp/modules/Modules.h"
#include <juce_gui_basics/juce_gui_basics.h>

// The riso look over the ascii skin: the one accent becomes an ink field (one ink per 240 px module column, edges
// dithered between neighbours, Resources/riso/field.png), the bright neutrals become cream paper ink, visualizers get a
// dim printed texture behind their dots and the amount dials a faint halftone glow. Only colours change, never a position.
// Textures come from Tools/generate_riso.py; they decode once on first use, on the message thread.
namespace snap::ui::riso
{
inline const juce::Colour paper { 0xffefe6d2 };
inline const juce::Colour inks[] { juce::Colour { 0xff5b7cff }, juce::Colour { 0xff2fd3c4 }, juce::Colour { 0xffa98bff },
                                   juce::Colour { 0xffffc83d }, juce::Colour { 0xffff6a2f }, juce::Colour { 0xff8a8a8a } }; // column 0..5
constexpr float textureOpacity = 0.22f, glowOpacity = 0.12f;
constexpr float dialGlowEdge = 43.5f; // the 88 px amount dial's ring: dots at radius 41.5 px, 2 px dot radius
constexpr int grainLift = 5; // levels

const juce::Image& field();                       // 1440 x 840, layer coordinates
juce::Colour ink(juce::Colour, juce::Point<float>); // a palette colour as printed at a layer position; others unchanged
// Sets g to draw c the riso way at the painting component's lattice origin: accent through the field pixel by pixel.
void use(juce::Graphics&, juce::Colour);
juce::Colour inkAt(const juce::Component&); // the accent as printed at a component's centre (carets, text selections)
const juce::Image& backdrop();        // the background with grain and the six amount-dial glows, 1440 x 808
const juce::Image& texture(Module);   // a visualizer's 212 x 104 backdrop (transparent where empty)
} // namespace snap::ui::riso
