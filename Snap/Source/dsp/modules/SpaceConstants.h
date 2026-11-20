#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace snap::spaceConstants
{
struct Response
{
    std::string_view name;
    double rt60Seconds, decayLeadInSeconds = 0.0;
};

// Keep this order aligned with the stable space_type choices.
inline constexpr std::array<Response, 20> responses {{
    { "nook", 0.23 }, { "booth", 0.35 }, { "den", 0.48 }, { "attic", 0.62 },
    { "garage", 0.88 }, { "cellar", 1.12 }, { "chapel", 1.75 }, { "hangar", 2.34 },
    { "tunnel", 1.48 }, { "stairwell", 1.03 }, { "silo", 2.08 }, { "canyon", 2.60 },
    { "sheet", 1.55 }, { "coil", 1.25 }, { "unfurl", 1.92, 0.10 }, { "smear", 1.32 },
    { "gate", 0.53 }, { "rewind", 0.34, 0.31 }, { "shimmer", 2.16 }, { "fog", 2.40 }
}};

inline constexpr double minDecayScale = 0.35, maxDecayScale = 1.65;
inline constexpr double irFloorDb = -78.0, endFadeSeconds = 0.012;
inline constexpr int resamplePhases = 256, resampleHalfTaps = 12;
inline constexpr double resampleCutoff = 0.94;
inline constexpr double crossfadeSeconds = 0.075, warmupSeconds = 0.075;
inline constexpr int workerPollMs = 5;
inline constexpr float maxPredelayMs = 500.0f, percentScale = 0.01f;
inline constexpr int requestStepsPerPercent = 100;
inline constexpr float defaultFocusLoHz = 150.0f, defaultFocusHiHz = 12000.0f;
inline constexpr int headSize = 64;
inline constexpr std::array partitionSizes { headSize, 512, 4096 };
inline constexpr double maxStretch = 2.0; // space_stretch +100 plays the response an octave slower

// Longest response the convolver can be given (decay 100 %, stretch +100): the plug-in's reported tail.
inline constexpr double maxTailSeconds()
{
    double longest = 0;
    for (const auto& r : responses)
        longest = std::max(longest, r.decayLeadInSeconds + r.rt60Seconds * maxDecayScale * irFloorDb / -60.0);
    return longest * maxStretch;
}
} // namespace snap::spaceConstants
