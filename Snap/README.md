# snap

A one-screen drum multi-FX by daveydidit. Version 0.5.4. Mono or stereo.

The 0.2 chain is six modules plus a master section, with one **Magnitude** macro that scales the
module amounts and the master's continuous controls toward neutral:

```
in -> input -> sub/hiss -> [shift | space | press | drive, any order] -> push -> master -> output
```

A new instance starts on the factory preset **hit hard**, a deliberately loud showcase. Turn Magnitude
down to hear less of it, or load another preset. With every control at its parameter default (all
amounts 0), snap passes the audio through unchanged, delayed by its constant latency.

- **sub/hiss**: a sub synth and 18 procedural noise layers, triggered by the drums.
- **shift**: pitch and formant shift (off in Low Latency Mode).
- **space**: 20 generated impulse responses.
- **press**: a four-band compressor (downward, upward or parallel).
- **drive**: six saturation modes with a Flavor blend.
- **push**: density shaper and limiter, x1/x2/x4.
- **master**: Input, Cut, Tone (4 shapes), Air, Attack/Sustain, Clip, Output, and Loudness Match.

The middle four modules can be reordered without clicks. Locks, four snapshots (A–D) and undo live in
the session. Latency is constant whatever is switched on or reordered; Low Latency Mode is exactly 64
samples.

The editor is one screen (1440 x 840 at 100 %, resizable 75–200 % from the corner or the ≡ menu): the top bar
(presets, dice, save, Magnitude, menu), six module columns in chain order (drag a middle column's ⋮⋮ grip to
reorder), the amount row (on/off, the big dial, hover for the lock), and the master strip, one group under each column (0.5.1: no bottom bar; snapshots A and B sit in the top bar). Every dial and fader: drag (Shift finer), Alt/Cmd-click resets,
double-click types a value with units, the wheel nudges, right-click opens a menu. The preset name lists the
presets; **browse** in the list opens the browser over the module columns: click a row to audition, ok keeps it, cancel goes back.

## 0.5.4

- The preset list: a click on the preset's name lists every factory preset in short groups, the current one inverted, then the user's and **browse** (the browser: search, favourites, collections). A click or return loads and
closes; esc, the name again or a click outside closes; the arrow keys walk the list. The list is the same in gulp, silt, snap and crumb
(`PresetMenu.h`). Nothing else changed.

## 0.5.3

- The compare slots are A and B (David: "I only need A B"); randomize, save, undo and redo close the gap. A session's C and D
  stay in its state.

## 0.5.2

David's notes on the v2 frames:
- press and drive modes, push's stages and the master tone shape are `< name >` selectors like space's type (the arrows
  step and wrap; the wheel and the arrow keys step too). Same parameters, same values.
- The captions under sub/hiss, shift and space (generated layers, balance, 03 / 20) are gone.
- space's pan/width pad is replaced by a width dial under stretch, beside duck. Space Pan stays a parameter (automation,
  sessions and presets keep it) with no control on the editor.
- One selector row: the dials, faders and lower bands of all six columns sit 24 px higher, right under it, and the
  window is 1440 x 784 (the amount row and master moved up with them). On the hiss tab, hiss's type is on the selector
  row beside the sub / hiss tabs, which share its left half.
- push's stages selector sits on the selector row with the other selectors (12 px higher), 12 px above its dials.

## 0.5.1

David's notes on the Logic window (the window is 1440 x 808 now):
- Top bar as silt's: `snap < preset > A B C D randomize save undo redo`, the word randomize in place of the dice; the
  text sits midway between the window's top and the rule; a gap between magnitude and its fader.
- The help line and its bar are gone (snapshots A–D moved to the top bar; the bypass parameter stays, reached from
  the host). The master strip puts one group under each module column on the columns' own dial slots.
- The lower bands (sens / detect, target / focus, attack) are full-width bars with their name and value above them,
  each a 32 px grab; centred fader lines (balance, down/up, bands, flavor) sit midway in their column; the sub/hiss
  toggles are one row apart.
- Every module's visualizer draws every one of its controls (a change redraws the still picture); a test changes each
  of the 70 and checks the picture changes. The amount dials print in their column's one ink.

## 0.5.0

- Every control makes a clearly audible change on drums: sub and hiss are louder (and hiss env also shortens the hiss),
  each space room is level-trimmed so the wet sits about 6 dB under the dry at 100 %, press compresses deeper and its six
  modes have distinct characters (punch lets hits through, warm adds even harmonics), drive saturates harder (tube and
  tape have their own tone), push drives harder with a wider tone, and air adds air on bright material.
  `SnapTests --audibility` (also in the full run) fails if any control's change on the kit loop drops below 2 dB.
- press gains **attack** (`press_attack`, appended, version hint 3): slow to fast around each mode's own attack, with
  the effective time under it. 0, the default, is the mode as before, so older sessions load unchanged.
