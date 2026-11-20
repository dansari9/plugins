#pragma once
#include "../chain/ChainOrder.h"
#include <juce_audio_processors/juce_audio_processors.h>

// Session state: the APVTS tree as XML, <SNAP stateVersion="2" order="..."> with one <PARAM id value/> per parameter.
// Version 1 (snap 0.1.x) held attack, sustain, mix and output and is migrated on load.
namespace snap
{
inline constexpr int stateVersion = 2;

void writeState(juce::AudioProcessorValueTreeState&, const ChainOrder&, juce::MemoryBlock&);

// Loads a state written by any version. Data that is not one complete <SNAP> element changes nothing.
void readState(juce::AudioProcessorValueTreeState&, ChainOrder&, const void* data, int size);
} // namespace snap
