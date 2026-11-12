#include "Presets.h"

#include "Parameters.h"

#include <cstring>

namespace rootnote
{
const std::vector<Preset>& factoryPresets()
{
    using namespace ids;
    static const std::vector<Preset> presets = {
        { "Default", {} },
        { "Sub Afterglow",
          { { series, 0 }, { bands, 4 }, { boostQ, 80 }, { boostGain, 12 }, { ring, 40 }, { ringDecay, 900 },
            { ringDrop, 2 }, { limiter, 1 } } },
        { "Pocket Kick", { { series, 0 }, { bands, 6 }, { boostQ, 10 }, { boostGain, 6 }, { range, 1.0f } } },
        { "Snare Core",
          { { series, 2 }, { bands, 6 }, { boostQ, 18 }, { boostGain, 7 }, { cutOn, 1 }, { cutDepth, -10 } } },
        { "Tom Shift", { { series, 2 }, { bands, 8 }, { boostQ, 25 }, { boostGain, 8 }, { cutOn, 1 } } },
        { "Octave Weight", { { series, 1 }, { boostQ, 14 }, { boostGain, 8 }, { tilt, -6 } } },
        { "Resonance Eraser",
          { { boostGain, 0 }, { cutOn, 1 }, { cutDepth, -18 }, { cutBands, 12 }, { cutOvertones, 80 } } },
        { "Played Percussion",
          { { midiMode, 1 }, { glide, 40 }, { series, 0 }, { bands, 12 }, { boostQ, 60 }, { ring, 30 } } },
        { "Noise Lattice", { { series, 0 }, { bands, 24 }, { boostQ, 120 }, { boostGain, 18 }, { mix, 70 } } },
    };
    return presets;
}

bool presetKeeps(const char* parameterId)
{
    using namespace ids;
    for (const char* kept : { boostNote, boostFine, cutNote, key, scale, keyLock, a4, bypass })
        if (std::strcmp(parameterId, kept) == 0)
            return true;
    return false;
}
} // namespace rootnote
