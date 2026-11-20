# snap

A one-screen drum multi-FX by daveydidit. Version 0.2.0 (in development). Mono or stereo.

The 0.2 chain is six modules plus a master section, with one **Magnitude** macro that scales the
module amounts and the master's continuous controls toward neutral:

```
in -> input -> sub/hiss -> [shift | space | press | drive, any order] -> push -> master -> output
```

Status of this build: the full parameter set exists, and the master **Attack** / **Sustain**
transient shaper, **Input**, **Output**, **Magnitude** and **Bypass** work. The modules and the
other master stages are not wired yet, so their controls do nothing. The editor is an interim one
(Attack, Sustain, Magnitude, Output and the gain display); the full screen comes later.

The shaper follows the level of the drum with a fast and a slow envelope. While a hit starts, the
fast envelope is above the slow one, and **Attack** turns that part up or down. While the hit rings
out, the slow envelope is above the fast one, and **Sustain** turns that part up or down. Both
channels share one gain, so the stereo image stays put.

With every control at its default, snap passes the audio through unchanged.

## Build

```bash
cmake -S Snap -B Snap/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Snap/build
xvfb-run -a Snap/build/SnapTests        # Linux; run it directly on macOS
```

On macOS, use `-G Xcode` and build `Snap_AU` and `Snap_VST3`. The bundles are `snap.component` and `snap.vst3`.
`SnapTests --screenshot editor.png` renders the editor to a PNG.

Source layout: `Source/Parameters.*` (the parameter table), `Source/PluginProcessor.*`,
`Source/presets/StateIO.*` (state and migration), `Source/chain/` (module order),
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
off, so an old session sounds as it did.

Loading ignores unknown entries, clamps out-of-range values, resets missing or unreadable values to
their default, and ignores data that is not one complete snap state. The state is read with a flat,
non-recursive scan rather than JUCE's XML parser, so deep nesting and DTDs cannot hurt it.
