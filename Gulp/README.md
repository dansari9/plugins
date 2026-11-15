# gulp

Sidechain pumping and ducking for dance music, by daveydidit. Version 0.1.0. Zero latency in every mode, mono or
stereo, with an optional mono or stereo sidechain input. One screen, no menus: 12 curves, 10 factory presets, user
presets and A/B.

Drop it on a bass, pad or bus and it pumps in time with the track straight away. Pick a curve, set how deep and how
long, and choose what fires it: the host tempo (**sync**), MIDI notes (**midi**, each note-on at its sample) or the kick
itself on the sidechain (**audio**: peak follower, threshold, re-armed 3 dB below, 30 ms hold-off). Duck the whole
signal or only the **lows** / **highs** of a Linkwitz-Riley split, on both channels, the mid or the side, and choose what
the duck does with **color**: level, bottom (low shelf), top (high shelf), dark (lowpass), thin (highpass), pinch
(band closing on 1 kHz), hollow (1 kHz notch) or grit (sample-rate drop), with **tone** as the one tweak.

## Controls

| ID | Control | Range | Default |
| --- | --- | --- | --- |
| `trigger` | Trigger | sync, midi, audio | sync |
| `tempoSync` | Tempo sync (off = **free**) | on/off | on |
| `rate` | Rate, one cycle | 1/32 … 4/1 incl. dotted (D) and triplet (T), 16 values | 1/4 |
| `freeHz` | Free rate | 0.1 … 20 Hz (log) | 2 Hz |
| `curve` | Curve | classic, tight, punch, round, long, slam, sub, sub long, trim, suck, wave, double | classic |
| `slope` | Slope: release length ×0.5 … ×2 | −100 … +100 % | 0 % |
| `invert` | Invert (duck ↔ push) | on/off | off |
| `depth` | Depth (100 % = silent at the bottom) | 0 … 100 % | 100 % |
| `shift` | Shift: ± half a cycle in sync, a trigger delay (positive only) in midi/audio | −50 … +50 % | 0 |
| `loop` | Loop in midi/audio (off = one shot, then full level until the next hit) | on/off | off |
| `thresh` | Audio trigger threshold | −60 … 0 dB | −24 dB |
| `band` | Band | full, lows, highs | full |
| `split` | Split frequency (LR4) | 20 … 5120 Hz (log) | 120 Hz |
| `output` | Output | −24 … +12 dB | 0 dB |
| `color` | Color | level, bottom, top, dark, thin, pinch, hollow, grit | level |
| `tone` | Tone (shelf frequency, resonance or smoothing, per color) | 0 … 100 % | 50 % |
| `stereo` | Stereo | both, mid, side | both |

The IDs, their order, ranges and choice lists are frozen: sessions and automation depend on them.

## Editor

One 120 × 34 cell grid in Geist Mono (scales 50–200 %, the size is saved with the session). Left: the curve list and
invert. Middle: one cycle as a tide — the red gain line, the audio that passes as shaded water under it, the sidechain's
kick as a grey ghost, the moving playhead, the live duck in dB and a ruler in fractions of the rate. Drag anywhere on
the tide to change the slope (◆ handle; double-click resets, click "slope n%" to type). Below: color and stereo words,
trigger, loop, rate (◁ ▷ step through all 16; **free** shows the Hz value instead), shift, thresh and tone. Right:
depth, band, split, output. Footer: rate, depth, a summary and the gain reduction. Every dial drags vertically,
double-click resets, click its value to type.

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
