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
