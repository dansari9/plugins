# gulp

Sidechain pumping and ducking for dance music, by daveydidit. Version 0.4.1. Zero latency unless audio-trigger lookahead
is on (5 ms), mono or stereo, with an optional mono or stereo sidechain input. One screen, no menus: 12 curves, 10 factory presets, user
presets and A/B.

Drop it on a bass, pad or bus and it pumps in time with the track straight away. Pick a curve, set how deep and how
long, and choose what fires it: the host tempo (**host**), MIDI notes (**midi**, each note-on at its sample) or the kick
itself on the sidechain (**audio**: peak follower, threshold, re-armed 3 dB below, 30 ms hold-off). Choose what the duck
does with the mode: **vol** (level), **lows** / **highs** (only below / above the Linkwitz-Riley **split**), dark
(lowpass), thin (highpass), pinch (band closing on 1 kHz), hollow (1 kHz notch) or grit (sample-rate drop), on both
channels, the mid or the side. **soothe** rounds the curve's hard corners (the same blur in the graph and the audio), so
the attack eases in; MIDI/audio envelopes also taper to full level at the cycle boundaries. Changes to a soothed
shape crossfade over 20 ms to avoid automation steps; soothe at zero preserves the original DSP.

## 0.4.1

- trigger and stereo are `‹ name ›` cyclers (David's note): the arrows step through host / midi / audio and both / mid /
  side and wrap; the wheel and the arrow keys step too. Same parameters and values; the sound is unchanged.

## 0.4.0

- The graph's telemetry holds still while no audio flows (silent input and sidechain), as with a stopped transport, so
  nothing moves without sound; the knob glows end at the dial rings. The sound is unchanged (bit-exact).

## Controls

| ID | Control | Range | Default |
| --- | --- | --- | --- |
| `trigger` | Trigger (index 0 was named sync until 0.2.0) | host, midi, audio | host |
| `tempoSync` | Tempo sync (off = **free**) | on/off | on |
| `rate` | Rate, one cycle | 1/32 … 4/1 incl. dotted (D) and triplet (T), 16 values | 1/4 |
| `freeHz` | Free rate | 0.1 … 20 Hz (log) | 2 Hz |
| `curve` | Curve | classic, tight, punch, round, long, slam, sub, sub long, trim, suck, wave, double | classic |
| `slope` | Slope: release length ×0.5 … ×2 | −100 … +100 % | 0 % |
| `invert` | Invert (duck ↔ push) | on/off | off |
| `depth` | Depth (100 % = silent at the bottom) | 0 … 100 % | 100 % |
| `shift` | Nudge: ± half a cycle in host, a trigger delay (positive only) in midi/audio. Stored as % of a cycle, shown and typed in ms at the current rate and host tempo (120 BPM without one; 1 / Hz in free) | −50 … +50 % | 0 ms |
| `loop` | Loop in midi/audio (off = one shot, then full level until the next hit) | on/off | off |
| `thresh` | Audio trigger threshold | −60 … 0 dB | −24 dB |
| `band` | unused since 0.2.0 (kept, not automatable) | full, lows, highs | full |
| `split` | Split frequency (LR4) | 20 … 5120 Hz (log) | 120 Hz |
| `output` | Output | −24 … +12 dB | 0 dB |
| `color` | Mode | vol, lows, highs, dark, thin, pinch, hollow, grit | vol |
| `tone` | unused since 0.2.0 (kept, not automatable; the modes use its old default) | 0 … 100 % | 50 % |
| `stereo` | Stereo | both, mid, side | both |
| `soothe` | Soothe: rounds the curve's corners | 0 … 100 % | 0 % |
| `lookahead` | Lookahead, audio trigger only: delays the input 5 ms (reported as latency) while the sidechain is read live, so the duck lands on the kick | on/off | off |

The IDs, their order, ranges and choice indices are frozen: sessions and automation depend on them. 0.1.0 sessions
load: band lows / highs with level becomes the lows / highs mode (the same sound), band and tone are otherwise ignored,
soothe is off. 0.1.0 and 0.2.0 sessions load with lookahead off.

## Editor

One 120 × 34 cell grid in Geist Mono (scales 50–200 %, the size is saved with the session), printed in a multicolour riso
"pump" skin: cream text, pink / tangerine / sun accents by region, a cobalt-lilac shadow and a dim braille field in the
graph. Left: the curve list. Middle: one cycle as a tide — the gain line heating from pink to sun across the cycle, the audio that passes as shaded water under it, the sidechain's kick as
a grey ghost, the playhead and a ruler in fractions of the rate. The nudge slides the curve against the beat; the kick
and the ruler stay put. Above it: the gain reduction (dotted) and its held peak (click it, or start the transport, to
reset), invert, the rate (‹ › step from 1/32 to 2 bars; a session saved at 4 bars keeps it; in **free** the Hz value, drag or click to type) and sync / free.
Drag the ◆ sideways to change the slope (double-click resets, click "slope n%" to type). Below: the modes, then
trigger, loop (dim with the host trigger, which ignores it), lookahead (under audio, dim unless the audio trigger), the sidechain meter, stereo, nudge (ms), thresh and
soothe. Right: depth, split (live for lows and highs), output and its meter. With the host's transport stopped the graph holds still; with no transport at all (standalone) it runs.
Every dial drags vertically, double-click resets, click its value to type.

## Build and test

From the repository root:

```bash
cmake -S Gulp -B Gulp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Gulp/build --target Gulp_VST3 Gulp_Standalone GulpTests
xvfb-run -a Gulp/build/GulpTests                     # Linux; on macOS run it directly
Gulp/build/GulpTests --screenshot editor.png        # the editor at 100 % (2x pixels), preset 03 with a kick
```

On macOS add the `Gulp_AU` target; the AU validates with `auval -v aumf Gulp Dwks` (a MIDI-controlled effect, so Logic
can route a MIDI track into it from the plug-in header's side-chain menu).

Geist Mono is under the SIL Open Font License (`Assets/Fonts/OFL.txt`).