- Visualizers move only with the audio: space draws its own wet signal, drive its input and what it adds, push its input
  and the change it makes; with the module off, the plug-in bypassed or no audio flowing every picture settles and stops.
  Bypass now dims every column too.
- The preset name box is as wide as the longest factory name, so `<` and `>` stay put; the amount-dial glows end at the
  dial ring. A test clicks the middle of every key and control through the editor's own hit testing at 75, 100 and 150 %.

## Build

```bash
cmake -S Snap -B Snap/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Snap/build
xvfb-run -a Snap/build/SnapTests        # Linux; run it directly on macOS
```

On macOS, use `-G Xcode` and build `Snap_AU` and `Snap_VST3`. The bundles are `snap.component` and `snap.vst3`.
`SnapTests --screenshot editor.png` (or `--screenshot-browser`) renders the editor to a PNG; `SnapTests --bench`
prints a CPU benchmark.

Source layout: `Source/Parameters.*` (the parameter table), `Source/PluginProcessor.*`,
`Source/presets/StateIO.*` (state and migration), `Source/presets/PresetStore.*` (preset files and browser data),
`Source/chain/` (module order), `Source/ui/` (the editor: Style, Widgets, Visualizers, PresetBrowser, Editor),
`Source/dsp/common|modules|master/` (DSP and its tunable constants).

## Install in Logic Pro

1. Download `snap-macos-universal` from the CI run and unzip `Snap-AU-macos.zip`.
2. Delete any old `Snap.component` from `~/Library/Audio/Plug-Ins/Components/` (0.1.x used a capitalised
   bundle name), then copy `snap.component` there.
3. Remove the download quarantine:
   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/snap.component
   ```
4. Restart Logic. snap is under **Audio FX > daveydidit > snap**. If it is missing, open
   Logic > Settings > Plug-in Manager and choose **Reset & Rescan Selection**.

For the VST3, copy `snap.vst3` to `~/Library/Audio/Plug-Ins/VST3/` and run the same `xattr` command on it.

## Frozen identifiers

Never change these (the company name `daveydidit` and the lowercase product name are display-only).
Hosts find the plug-in and its saved settings through them.

| What | Value |
| --- | --- |
| AU type | `aufx` (Audio FX, no MIDI) |
| Manufacturer code | `Dwks` |
| Plug-in code | `Snap` |
| Bundle ID | `com.dripworks.snap` |
| Parameter IDs | `attack`, `sustain` (version hint 1); every other ID in `Source/Parameters.cpp` (version hint 2) |

`auval -v aufx Snap Dwks` validates the AU. The 0.1.x IDs `mix` and `output` are retired and must never be reused.

## State versioning

The state is the parameter tree as XML: `<SNAP stateVersion="2" order="shift,space,press,drive">` with one
`<PARAM id value/>` per parameter. The module order lives in the state, not in a parameter.

- Never rename, remove or reuse a parameter ID, and never change a range in a way that moves old values.
- New parameters may be added. Old sessions don't contain them, so they load at their default.
- Bump `stateVersion` only when the meaning of stored data changes, and keep loading every older version.
- The v1 fixture in `Tests/SnapTests.cpp` must keep loading unchanged.

**Version 1** (0.1.x: `attack`, `sustain`, `mix`, `output`) is migrated on load: Attack and Sustain are scaled
by the old Mix (it was the shaper's dry/wet), Output becomes the master Output, and every module is switched
off. A neutral old session is an exact pass-through; a shaped one lands on the 0.2 shaper's nearest settings
(the 0.2 transient shaper is a new design, so the shaping is close to, not identical with, 0.1.x). Host automation of
the retired `mix` and `output` IDs is not carried over.

Loading ignores unknown entries, clamps out-of-range values, resets missing or unreadable values to
their default, and ignores data that is not one complete snap state. The state is read with a flat,
non-recursive scan rather than JUCE's XML parser, so deep nesting and DTDs cannot hurt it.

## Presets

30 factory presets ship inside the plug-in (`Resources/presets/*.snappreset`, read-only). A preset file
is the same flat `<SNAP stateVersion="2" order="...">` element as the session, plus `name` and `tags`
attributes; parameters it leaves out take their default. A preset holds every parameter and the module
order. It does not hold the session toggles (Bypass, Low Latency, Oversampling), the locks or the
snapshots. Loading a preset keeps locked modules (their values and their slot) and is one undo step.

User presets are plain files, so they can be copied, backed up or synced:

| | Location |
| --- | --- |
| macOS | `~/Library/Application Support/daveydidit/snap/Presets/*.snappreset` |
| Linux | `~/.config/daveydidit/snap/Presets/*.snappreset` |

Favourites and named collections are kept next to them in `library.xml`. A preset can sit in several
collections. Tags come from two fixed lists, Character (Dark, Warm, Punchy, …) and Function (Kick,
Snare, Drum Bus, …), in `Source/presets/PresetStore.h`.
