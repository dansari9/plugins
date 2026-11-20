#pragma once

// Tunable values shared across the chain. Module-specific values live in dsp/modules/<Name>Constants.h.
namespace snap::constants
{
inline constexpr double smoothingSeconds = 0.02; // continuous-parameter ramp, long enough to avoid zipper noise

// Master transient shaper (inherited from 0.1.x so migrated sessions sound the same).
inline constexpr double shaperFastAttackMs = 0.3, shaperFastReleaseMs = 40, shaperSlowAttackMs = 20, shaperSlowReleaseMs = 300;
inline constexpr double shaperGainSmoothingMs = 1;
inline constexpr float shaperAttackDbPerPercentDb = 0.006f;  // gain dB per (Attack % x transient dB)
inline constexpr float shaperSustainDbPerPercentDb = 0.01f;  // gain dB per (Sustain % x tail dB)
inline constexpr float shaperMaxGainDb = 24.0f;
} // namespace snap::constants
