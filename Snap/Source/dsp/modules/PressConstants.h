#pragma once
#include <array>

namespace snap::pressConstants
{
struct Mode
{
    double attackMs, releaseMs, slowAttackMs, slowReleaseMs;
    float slowShare, ratioStart, ratioEnd, upRatio, kneeDb;
    float downStartDb, downEndDb, upStartDb, upEndDb, makeupDb;
    float feedback, preDrive, evenDrive, bandMakeup;
};

inline constexpr std::array<Mode, 6> modes {{
    // Attack and release, slow timings and share, ratios and knee, relative thresholds and makeup, feedback and colour.
    { 18,  85, 0,   0,    0,    4,    4,  2.0f,  3,  9, -5, -19,  -9, 3.5f, 0,    0,     0,   0 }, // tight
    { 24, 110, 100, 380,  0.85f, 2.5f, 3,  2.0f,  6,  9, -6, -20, -11, 3.5f, 0,    0,     0,   0 }, // glue
    { 0.3, 72, 0,   0,    0,    8,    16, 2.5f,  3, 10, -9, -19, -12, 4.5f, 0.72f, 0.55f, 0,   0 }, // crush
    { 32, 150, 0,   0,    0,    1.5f, 6,  2.0f, 12, 12, -7, -21, -13, 4.0f, 0,    0,     0,   0 }, // silk
    { 10,  60, 45,  1700, 0.35f, 3,    3,  2.0f,  9,  9, -6, -20, -11, 3.0f, 0,    0,     2.4f, 0 }, // lull
    { 4,  125, 0,   0,    0,    6,    12, 5.0f,  5,  9, -9, -21, -12, 4.5f, 0,    0,     0,   1 }  // stack
}};

inline constexpr std::array crossoverHz { 120.0f, 1000.0f, 6000.0f };
inline constexpr std::array stackMakeupDb { 0.75f, 0.25f, -0.25f, -0.75f };
inline constexpr double peakAttackMs = 0.25, peakReleaseMs = 12;
inline constexpr double referenceAttackMs = 500, referenceReleaseMs = 750;
inline constexpr float minLevel = 1.0e-6f, gateLowDb = -70, gateHighDb = -57;
inline constexpr float maxDownDb = 26, maxUpDb = 12, evenAsymmetry = 0.20f;
inline constexpr double evenDcBlockHz = 10; // removes the offset the even-harmonic term adds
} // namespace snap::pressConstants
