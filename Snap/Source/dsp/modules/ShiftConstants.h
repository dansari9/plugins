#pragma once

// shift: STFT pitch and formant shifter. Tunable values.
namespace snap::shiftConstants
{
inline constexpr double referenceRate = 48000;
inline constexpr int referenceOrder = 11, overlap = 4; // 2048-point FFT at 48 kHz, scaled to the nearest power of two; hop = N / 4
inline constexpr float maxSemitones = 12, amountExponent = 2, semitonesPerOctave = 12; // semitones = 12 * a * |a|: fine near centre
inline constexpr float pivotLowHz = 60, pivotCentreHz = 250, pivotHighHz = 2000; // Stretch -1 / 0 / +1: formant warp fades out below
inline constexpr float pivotTaperOctaves = 1.5f;
inline constexpr double lifterSeconds = 0.002;  // cepstral lifter length for the spectral envelope
inline constexpr float magnitudeFloor = 1.0e-6f, maxEnvelopeGainLog = 1.3862944f; // envelope correction limited to +-12 dB
inline constexpr float onsetPowerRatio = 4, onsetPowerFloor = 1.0e-9f; // hop power jump that resets the phases (onset)
inline constexpr int onsetResetFrames = 3;
inline constexpr double peakReleaseMs = 12, attackRecoveryMs = 1; // dry peak follower; how fast an abrupt attack hands back to wet
inline constexpr float attackPeakRatio = 4, attackPeakFloor = 0.01f; // a sample this far over the running peak passes dry
inline constexpr float peakHeadroom = 3, engagementSemitones = 0.25f; // wet ceiling over the dry peak; shift that fully engages the wet
} // namespace snap::shiftConstants
