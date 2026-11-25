# crumb

A bit crusher and lossy codec by daveydidit. Version 0.4.0. Mono or stereo. Zero latency while the codec is off; while loss
or drop is up it reports one long frame (2048 samples at 44.1 / 48 kHz, 4096 at 88.2 / 96, 8192 above). One screen, 41 factory
presets, user presets, A / B.

Signal path: drive (0 … +48 dB) into a soft ADC clip → filter (pre) → **crush** (dither, quantizer and DAC with fixed vintage
ladder errors → anti-alias lowpass → sample & hold on a jittered clock → anti-image lowpass → DC coupling) → gate → **loss** (frame
codec → packet loss) → filter (post) → mix → output.

The crush stage is 0.2's, bit for bit: antiderivative antialiased clip, hold steps bandlimited with a minimum-phase BLEP, the
virtual ADC reading through the same kernel, so the only aliasing is the emulated fold-down. Clean settings null.

## Loss

- An MDCT codec (`Source/Codec.h`), sine windows, half overlap. **speed** sets the hop, 1024 samples (slow: smear, pre-echo) to
  64 (fast: chatter) in nine steps, twice and four times that at 88.2 and 176.4 kHz and up; a change crossfades two coders.
- **loss** ranks each frame's coefficients (by how far they stand over an ear model's masking threshold, or with **weighting** `flat` by
  size), keeps the top share (all of them at 0 %, 1.2 % at 100 %), coarsens those and fills emptied bands with a faint noise.
  **delta** plays what it threw away, exactly the rest.
- **packet loss** loses frames in bursts (a two-state chain); **packets** `drop` plays them as gaps, `repeat` plays the last good
  frame again. Works at loss 0.
- **stereo**: sides coded and lost separately; `joint`: mid / side, lost together; `mono`: the sum.
- **jitter** also rotates the codec's coefficients and wobbles its clock.

## Filter, gate

Low and high cut, Butterworth 6 … 96 dB / oct with resonant edges, before the crush or after the codec; `invert` cuts what lies between
them. Both edges open is the input, bit for bit. The gate silences the wet path under its threshold.

## Parameters

The IDs are frozen; the names on the panel and in the host are 0.4.0's.

