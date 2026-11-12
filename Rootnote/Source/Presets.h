#pragma once

#include <vector>

namespace rootnote
{
struct PresetValue
{
    const char* id;
    float value; // in the parameter's own units; choices and switches by index
};

/**
    A factory preset: a name and the parameters that differ from their
    defaults. Loading one puts every other parameter on its default, apart
    from the ones that belong to the song (see presetKeeps()).
*/
struct Preset
{
    const char* name;
    std::vector<PresetValue> values;
};

/** "Default" followed by the eight factory presets. */
const std::vector<Preset>& factoryPresets();

/**
    Parameters a preset never touches: the notes, the fine tune, key, scale,
    key lock, A4 and bypass. They describe the song, not the sound.
*/
bool presetKeeps(const char* parameterId);
} // namespace rootnote
