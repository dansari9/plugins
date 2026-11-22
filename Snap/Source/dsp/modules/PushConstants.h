#pragma once
#include <array>

namespace snap::pushConstants
{
inline constexpr std::array stageCounts { 1, 2, 4 };
inline constexpr double tonePivotHz = 1200;
inline constexpr float toneOctaves = 1.0f; // 6 dB in opposite directions on either side of the pivot
inline constexpr float minDrive = 0.35f, maxDrive = 4.5f, referenceFloor = 1.0e-4f; // anchor for near-silence; above it the curve is level-independent
inline constexpr double peakAttackMs = 0.5, peakReleaseMs = 32;
inline constexpr float limiterThreshold = 1.02f, clipKnee = 0.90f, clipCeiling = 1.12f;
} // namespace snap::pushConstants
