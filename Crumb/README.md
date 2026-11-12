# Crumb

A vintage-sampler bit crusher by daveydidit. Version 0.1.0. Zero latency, mono or stereo, both channels share the
settings. One screen, no menus: 25 factory presets, user presets, A/B, a live scope and an output meter.

Signal path: preamp (0 … +48 dB) into a soft ADC clip (tanh shoulder above 0.5, antiderivative antialiasing) →
filter (pre) → dither, quantizer and DAC with fixed vintage ladder errors on the four MSBs → approx (anti-alias)
lowpass → sample & hold on a jittered virtual clock → images lowpass → fixed DAC coupling (converter-added DC
removal) → filter (post) → mix → output. Antialiasing is engaged while the preamp is up or the input passes the
knee; below it the clip is exact identity, so clean settings null.

The hold steps are bandlimited with a minimum-phase BLEP, and the virtual ADC reads the input through the same
causal kernel, so the only aliasing is the emulated fold-down at the virtual rate. No oversampling, no lookahead.
When the rate is below the host rate the wet path has about 7 host samples of group delay plus half a hold period.
At rate = host rate (no jitter) the sample & hold is the identity.

## Controls

| ID | Control | Range | Default |
| --- | --- | --- | --- |
| `preamp` | Preamp | 0 … 48 dB | 0 dB |
| `bits` | Resolution | 0 (OFF), 1 … 24 bit | 12 |
| `dcShift` | DC Shift | off = mid-riser, on = mid-tread | off |
| `dither` | Dither | 0 … 100 % of ½ LSB | 0 % |
| `rate` | Rate | 44 Hz … 44.1 kHz (log) | 26.04 kHz |
| `jitter` | Jitter | 0 … 100 % (up to ±3 % of the rate) | 0 % |
| `approxOn` | Approx filter | on/off | on |
| `approxDev` | Approx deviation | −100 … +100 % = cutoff Nyquist(rate) · 2^(±1 oct) | 0 % |
| `imagesOn` | Images filter | on/off | off |
| `imagesShift` | Images shift | 0 … 100 % = cutoff Nyquist(rate) · 2^(0 … 2 oct) | 0 % |
| `fType` | Filter type | OFF, LP, BP, HP, BR | OFF |
| `fOrder` | Filter order | Pre, Post | Post |
| `cutoff` | Cutoff | 20 Hz … 20 kHz (log) | 20 kHz |
| `reso` | Reso / bandwidth | 0 … 100 % (Q 0.5 … 20) | 0 % |
| `mix` | Mix | 0 … 100 % | 100 % |
| `output` | Output | −24 … +12 dB | 0 dB |
| `bypass` | Bypass (host bypass parameter) | on/off | off |

## Editor

- **Header:** preset number and name, ◀ ▶ step through the factory presets and then your saved ones, **save** writes the
  current sound as a new user preset, **A / B** keep two complete sounds and switch between them.
- **Screen:** the rate and resolution in large type, the converter / resampler / filter status top right, and a live
  scope: 20 ms of the input (grey) and the crushed output (orange), triggered on the input so it stands still.
- **Quantizer:** − / + step the bits (0 = off), **dc shift** switches mid-riser / mid-tread.
  **Resampler:** **approx filter** (anti-alias lowpass) and **images filter** (reconstruction lowpass).
  **Filter:** off / lp / bp / hp / br and pre / post.
- **Knobs:** FREQ (rate), JITTER, DEVIATE (approx cutoff), SHIFT (images cutoff); PREAMP, DITHER, CUTOFF, RESO;
  FX (mix) and OUTPUT, next to the stereo OUT meter. Drag up/down, **double-click = default**, **click the value
  under a knob to type** one (`9.2k`, `-3`, `35`).
- **Signal path:** the chain as it runs now. Stages that do nothing are grey; the filter is bold where it sits (pre/post).
- The window scales from 50 % to 200 % (drag the corner; the aspect is fixed) and remembers its size per instance.

