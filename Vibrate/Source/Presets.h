#pragma once

#include "Engine.h"

namespace vibrate
{
struct Preset
{
    const char* name;
    Controls values;
};

inline constexpr Preset presets[] {
    { "init", defaults },
    { "breath",   { .10, .15, .20, .00, .15, 1.00 } },
    { "flicker",  { .00, .80, .10, .00, .00, 1.00 } },
    { "hats",     { .00, .60, .25, .00, .30, 1.00 } },
    { "dust",     { .15, .30, .00, .00, .40,  .60 } },
    { "tape",     { .45, .00, .00, .00, .00, 1.00 } },
    { "lean",     { .00, .20, .70, .00, .00, 1.00 } },
    { "keys",     { .35, .20, .30, .30, .20, 1.00 } },
    { "wide",     { .00, .00, .40, .00, 1.0, 1.00 } },
    { "pad",      { .55, .15, .50, .40, .50, 1.00 } },
    { "nervous",  { .20, 1.0, .30, .00, .10, 1.00 } },
    { "double",   { .70, .00, .60, .00, .20, 1.00 } },
    { "haze",     { .25, .10, .45, .60, .70,  .80 } },
    { "swirl",    { .10, .25, .00, 1.0, .20, 1.00 } },
    { "ensemble", { .85, .30, .75, .50, .60, 1.00 } },
    { "all",      { 1.0, 1.0, 1.0, 1.0, 1.0, 1.00 } },
};
inline constexpr int numPresets = static_cast<int>(std::size(presets));

struct PresetGroup { const char* name; std::array<int, 4> programs; };
inline constexpr PresetGroup presetGroups[] {
    { "subtle", { 0, 1, 4, 7 } },
    { "rhythm", { 2, 3, 6, 10 } },
    { "doubles", { 5, 8, 11, 14 } },
    { "space", { 9, 12, 13, 15 } },
};
} // namespace vibrate
