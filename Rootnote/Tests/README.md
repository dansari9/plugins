# Test comparisons

Parameter expectations use `parameterTolerance`: 1% of the parameter step or
eight float epsilons relative to the expected value, whichever is larger.
This accommodates normalisation, logarithmic conversion and snapping without
accepting a neighbouring step. Defaults, presets, state, typed values and
zero Fine are checked with tolerances. The parameter round-trip test exercises
every range and verifies the helper also rejects changed values.

The audit covers every `==`/`!=` in this directory. Computed pitches, levels,
responses, decay times and series ratios use numerical tolerances. Intentional
exact checks remain for integers/enums/text, copied knob values, clamped
endpoints, silence, FIFO copies, unchanged state, bypass transparency and
identical processing across buffer sizes/channels. Those tests assert exact
behaviour; weakening them would hide data loss or unwanted audio processing.

# Review 1 regressions

| Finding | Regression coverage |
| --- | --- |
| 1 | Engine limiter with 32 wide bands; enormous dry input at crossfade endpoints |
| 2 | Engine measured decay at low/MIDI/high pitches, both Q modes; overdamped cuts |
| 3 | Plugin state restore after MIDI with unchanged knob values |
| 4 | MIDI channel/note identity, per-channel All Notes Off, routing changes including Hold, full fixed capacity |
| 5 | TransientGate offsets 0/1/100; Engine enabling Ring during sustained audio |
| 6 | ModalRing octave series with C7 root and 65 Hz focus |
| 7 | ModalRing opposite-polarity stereo excitation |
| 8 | ModalRing Drop crossing the Nyquist guard without accumulated guard tone |
| 9 | AnalysisWorker concurrent reads during publication and sample-rate resets |

Plugin tests also cover editor-gated spectrum/peaks, reopening without stale
samples, live smoothed band gains and concurrent complete UI snapshots.
Finding 10 is intentionally outside this change.

# Review 2 regressions

The editor tests send real mouse and key events through a desktop window's
peer (`Desk`), not direct callback calls. Under `xvfb-run` there is no window
manager, so `main()` first defines the X atoms JUCE expects and `Desk` hands
its window the keyboard focus.

| Finding | Regression coverage |
| --- | --- |
| 1 | Preset and settings menu results delivered after the editor closed are dropped |
| 2 | Preset/size kept out of `apvts.state`; concurrent restores while reading; 0.4.0 states |
| 3 | Gesture counts for a drag across a page remap, a same-page reselect and closing the editor |
| 4 | Clicking/Alt-clicking the unchanged knob note after MIDI took the bank (also core `takeBack`) |
| 5 | Program count, names, values equal to `loadPreset()`, no gestures, call from another thread |
| 6 | Legacy 400×320 size opens at 720×405; oversize clamps to 1920×1080 |
| 7 | Alt/Cmd/right-click and legend-click text entry with the parameter's parser, Escape, arrow keys, focus order |
| 8 | Blend's encoder rebound as Fine formats and parses cents; choices use their own text |
| 9 | Hold past `kPagePeekMs` peeks and restores; a short click selects |

Also found by pluginval with some random seeds (0.4.0 as well): a restored
state now settles switches and steps the host left between legal values;
the state round-trip test covers it.
