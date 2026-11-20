#pragma once

#include <array>
#include <string_view>

namespace snap::subHissConstants
{
inline constexpr double subAttackMs = 1.0, hissAttackMs = 0.65, retriggerFadeMs = 2.0;
inline constexpr double pitchFallMs = 30.0, filterFallMs = 85.0;
inline constexpr float pitchDropSemitones = 12.0f, velocityTracking = 0.5f;
inline constexpr float decayEndGain = 0.001f, tailFloor = 0.00001f;
inline constexpr float subGain = 0.6f, hissGain = 0.75f;
inline constexpr float lowSensitivityDb = -4.0f, highSensitivityDb = -49.0f;
inline constexpr float subDefaultThresholdDb = -16.0f, hissDefaultThresholdDb = -19.0f;
inline constexpr float stereoCrossoverHz = 120.0f, stereoGain = 0.42f;
inline constexpr double stereoDelayMs = 1.7;
inline constexpr double filterUpdateHz = 6000.0;
inline constexpr float colorDrive = 2.6f, colorBias = 0.17f;
inline constexpr float acidMinDrive = 2.1f, acidColorDrive = 2.2f;
inline constexpr float acidCutoffHz = 830.0f, acidResonance = 1.35f, dcCutoffHz = 10.0f;
inline constexpr float stereoSecondHarmonic = 0.6f, stereoThirdHarmonic = 0.65f;
inline constexpr float hissTonePivotHz = 2700.0f, hissTilt = 0.62f;
inline constexpr float hissOpenHz = 18000.0f, hissClosedOctaves = 3.6f, hissMaxResonance = 1.8f;
inline constexpr double hissSampleRate = 48000.0;

inline constexpr std::array<std::string_view, 18> noiseNames {
    "fizz", "hush", "drizzle", "sizzle", "grit", "rustle", "crinkle", "spray", "steam",
    "clatter", "jangle", "scrape", "breath", "buzz", "flutter", "sand", "chime", "swarm"
};
} // namespace snap::subHissConstants
