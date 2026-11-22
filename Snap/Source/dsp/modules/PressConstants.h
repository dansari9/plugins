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
    { 30,  60, 0,   0,    0,    4,    4,  2.0f,  3,  9, -13, -19, -16, 3.5f, 0,    0,     0,   0 }, // tight
    { 24, 110, 100, 380,  0.85f, 2.5f, 3,  2.0f,  6,  9, -14, -20, -18, 3.5f, 0,    0,     0,   0 }, // glue
    { 0.3, 72, 0,   0,    0,    8,    16, 2.5f,  3, 10, -18, -19, -20, 4.5f, 0.72f, 0.55f, 0,   0 }, // crush
    { 6,  220, 0,   0,    0,    1.5f, 6,  2.0f, 12, 12, -15, -21, -20, 4.0f, 0,    0,     1.8f, 0 }, // silk
    { 10,  60, 45,  1700, 0.35f, 3,    3,  2.0f,  9,  9, -14, -20, -18, 3.0f, 0,    0,     2.4f, 0 }, // lull
    { 4,  125, 0,   0,    0,    6,    12, 5.0f,  5,  9, -17, -21, -20, 4.5f, 0,    0,     0,   1 }  // stack
}};

inline constexpr float attackOctaves = 3; // press_attack at -1 / +1: the mode's attack time x8 (slow) / x1/8 (fast)
inline constexpr std::array crossoverHz { 120.0f, 1000.0f, 6000.0f };
inline constexpr std::array stackMakeupDb { 0.75f, 0.25f, -0.25f, -0.75f };
inline constexpr double peakAttackMs = 0.25, peakReleaseMs = 12;
inline constexpr double referenceAttackMs = 500, referenceReleaseMs = 750;
inline constexpr float minLevel = 1.0e-6f, gateLowDb = -70, gateHighDb = -57;
inline constexpr float maxDownDb = 26, maxUpDb = 12, evenAsymmetry = 0.20f;
inline constexpr double evenDcBlockHz = 10; // removes the offset the even-harmonic term adds
} // namespace snap::pressConstants
