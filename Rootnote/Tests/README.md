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

**Conclusion:** Linux processor tests do not reproduce the reported beep; its
cause remains unconfirmed. The reported Gatekeeper rejection establishes a
load problem, and a failed/cached AU scan plausibly explains the missing GUI.
No editor construction/painting failure was found. Removing quarantine still
requires a rescan; there is no evidence of a Logic fallback beep.

Two beep hypotheses remain:

- **(a) MIDI retuning:** the old MIDI-controlled AU could receive the drum
  track's channel-1 notes. Its Ch 1+2 default retuned Boost on every hit;
  +9 dB, Q 12 and eight harmonics could ring at beep-like pitches, held near
  −0.3 dBFS by the limiter. Harmonics of drum notes (e.g. 65/73 Hz) would
  support this explanation.
- **(b) Host render failure:** a repeated stale buffer could produce harmonics
  of sampleRate/bufferSize (375 Hz at 48 kHz/128, 93.75 Hz at 48 kHz/512).
  No render trace demonstrates this here; processor guards cannot repair
  errors returned before the processor callback.

0.4.2 addresses (a) with the plain **`aufx RtNt Dwks`** AU and MIDI **Off** by
default. VST3/Standalone retain optional MIDI; Played Percussion enables it
explicitly, and saved sessions retain their stored mode. The AU identity
change requires [migration/rescanning](../README.md#install). CI signs and
verifies the bundles before installation, `auval` and packaging; ad-hoc
signing is not notarization. These changes do not prove the incident's cause.
A recording/pitch, Plug-in Manager status and Console logs from
`AudioComponentRegistrar`/`CoreServicesUIAgent` would distinguish the hypotheses.

JUCE's AU/VST3 wrappers already enforce valid layouts and preparation before
rendering. Tests retain mono→stereo copying before preparation, valid 1→1,
1→2 and 2→2 processing, silence after sample-rate preparation, normalised
parameter sweeps, state restoration and a nonuniform editor snapshot. Linux
cannot validate Cocoa embedding or Logic's selected window view.

Injected NaN/Inf previously poisoned recursive audio state. The last wet
sample now detects that state, resets it and returns dry audio with invalid
samples zeroed. The analysis feed also replaces non-finite samples, with
mono/stereo recovery coverage. Neither issue is established as the Logic cause.

The deterministic stress matrix has 17 renders: nine presets at 48 kHz in
stereo without MIDI, six extreme series/Q settings at 48 kHz with channel-1
MIDI and alternating 1→2/2→2 layouts, and one extreme at 44.1/96 kHz. Only the
six extremes at 48 kHz measure full tails. Loud drum hits, clicks, DC, restored
states and callback sizes 1–8193 exercise finite output, limiter bounds and
exact initial/reset silence. The final half-second after two reported T60
tails must be below −90 dBFS RMS. Earlier high-Q/Ring tails are intentional.

# 0.4.3 changes and audit

| Change / finding | Regression coverage |
| --- | --- |
| Octave keys move the sounding note (boost; Alt: cut), clamped, one gesture; keys follow the note's octave | Engine band doubles after `oct +`; automation moves the keys' octave; MIDI note shifted and taken back; two presses within a frame |
| Match cut toggles Cut; LED = Cut on; no hit blinks (Tune to key too) | Toggle on/off with a detected hit, blink without, LED after automation |
| Every legend value is typed in place on the screen; garbage reverts | Every slot of every page: garbage and own-text round trip; natural formats for `parseValue` and host text |
| Cut page has no Note encoder | Encoder absent, six encoders shown |
| Focus window drawn from `rangeWeight` | Centre and ±Range follow Focus, Range, link and the boost note; hidden at 0 dB |
| Focus was dead while linked | Turning it unlinks; legend shows LINK / ALL / N/A where a value has no effect |
| A page key held and released off the key swallowed the next click | Hold, drag off, release; the next click selects |
| Host text entry: choices were case-sensitive, Bool/Int accepted junk as 0 | `parsed()` checks for choices, bands, switches and the default fallback |

# 0.4.3 reliability

| Finding / requirement | Coverage |
| --- | --- |
| A/B slots and the active slot were lost with the session; the editor page too | Full round trip with random non-default values: bit-exact parameters, A/B, preset, size, page, identical bytes on re-save, identical audio |
| A truncated or unclosed state (`<RootnoteState pres…`) parsed leniently as empty and reset every setting | Truncations at every 7 bytes and unclosed XML leave the state unchanged |
| Selecting the current program again must reload it (AU preset menu); restoring never selects a program | Reselecting reloads the preset; the round trip keeps edits; program-state paths equal the state |
| `prepareToPlay(0)` would prepare the engine with no rate | Unprepared processor passes audio through |
| Thread creation failure in the tuner would throw out of `prepareToPlay` | Caught: the tuner stays silent (not testable without exhausting threads) |
| Load order / rates | Restore before or after preparing, while playing, with an editor open, at 44.1/48/96/192 kHz and blocks 7–1024: same parameters and sample-identical audio |
| Old sessions | Real 0.4.0/0.4.1/0.4.2 blobs (`fixtures/`) with edits and defaults |
| Hostile states | Random bytes with and without header, bit flips, wrong root, NaN/Inf/text/out-of-range values, unknown fields, 8 MB field, bad A/B: never crash, always legal and playable |
| Frozen identity | Codes, bundle ID, schema, and the ordered parameter IDs, version hints, ranges and defaults |
| Lifecycle | 60 create/play/edit/save/restore/destroy cycles with editors: no extra threads, under 16 MB growth |
| Review 4: save, restore and A/B switch could interleave across threads | A listener mid-switch saves from a second thread (blocked until the switch ends, saves all B); a switch mid-restore runs after it; a same-thread save mid-switch does not deadlock |
| Review 4: a restore with the editor open kept the old page, size and a typed field | Restore with a field open: the editor shows the restored page and size, the field is dropped and writes nothing |
| Review 4: state numbers were read by prefix (`1.2.3`, `2-1`, `0.4.5`) | Exactly one number or missing: malformed values default, fractional page/preset/slot index are invalid, a malformed A/B slot is dropped |
| Review 4: opening and closing a field unedited rounded Q or unlinked Focus | Return and focus loss on every boost value with Focus linked and continuous values: nothing changes, no gesture |
| Review 4: a display refresh between two octave presses brought back the stale MIDI note | Press, refresh, press: 40 → 52 → 64; newer MIDI wins again |
| Review 4: Blend took the text after the last slash; huge integers overflowed | `garbage/70`, `30/20/70`, `30/60` are no value; Bands at 2^31 and beyond clamps to 32 for host and display alike; beyond a float is no value |
