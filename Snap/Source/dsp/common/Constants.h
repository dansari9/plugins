#pragma once

// Tunable values shared across the chain. Module-specific values live in dsp/modules/<Name>Constants.h.
namespace snap::constants
{
inline constexpr double smoothingSeconds = 0.02; // continuous-parameter ramp, long enough to avoid zipper noise
inline constexpr float denormalFloor = 1.0e-15f; // smoothers and followers snap to zero below this

// Master transient shaper (inherited from 0.1.x so migrated sessions sound the same).
inline constexpr double shaperFastAttackMs = 0.3, shaperFastReleaseMs = 40, shaperSlowAttackMs = 20, shaperSlowReleaseMs = 300;
inline constexpr double shaperGainSmoothingMs = 1;
inline constexpr float shaperAttackDbPerPercentDb = 0.006f;  // gain dB per (Attack % x transient dB)
inline constexpr float shaperSustainDbPerPercentDb = 0.01f;  // gain dB per (Sustain % x tail dB)
inline constexpr float shaperMaxGainDb = 24.0f;

// Focus band and crossovers.
inline constexpr float focusMinHz = 20.0f, focusMaxHz = 20000.0f; // an edge at either end drops out of the path exactly
inline constexpr double crossoverMaxFraction = 0.45;               // highest usable cutoff, as a fraction of the sample rate

// Target (transient/sustain mask).
inline constexpr double maskFastAttackMs = 0.5, maskFastReleaseMs = 10, maskSlowAttackMs = 20, maskSlowReleaseMs = 150;
inline constexpr float maskDeadZoneDb = 1.0f;    // follower ripple on a steady tone stays at zero
inline constexpr float maskRangeDb = 12.0f;      // fast-over-slow difference (above the dead zone) that counts as fully transient
inline constexpr double maskSmoothingMs = 1.0;   // light smoothing on the mask itself
inline constexpr double maskLookaheadMs = 3.0;   // normal mode
inline constexpr int lowLatencySamples = 64;     // total plug-in latency in Low Latency Mode
inline constexpr int lowLatencyMaskLookahead = 16; // mask/detector lookahead inside that budget, in samples

// Hit detector (sub/hiss triggers).
inline constexpr double hitAttackMs = 0.1, hitReleaseMs = 30, hitRetriggerMs = 40;
inline constexpr float hitHysteresisDb = 6.0f;    // the level must fall this far below threshold to re-arm
inline constexpr float hitVelocityRangeDb = 24.0f; // peak over threshold that maps to full velocity
inline constexpr float hitMinVelocity = 0.3f;      // velocity of a hit that barely crosses the threshold

// Ducker (sub, hiss and space Duck).
inline constexpr double duckAttackMs = 1.0, duckReleaseMs = 150;
inline constexpr float duckFloorDb = -48.0f;  // key level where ducking starts; full key (0 dBFS) gives the full depth
inline constexpr float duckDepthDb = 30.0f;   // reduction at Duck 100 % and a full-scale key

// Loudness Match.
inline constexpr double loudnessWindowSeconds = 3.0; // BS.1770 short-term
inline constexpr double loudnessHopSeconds = 0.1;
} // namespace snap::constants