## Presets

Factory presets are the host's programs (Logic's preset menu lists them). 0 `init` (the defaults) ·
vintage-sampler flavours, named bits·rate: 1 `12·26k dusty` · 2 `12·40k warm` · 3 `8·22k grit` · 4 `16·40k clean` ·
5 `10·dark` · 6 `12·32k punch` · 7 `8·11k crunch` · 8 `12·26k tread` · 9 `8·16k fold` · 10 `6·8k toy` ·
11 `14·44k glue` · 12 `12·20k thick` · by use: 13 `drums · crunch bus` · 14 `drums · break dust` ·
15 `bass · warm 12` · 16 `bass · sub grit` · 17 `keys · lo-fi` · 18 `vocal · radio` · 19 `vocal · phone` ·
20 `hats · sizzle` · 21 `pad · drift` · 22 `fx · alias sweep` · 23 `fx · 700 hz wreck` · 24 `mix · parallel crush`.

User presets are plain XML files (`user 01.xml`, …) in the user's app-data folder under `daveydidit/crumb/Presets`
(macOS `~/Library/daveydidit/crumb/Presets`, Windows `%APPDATA%\daveydidit\crumb\Presets`, Linux
`~/.config/daveydidit/crumb/Presets`). Rename or delete them there; ◀ ▶ list them in name order after the factory set.

## Install

Copy `Crumb.vst3` to `~/Library/Audio/Plug-Ins/VST3` (macOS), `%COMMONPROGRAMFILES%\VST3` (Windows) or `~/.vst3`
(Linux), and on macOS `Crumb.component` to `~/Library/Audio/Plug-Ins/Components`. The builds are ad-hoc signed:
if macOS blocks them, run `xattr -dr com.apple.quarantine` on the bundle. Logic lists Crumb under daveydidit.

## Build

```bash
cmake -S Crumb -B Crumb/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Crumb/build --target Crumb_VST3 Crumb_Standalone CrumbTests
xvfb-run -a Crumb/build/CrumbTests        # Linux; run it directly on macOS
xvfb-run -a Crumb/build/CrumbTests --screenshot editor.png   # the editor at 100 % (2x pixels) after a drum + bass loop
```

On macOS, use `-G Xcode` and build `Crumb_AU` and `Crumb_VST3`. `auval -v aufx Crmb Dwks` validates the AU.

## Frozen identifiers

| What | Value |
| --- | --- |
| AU type | `aufx` (Audio FX, no MIDI) |
| Manufacturer code | `Dwks` |
| Plug-in code | `Crmb` |
| Bundle ID | `com.dripworks.crumb` |
| Product name | `Crumb` |
| Parameter IDs | the 17 IDs above, in that order (version hint 1) |

## State versioning

The state is the parameter tree as XML (`<CRUMB stateVersion="1">`), validated by a flat-schema scanner before XML decoding. Loading
ignores unknown entries, clamps out-of-range values, resets missing or unreadable values to their default, and
leaves the state alone for data that is not a complete Crumb state.

- Never rename, remove or reorder a parameter ID, or change a range so that old values move. Only append.
- Independent L/R (pending): append `link` (default on) and per-channel twins (`rateR`, `bitsR`, …). Old
  sessions lack them and load linked, identical on both channels.
- Additive root attributes (since milestone 3, ignored by older builds, optional when loading): `preset`
  (factory index, −1 = user/unknown), `presetName` (user preset), `ab` (active slot), `slotA` / `slotB`
  (`preset;v0,…,v15`: the 16 non-bypass parameters, normalised, 9 digits), `slotNameA` / `slotNameB` (user preset names), and `editorWidth`. A state without them
  (0.1.0 milestone 1) loads as before, shown as preset "custom", slot A, default size.
- Bump `stateVersion` only when stored data changes meaning; translate older data in the migration hook in
  `setStateInformation`. The v1 fixture in `Tests/CrumbTests.cpp` must keep loading unchanged.
