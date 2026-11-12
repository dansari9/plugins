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
| `displayState()` | Newest complete audio frame: sample rate, live boost/cut Hz, the note each bank plays (knob or MIDI, key lock applied, before Fine and glide), MIDI ownership and note-on counter, linear limiter gain, actual smoothed band Hz/gains. A triple buffer: audio swaps in each finished frame, the UI swaps out the newest, so it is never stale and never torn. Before audio arrives, or 250 ms after the host stopped processing, it is a preview computed from APVTS so knob moves still show. |
| `tunerReading()` | Input pitch, hit counts, clarity and glide. Worker/UI share one short mutex; the audio producer never takes it. Invalid readings disable Match/Tune. |
| `readSpectrumSamples(dest, count)` | Bounded SPSC FIFO of mono output samples at the display sample rate. One reader. FFT and windowing belong to the editor. A full FIFO drops an entire incoming block. |
| `takeOutputPeak(channel)` | Atomic peak since the last read, linear amplitude, channel 0 or 1. UI applies meter decay. |
| `setKnobNote(bank, note)` | Sets the boost or cut note as a gesture and takes that bank back from a held MIDI note, even if the value is unchanged (an atomic request the next audio block consumes). Use it for every explicit note choice. |
| `parseValue(id, text, a4)` (Parameters.h) | Typed text in a parameter's units, or nothing for text that is no value. The parameters' host text conversion uses it too. |
| `matchCutToHit()` / `tuneBoostToKey()` | Apply the detected pitch via host-notified APVTS gestures and `setKnobNote()`. Match also enables Cut; Tune snaps to the selected scale and clears Fine. |
| `loadPreset()`, `selectAbSlot()` | Message-thread operations; all sound values go through APVTS. A/B slots are saved and restored with the state, behind a mutex that is never held while parameters are set. |
| `currentPreset()`, `storedEditorSize()` / `storeEditorSize()`, `storedEditorPage()` / `storeEditorPage()` | Atomics, never `apvts.state`: a host may replace that tree on another thread. They are added to the saved state and read back from it. |

The factory presets are also the host's programs. `setCurrentProgram()` may
come from any thread: it sets precomputed normalised values with no gestures,
allocations or state-tree changes.

Limiter reduction in dB is `-20 * log10(limiterGain)`. MIDI activity is a
change in the unsigned note-on counter (wraparound is harmless).

With no editor, the processor skips all display publication, spectrum copies,
peak scans and tuner input copies. Reopening discards stale spectrum, peaks
and display frames. Analysis may finish an already queued block; it never
affects the audio. The audio thread allocates no memory, takes no locks and
sends no message-thread notifications. State restores request MIDI release
with release/acquire ordering before the next block reads the parameters.
