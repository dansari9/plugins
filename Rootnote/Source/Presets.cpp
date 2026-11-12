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
        { "Kick to 808 Tail",
          { { series, 0 }, { bands, 4 }, { boostQ, 80 }, { boostGain, 12 }, { ring, 40 }, { ringDecay, 900 },
            { ringDrop, 2 }, { limiter, 1 } } },
        { "Kick in Key", { { series, 0 }, { bands, 6 }, { boostQ, 10 }, { boostGain, 6 }, { range, 1.0f } } },
        { "Snare Body Tune",
          { { series, 2 }, { bands, 6 }, { boostQ, 18 }, { boostGain, 7 }, { cutOn, 1 }, { cutDepth, -10 } } },
        { "Tom Retune", { { series, 2 }, { bands, 8 }, { boostQ, 25 }, { boostGain, 8 }, { cutOn, 1 } } },
        { "Octave Thump", { { series, 1 }, { boostQ, 14 }, { boostGain, 8 }, { tilt, -6 } } },
        { "Clean Slate",
          { { boostGain, 0 }, { cutOn, 1 }, { cutDepth, -18 }, { cutBands, 12 }, { cutOvertones, 80 } } },
        { "MIDI Melodic Perc",
          { { midiMode, 1 }, { glide, 40 }, { series, 0 }, { bands, 12 }, { boostQ, 60 }, { ring, 30 } } },
        { "Noise to Tone", { { series, 0 }, { bands, 24 }, { boostQ, 120 }, { boostGain, 18 }, { mix, 70 } } },
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