| ID | Control | Range | Default |
| --- | --- | --- | --- |
| `preamp` | drive | 0 … 48 dB | 0 dB |
| `gate` | gate | −96 (off) … 0 dB | off |
| `stereo` | (stereo mode) | stereo / joint / mono | stereo |
| `bits` | bits | 0 (off), 1 … 24 | 12 |
| `rate` | rate | 44 Hz … 44.1 kHz (log) | 26.04 kHz |
| `jitter` | jitter | 0 … 100 % (up to ±3 % of the rate) | 0 % |
| `dither` | dither | 0 … 100 % of ½ LSB | 0 % |
| `approxOn`, `approxDev` | anti-alias | off, −100 … +100 % = cutoff Nyquist(rate) · 2^(±1 oct) | 0 % |
| `imagesOn`, `imagesShift` | anti-image | off, 0 … 100 % = cutoff Nyquist(rate) · 2^(0 … 2 oct) | off |
| `dcShift` | dc shift | off (mid-riser) / on (mid-tread) | off |
| `loss`, `speed`, `drop` | loss, speed, packet loss | 0 … 100 % | 0, 100, 0 % |
| `ear` | weighting | perceptual / flat | perceptual |
| `fill` | packets | drop / repeat | drop |
| `keep` | delta | off / on | off |
| `lowCut`, `highCut` | low cut, high cut | 20 Hz … 20 kHz (log) | open |
| `reso` | resonance | 0 … 100 % | 0 % |
| `slope` | slope | 6, 12, 18, 24, 36, 48, 72, 96 dB / oct | 24 |
| `fOrder` | position | pre / post | post |
| `invert` | invert | off / on | off |
| `mix` | mix | 0 … 100 % | 100 % |
| `output` | output | −24 … +12 dB | 0 dB |
| `bypass` | (the host's bypass, not on the panel) | on / off | off |
| `autoGain`, `auto` | (auto gain until 0.3.0, not on the panel) | on / off, 0 … 100 % | off, 100 % |
| `fType`, `cutoff` | (0.2's filter, not on the panel) | off, lp, bp, hp, br; 20 Hz … 20 kHz | off |

**Auto gain** brought back the level drive and the codec changed. 0.4.0 has no control for it and starts with it off. A session, A/B
slot or user preset that stored it on still runs it and sounds as it did; loading a factory preset or randomize turns it off. The
factory presets have what it did to them baked in: drive's share exactly (mix and output), the codec's as a fixed output gain.

## Editor

One monospace character grid (132 × 41 cells, 1056 × 656 at 100 %): `crumb < preset > A B randomize`, the picture, and every control on
one page by section (in · crush · loss · filter · out): two rows of dials, loss's and filter's `< name >` selectors stacked under
their dials, the clip meters and the output's peak under out. A selector's slot is the longest of its names and a cell either
side; its arrows click, wheel and key-step, and wrap. Dials: drag, double-click = default, click the value to type; past the end
of bits, anti-alias and anti-image lies off. A click on the preset's name lists them all and saves.

**The picture** (`Source/Wave.h`) is drawn from what the audio thread wrote for every sample and nothing else: the dry input lined
up with the output (the dim ghost), the output itself (the outline in the ink), what the codec took away (the dots inside) and the
codec's own decision for the frame the sample came from (a lost frame prints in the cool ink: flat when a gap, the repeat when
held). While the codec runs a step is one of its frames, its true lowest and highest sample; otherwise the samples themselves are
drawn, four dots a hold, triggered so they stand still: bit steps, hold steps and what the filter smooths. Turning a crush control
shows the samples, a loss control the frames; a preset goes back to automatic. No audio: a flat line.

**Clipping** is drawn where it happens, from the same per-sample feed. Where the drive's clip pressed a sample past the converter's
full scale (1.1 dB or more off it; late with the codec, not shown while it is not heard) the crest prints in the hot red, a dot deeper every
2 dB up to 8; where the output is past 0 dBFS a red rail stands on the picture's edge. `clip` in out: the light, and a meter each for
drive and output, a cell a dB, its highest held 0.6 s. Stopped: nothing lit.

## Presets

Factory presets are the host's programs. 0 `init` and 1 … 24 are 0.2's (same numbers, same sound); 25 … 40 are 0.3's, `256k stream` to `hard
crush`; the arrows step init, 0.3's, 0.2's, then the user's. Presets turn auto gain off.

User presets are plain XML files (`user 01.xml`, …) in the user's app-data folder under `daveydidit/crumb/Presets`
(macOS `~/Library/daveydidit/crumb/Presets`, Windows `%APPDATA%\daveydidit\crumb\Presets`, Linux
`~/.config/daveydidit/crumb/Presets`). Rename or delete them there; the arrows list them in name order after the factory set.

## Install

Copy `crumb.vst3` to `~/Library/Audio/Plug-Ins/VST3` (macOS), `%COMMONPROGRAMFILES%\VST3` (Windows) or `~/.vst3`
(Linux), and on macOS `crumb.component` to `~/Library/Audio/Plug-Ins/Components`. The builds are ad-hoc signed:
if macOS blocks them, run `xattr -dr com.apple.quarantine` on the bundle. Logic lists crumb under daveydidit.

## Build

```bash
cmake -S Crumb -B Crumb/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Crumb/build --target Crumb_VST3 Crumb_Standalone CrumbTests
xvfb-run -a Crumb/build/CrumbTests        # Linux; run it directly on macOS
xvfb-run -a Crumb/build/CrumbTests --screenshot editor.png 1440 [clip|quiet|crush]  # the editor after a drum + bass loop
xvfb-run -a Crumb/build/CrumbTests --render in.f32 out.f32 preset=28                # a file through the processor
xvfb-run -a Crumb/build/CrumbTests --bench-editor   # editor timer and paint ms at 100 % and 200 %
```

On macOS, use `-G Xcode` and build `Crumb_AU` and `Crumb_VST3`. `auval -v aufx Crmb Dwks` validates the AU.

## Frozen identifiers

| What | Value |
| --- | --- |
| AU type | `aufx` (Audio FX, no MIDI) |
| Manufacturer code | `Dwks` |
| Plug-in code | `Crmb` |
| Bundle ID | `com.dripworks.crumb` |
| Product name | `crumb` (display only; bundle file names) |
| Parameter IDs | the 31 IDs in `Crumb.cpp`, in that order (version hint 1; `autoGain` 2; 0.3.0's 3) |

## State versioning

The state is the parameter tree as XML (`<CRUMB stateVersion="4">`), validated by a flat-schema scanner before XML decoding. Loading
ignores unknown entries, clamps out-of-range values, resets missing or unreadable values to their default, and
leaves the state alone for data that is not a complete Crumb state.

- Never rename, remove or reorder a parameter ID, or change a range so that old values move. Only append.
- Additive root attributes (since milestone 3, ignored by older builds, optional when loading): `preset`
  (factory index, −1 = user/unknown), `presetName` (user preset), `ab` (active slot), `slotA` / `slotB`
  (`preset;v0,…,v29`: every parameter but bypass, normalised, 9 digits; 0.1.x wrote 16, 0.2.x 17), `slotNameA` / `slotNameB`
  (user preset names), and `editorWidth`.
- Bump `stateVersion` only when stored data changes meaning; translate older data in the migration hook in
  `setStateInformation`. The v1, v2 and v3 fixtures in `Tests/CrumbTests.cpp` must keep loading unchanged.
- v2 (0.2.0) appends `autoGain`. Sessions, A/B slots and user presets without it (v1) load it off, so they sound as before.
- v3 (0.3.0) appends `loss` … `auto`. Older data lacks them and loads them neutral: the codec, the filter's edges and the gate off, `auto`
  100 % of the old switch, so it sounds as before. Its filter keeps running as `fType` / `cutoff`; the panel shows it on low / high cut
  and moves it there when one of the filter's controls is edited.
- v4 (0.4.0) stores the same parameters. `autoGain`'s default is off; stored values, older ones too, are kept and run (above).
