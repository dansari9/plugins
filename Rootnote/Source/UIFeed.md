# Processor/editor contract

`RootnoteEditor` is replaceable. Bind controls to the 32 parameters in
`getApvts()`; parameter IDs and units are in `Parameters.h/.cpp`. MIDI never
writes automation parameters. Presets, A/B, tuner actions and editor size are
processor operations, so another editor needs no DSP access.

Call the editor interface on the JUCE message thread. Balance
`setDisplayActive(true)` on opening with `false` on closing. There may be
multiple activity users, but only one consumer of the display/spectrum/peaks.

| API | Meaning and threading |
| --- | --- |
| `displayState()` | Latest complete audio frame: sample rate, live boost/cut Hz, MIDI ownership and note-on counter, linear limiter gain, actual smoothed band Hz/gains. Before audio arrives, computes a preview from APVTS. A four-frame SPSC queue drops new frames when full; the UI drains it once per frame and retains its last complete reading. |
| `tunerReading()` | Input pitch, hit counts, clarity and glide. Worker/UI share one short mutex; the audio producer never takes it. Invalid readings disable Match/Tune. |
| `readSpectrumSamples(dest, count)` | Bounded SPSC FIFO of mono output samples at the display sample rate. One reader. FFT and windowing belong to the editor. A full FIFO drops an entire incoming block. |
| `takeOutputPeak(channel)` | Atomic peak since the last read, linear amplitude, channel 0 or 1. UI applies meter decay. |
| `matchCutToHit()` / `tuneBoostToKey()` | Apply the detected pitch via host-notified APVTS gestures. Match also enables Cut; Tune snaps to the selected scale and clears Fine. |
| `loadPreset()`, `selectAbSlot()` | Message-thread operations; all sound values go through APVTS. Host state restoration invalidates A/B using an atomic request consumed on the message thread. |

Limiter reduction in dB is `-20 * log10(limiterGain)`. MIDI activity is a
change in the unsigned note-on counter (wraparound is harmless).

With no editor, the processor skips all display publication, spectrum copies,
peak scans and tuner input copies. Reopening discards stale spectrum, peaks
and display frames. Analysis may finish an already queued block; it never
affects the audio. The audio thread allocates no memory, takes no locks and
sends no message-thread notifications. State restores request MIDI release
with release/acquire ordering before the next block reads the parameters.
