#pragma once
#include <array>

namespace snap::driveConstants
{
inline constexpr std::array<std::array<const char*, 2>, 6> flavorLabels {{
    { "thick", "bite" }, { "squeeze", "spark" }, { "dark", "open" },
    { "warm", "edge" }, { "alias", "grain" }, { "bell", "bend" }
}};

struct CrunchVoice { float bass, treble, bias, inputDrive, outputDrive; };
inline constexpr CrunchVoice crunch[] { { 0.85f, 0.0f, 0.22f, 2.2f, 1.45f },
                                        { 0.20f, 0.3f, 0.10f, 2.7f, 1.25f } };
inline constexpr double crunchBassHz = 420;

struct PocketVoice { float compand, drive, bias; double rolloffHz; };
inline constexpr PocketVoice pocket[] { { 3.0f, 4.0f, 0.13f, 7800 }, { 1.15f, 3.1f, 0.07f, 14500 } };
inline constexpr double pocketAttackMs = 0.65, pocketReleaseMs = 24;

struct ReelVoice { float emphasis, drive, memory, bump; double rolloffHz; };
inline constexpr ReelVoice reel[] { { 0.45f, 2.8f, 0.38f, 1.3f, 6500 }, { 0.8f, 2.1f, 0.26f, 0.9f, 13300 } };
inline constexpr double reelEmphasisHz = 2700, reelMemoryHz = 1300, reelBumpLoHz = 52, reelBumpHiHz = 116;

struct GlowVoice { float bias, drive, gridKnee, gridCompression; };
inline constexpr GlowVoice glow[] { { 0.36f, 1.9f, 0.19f, 0.8f }, { 0.12f, 3.05f, 0.10f, 1.65f } };

struct CrumbleVoice { double rateHz; float levels; };
inline constexpr CrumbleVoice crumble[] { { 11000, 511 }, { 39000, 11 } };

inline constexpr double wobbleCarrierHz = 820, wobbleDelaySeconds = 0.00019, wobbleSwingSeconds = 0.00012;
inline constexpr float wobbleRingDepth = 0.52f, wobblePhaseDepth = 0.68f, wobbleCarrierDrive = 3.4f, wobbleDelayDrive = 2.6f;

inline constexpr double dcBlockHz = 14, levelMatchMs = 25;
inline constexpr float levelFloor = 0.0001f, minMakeup = 0.60f, maxMakeup = 1.75f;
} // namespace snap::driveConstants
