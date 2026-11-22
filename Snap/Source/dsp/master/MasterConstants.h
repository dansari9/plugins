#pragma once
#include <numbers>

namespace snap::masterConstants
{
inline constexpr float cutLoOpenHz = 20.0f, cutLoMaxHz = 2000.0f;
inline constexpr float cutHiMinHz = 1000.0f, cutHiOpenHz = 20000.0f;
inline constexpr double maxCutoffFraction = 0.45;
inline constexpr float cutLoPresenceOctaves = 1.0f, cutHiPresenceOctaves = 0.5f;
inline constexpr double butterworthQ = std::numbers::sqrt2 / 2;
inline constexpr double cutLoSteepFirstQ = 0.54, cutLoSteepSecondQ = 1.8;
inline constexpr double cutHiSteepFirstQ = 0.541196100146197, cutHiSteepSecondQ = 1.306562964876377;

inline constexpr int toneCoefficientInterval = 16;
inline constexpr double toneShapeFadeSeconds = 0.01;
inline constexpr float toneNeutralFade = 0.05f;
inline constexpr double toneShelfSlope = 0.8;
inline constexpr double tiltHz = 800, tiltDb = 6;
inline constexpr double bumpLowHz = 80, bumpLowDb = 6, bumpMidHz = 300, bumpMidDb = -4, bumpMidQ = 0.9;
inline constexpr double smileLowHz = 200, smileMidHz = 1000, smileHighHz = 4000;
inline constexpr double smileShelfDb = -4, smileMidDb = 8, smileMidQ = 0.65;
inline constexpr double focusPresenceHz = 2500, focusPresenceDb = 4, focusPresenceQ = 0.8;
inline constexpr double focusSideCutDb = -2, focusSideShelfHz = 6000, focusSideShelfDb = 6;
inline constexpr double focusMidShelfHz = 8000, focusMidShelfDb = -2;

inline constexpr float airHz = 5500.0f;
inline constexpr double airAttackMs = 1.0, airReleaseMs = 60.0;
inline constexpr float airGapDb = 0.0f, airRatioSlope = 0.85f, airBoostMaxDb = 12.0f;
inline constexpr float airGateDb = -60.0f, airGateOpenDb = -48.0f, airSideDrive = 1.5f;

inline constexpr float shaperRangeDb = 12.0f;
inline constexpr int clipOversamplingStages = 2;
inline constexpr float clipCeilingDb = -0.1f, clipKneeDb = 2.0f;
inline constexpr float clipReconstructionKneeDb = 0.05f;
} // namespace snap::masterConstants
