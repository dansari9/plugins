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

# Logic beep / missing editor, 0.4.2

**Conclusion:** real-processor Linux tests do not reproduce the reported beep;
the Logic incident's root cause remains unconfirmed. The reported Gatekeeper
rejection establishes a load problem.
A failed/cached AU load or routing mismatch plausibly explains both symptoms,
but there is no evidence that Logic generates a fallback beep. Removing
quarantine does not rescan cached validation. No editor construction/painting
failure was found. These defensive fixes do not prove the original beep's cause;
that requires a Mac/Logic render trace or recording.

JUCE 8.0.15 evidence (`juce_audio_plugin_client_AU_1.mm`, `juce_AU_Shared.h`,
`AU/AudioUnitSDK/AUBase.cpp`):

- `AudioProcessorHolder` enables buses; MIDI capability adds dispatch, not a
  different audio bus. `SupportedNumChannels` derives 1→1, 1→2 and 2→2 from
  our predicate. Disabled input, 2→1 and surround are rejected; failed layout
  synchronization stops initialization.
- `Initialize` synchronizes/allocates, then prepares at the output sample rate
  and maximum frames per slice. AUBase rejects uninitialized/oversized renders
  before our callback, without filling the caller's output. Format/frame-limit
  changes require uninitialization. Engine already chunks oversized direct
  processor calls. A missed direct re-prepare retains old tuning but stays
  finite; re-preparing clears the tail.
- `Render` copies/deinterleaves fresh input, clears failed pulls and unused
  channels, processes, then copies out. An early processor return is therefore
  dry/silent in valid AU rendering. Repeating stale host audio could sound at
  fs/blockSize and harmonics, but no trace demonstrates it here. Processor
  guards cannot repair AUBase's earlier error exits.
- AU floats and ints expose 0…1; choices expose 0…(steps−1), bools 0…1.
  JUCE's `AudioParameterInt` snaps internally but does not override `isDiscrete`.
  Defaults are normalized defaults times the AU maximum. Tests check all 32
  defaults, every integer/choice/bool value and 101 points per float. Log
  mappings, endpoints and monotonicity are correct. Stepped floats allow half
  a step of snapping.
  State restore is covered. Velocity-zero is note-off; ordinary CCs are ignored;
  All Notes/Sound Off respect Hold. MIDI retunes, never excites Ring. The tuner
  only reads audio; denormals are disabled in the callback.
- Cocoa UI creation calls `createEditorAndMakeActive`; `hasEditor()` is true.
  Tests create/reopen it before preparation and verify a nonblank 960 × 540
  painting, plus existing desktop/resize/state/tuner coverage. Linux cannot
  validate Cocoa embedding or Logic's selected window view.

Two old-code failures were reproduced: unprepared mono→stereo left its extra
output untouched; injected NaN/Inf poisoned recursive state and subsequent
silence even with limiting. Outputs are now initialized before early returns,
incomplete/unprepared/released calls return dry, and extra buses are rejected.
Non-finite wet chunks reset audio state and return retained dry audio, replacing
invalid dry samples with zero, without allocating another buffer. Neither
failure is established as the Logic incident's cause.

The AU is now **`aufx RtNt Dwks`** for normal drum-track Audio FX insertion;
VST3 retains optional MIDI. This avoids instrument/side-chain routing surprises
but changes AU session identity: see [migration/install instructions](../README.md#install).
CI signs/verifies both Mac bundles before `auval -v aufx RtNt Dwks` and packaging;
ad-hoc signing is not notarization.

The processor matrix covers 44.1/48/96 kHz, all three layouts, nine programs,
six extreme series/Q-mode combinations, and no MIDI/channel 1/channel 10.
Inputs include +12 dB kick/snare/hat hits, full-scale clicks, a +12 dBFS DC step,
silence and restored states. Callback sizes 1/17/64/512/1024/4096/8193 exceed
prepared sizes 17/512. Samples must be finite, peaks ≤ limiter ceiling + 1e-6,
and initial/reset silence exactly zero, including MIDI-only input. Every
setting/rate/MIDI combination gets a full tail measurement, balanced across
layouts: final 0.5 s after two reported T60 tails must be below −90 dBFS RMS.
Earlier high-Q/Ring tails are intentional. AU validation, quarantine handling
and Logic listening remain macOS CI/user-host checks.

0.4.2 Linux Release validation: warning-free CMake/Ninja build; 106 core tests
and 37 plugin tests passed (405 stress renders, 135 complete tail measurements);
VST3 pluginval passed at strictness 10 with `--validate-in-process`.
