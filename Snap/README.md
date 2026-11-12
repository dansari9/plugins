# Snap

A tiny drum transient shaper by daveydidit. Version 0.1.1. Zero latency, mono or stereo.

Snap follows the level of the drum with a fast and a slow envelope. While a hit starts, the fast
envelope is above the slow one, and **Attack** turns that part up or down. While the hit rings out,
the slow envelope is above the fast one, and **Sustain** turns that part up or down. Both channels
share one gain, so the stereo image stays put.

The display shows the last 2.5 s: the input in grey, the output in orange, and the gain Snap applied
after Mix in blue (centre line = 0 dB, edges = ±18 dB).

## Controls

| Control | Range | Default |
| --- | --- | --- |
| Attack | -100 .. +100 % | 0 % |
| Sustain | -100 .. +100 % | 0 % |
| Mix | 0 .. 100 % | 100 % |
| Output | -24 .. +12 dB | 0 dB |

Drag an encoder up or down. Double-click it to reset. Click the value under it to type a number.
With every control at its default, Snap passes the audio through unchanged.

## Build

```bash
cmake -S Snap -B Snap/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Snap/build
xvfb-run -a Snap/build/SnapTests        # Linux; run it directly on macOS
```

On macOS, use `-G Xcode` and build `Snap_AU` and `Snap_VST3`.
`SnapTests --screenshot editor.png` renders the editor to a PNG.

## Install in Logic Pro

1. Download `snap-macos-universal` from the CI run and unzip `Snap-AU-macos.zip`.
2. Copy `Snap.component` to `~/Library/Audio/Plug-Ins/Components/`.
3. Remove the download quarantine:
   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Snap.component
   ```
4. Restart Logic. Snap is under **Audio FX > daveydidit > Snap**. If it is missing, open
   Logic > Settings > Plug-in Manager and choose **Reset & Rescan Selection**.

Updating from 0.1.0: the brand changed from Dripworks to daveydidit, so after **Reset & Rescan Selection**
Logic lists Snap under **Audio Units > daveydidit** instead of Dripworks. Existing sessions still load,
because the manufacturer code, plug-in code, bundle ID and parameter IDs are unchanged.

For the VST3, copy `Snap.vst3` to `~/Library/Audio/Plug-Ins/VST3/` and run the same `xattr` command on it.

## Frozen identifiers

Never change these (the company name `daveydidit` is display-only and not one of them). Hosts find the plug-in and its saved settings through them.

| What | Value |
| --- | --- |
| AU type | `aufx` (Audio FX, no MIDI) |
| Manufacturer code | `Dwks` |
| Plug-in code | `Snap` |
| Bundle ID | `com.dripworks.snap` |
| Product name | `Snap` |
| Parameter IDs | `attack`, `sustain`, `mix`, `output` (version hint 1) |

`auval -v aufx Snap Dwks` validates the AU.

## State versioning

The state is the parameter tree as XML (`<SNAP stateVersion="1">`). Rules for future versions:

- Never rename or remove a parameter ID, and never change a range in a way that moves old values.
- New parameters may be added. Old sessions don't contain them, so they load at their default.
- Bump `stateVersion` only when the meaning of stored data changes, and keep loading every older version.
- The v1 fixture in `Tests/SnapTests.cpp` must keep loading unchanged.

Loading ignores unknown entries, clamps out-of-range values, resets missing or unreadable values to
their default, and ignores data that is not a Snap state. Blobs with more than 256 tags or a DTD are
rejected before parsing (JUCE's XML parser recurses per level), so a state must stay under 256 tags.
