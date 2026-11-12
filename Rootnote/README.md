# Rootnote 0.4.3

Dripworks note-based resonant drum-tuning EQ. Cut unwanted resonances, boost a
note and its harmonic, octave or membrane series, and add a transient-excited
Ring tail. This is an EQ/resonator, not a pitch shifter. Zero reported latency;
mono, stereo and mono-to-stereo processing. JUCE 8.0.15, C++17.

The editor is the **Faceplate**: a light hardware-style panel with one dark
screen (boost note, hit and status lines, output spectrum with white boost and
orange cut pillars under the blue Focus window, a legend of the page's values),
twelve pitch keys, five pages and up to eight colour-coded encoders.

- **Pitch keys:** click sets the boost note, Alt-click sets the cut note and
  switches Cut on. `oct −`/`oct +` move the boost note (Alt: the cut note) an
  octave down/up at once, from the note that sounds (a held MIDI note too);
  the number beside them is the boost note's octave, which the pitch keys play
  in. The bar under a key shows whether it is in the selected key/scale.
- **Pages** (boost, cut, ring, out, midi) give the encoders their roles:
  Boost: Gain, Q, Bands, Focus, Range, Blend, Tilt, Fine. Cut: Depth, Q,
  Overtones, Bands, Fine (cents of the cut note), Cut on/off; the cut note
  itself is set with the keys, Match cut or by typing it on the screen.
  Ring: Amount, Decay, Drop. Out: Mix, Output, Glide, A4, Q mode. MIDI: Mode,
  Note-off, Key, Scale. Hold a page key to peek at its values on the screen;
  a click selects it.
- **Encoders:** drag up/down, Shift for fine control, double-click to reset.
  With the keyboard focus (click or Tab), the arrow keys step it, Shift finer.
- **Typing values:** click any value in the screen's bottom row (or Alt-, Cmd-
  or right-click an encoder, or press Return on it) to type over it. Units and
  case are optional: `-3.1`, `-3.1db`, `q8`, `50%`, `C#1`, `db1`, `55 Hz`,
  `-12ct`, `on`, `min`. Return or clicking elsewhere applies it as one host
  gesture, Escape cancels, and text that is no value changes nothing. Host
  text entry reads the same formats.
- **Focus window:** the blue curve is the weighting Focus and Range give the
  boost bands (centre dashed, ±Range marked under the axis); "link" means it
  follows the boost note. Turning or typing Focus switches the link off. The
  legend shows LINK, ALL (Bands of the octave series) or N/A (Blend outside
  the harmonic series) where a value has no effect.
- **Match cut** is the cut's switch: lit while Cut is on; a click switches it
  off, or on at the detected hit. Without a hit it (like **tune to key**)
  blinks and changes nothing. **Key lock** snaps MIDI notes to the key.
- **Series keys** harm/oct/drum, **link** (Focus follows the note), **limit**.
- **Header:** preset (click the name for the list, arrows to step), A/B,
  settings (UI size, Q mode, note-off) and the orange ON switch (bypass).

The editor keeps its aspect ratio; hosts resize it, and Settings offers 75-200%.
The tuner and display feed run only while an editor is open. The
[UI feed contract](Source/UIFeed.md) documents the processor's editor interface.
The font is Geist Mono (SIL OFL 1.1, `Assets/Fonts`), subset by `make_fonts.py`,
which also draws the ♯ glyph Geist Mono lacks.

## Build and test

Run from the repository root. Requires CMake 3.22+, Ninja, a C++17 compiler and
network access on the first configure to fetch the pinned JUCE release.
Linux development packages include ALSA, X11/Xext/Xinerama/Xrandr/Xcursor,
FreeType, fontconfig and OpenGL; install Xvfb for headless GUI tests.
macOS needs Xcode command-line tools; Windows needs the MSVC build environment.

```sh
cmake -S Rootnote -B Rootnote/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Rootnote/build
Rootnote/build/RootnoteCoreTests
xvfb-run -a Rootnote/build/RootnotePluginTests
xvfb-run -a /path/to/pluginval --strictness-level 10 --validate-in-process --validate Rootnote/build/Rootnote_artefacts/Release/VST3/Rootnote.vst3
```

On macOS/Windows run the plugin tests directly, without `xvfb-run` (Windows
executables have `.exe`). `-DROOTNOTE_BUILD_PLUGIN=OFF` builds the JUCE-free
core/tests only. macOS defaults to universal arm64+x86_64 and a 10.13 deployment
target. Build products are under `Rootnote/build/Rootnote_artefacts/Release/`:
VST3 and Standalone on all platforms, plus AU on macOS. Copying into plugin
folders is opt-in with `-DROOTNOTE_COPY_AFTER_BUILD=ON`.

## Install

Close your DAW, copy the complete bundle, then rescan plugins:

| Platform / format | User install location |
| --- | --- |
| macOS AU | `~/Library/Audio/Plug-Ins/Components/Rootnote.component` |
| macOS VST3 | `~/Library/Audio/Plug-Ins/VST3/Rootnote.vst3` |
| Linux VST3 | `~/.vst3/Rootnote.vst3` |
| Windows VST3 | `%LOCALAPPDATA%\Programs\Common\VST3\Rootnote.vst3` (or the host's configured VST3 folder) |

CI builds are ad-hoc signed and signature-verified, but are not notarized.
For a downloaded build you trust, remove quarantine from the installed bundle:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Rootnote.component
# If you installed VST3 too:
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Rootnote.vst3
```

In **Logic Pro > Settings > Plug-in Manager**, select **Rootnote > Reset &
Rescan Selection**, then restart Logic. Ad-hoc signing alone does not remove
the downloaded-file quarantine. See [AU cache and rescanning](#au-cache-and-rescanning)
if an earlier failed scan is still cached.

The Standalone runs directly from its build folder and needs audio/MIDI
devices selected in its settings.

## Logic Pro: Audio FX (`aufx`)

On the drum track, choose **Audio FX > Audio Units > Dripworks > Rootnote**,
then Mono, Mono → Stereo or Stereo as appropriate. This also works after
Drummer / Drum Kit Designer on a software-instrument channel strip. Open the
insert to show the editor; if Logic shows its generic controls, select
**Editor** in the plug-in window's View menu.

The AU is now a plain Audio FX so normal drum inserts use the track's audio
without MIDI-controlled-effect instrument/side-chain routing. Optional MIDI
control remains available in VST3 and Standalone and defaults to **Off**; use
the note controls in Logic. Saved sessions retain their stored MIDI mode.

The AU identity is `aufx RtNt Dwks`; validate with `auval -v aufx RtNt Dwks`.
The 0.4.0 and 0.4.1 test builds were a MIDI-controlled AU (`aumf RtNt Dwks`).
Logic sessions made with those builds do not find 0.4.2 or later: insert
Rootnote again as an Audio FX and reapply the settings. From 0.4.2 on the AU
identity `aufx RtNt Dwks` is frozen, so sessions find every later release.

## Ableton Live

Use the **VST3** on the drum audio track and enable **Ch 1+2** in Rootnote's MIDI
page for MIDI control (the default is **Off**). Create a separate MIDI track with no
instrument, show I/O, set its **MIDI To** upper chooser to the drum track and
the lower chooser to **Rootnote**. Play a MIDI clip, or arm the MIDI track with
Monitor Auto / use Monitor In for a keyboard. Keep drum audio playing: MIDI
selects resonances but does not generate a hit. Use Omni > Boost if Live's
routing does not preserve the incoming MIDI channel. Use manual Cut Note or
Match cut when a separate channel-2 destination is unavailable.
Ch 1+2 controls Boost on channel 1 and Cut on channel 2; Omni > Boost accepts
any channel. Set MIDI Mode to Off when MIDI control is unwanted.
See [Live's routing manual](https://www.ableton.com/en/manual/routing-and-i-o/).

## Tuning and presets

Play a few isolated hits with the editor open. **Match cut** places Cut on the
detected frequency and enables it; clicked again it switches Cut off. **Tune to key** puts Boost on the nearest
degree of the selected key/scale and clears Fine. These actions are explicit;
presets never silently replace a note using a detector reading. Rootnote uses
C4 = MIDI 60; hosts may label octaves differently. MIDI note-off defaults to
Hold; Return goes back to the knob after the last held key is released.

The eight factory presets use the exact settings below. Unlisted sound values
reset to parameter defaults. Notes, Fine, Key, Scale, Key Lock, A4 and Bypass
stay with the song. **Default** is an additional reset entry with the same
preservation rule.

| Preset | Settings differing from defaults (plus defining settings) |
| --- | --- |
| Sub Afterglow | Harmonic; 4 bands; Q 80; Boost +12 dB; Ring 40%; Decay 900 ms; Drop 2 st; Limiter on |
| Pocket Kick | Harmonic; 6 bands; Q 10; Boost +6 dB; Range 1 oct |
| Snare Core | Drum; 6 bands; Q 18; Boost +7 dB; Cut on, −10 dB |
| Tom Shift | Drum; 8 bands; Q 25; Boost +8 dB; Cut on (default −12 dB) |
| Octave Weight | Octaves; Q 14; Boost +8 dB; Tilt −6 dB/oct |
| Resonance Eraser | Boost 0 dB; Cut on, −18 dB; 12 cut bands; Cut Overtones 80% |
| Played Percussion | MIDI Ch 1+2; Glide 40 ms; Harmonic; 12 bands; Q 60; Ring 30% |
| Noise Lattice | Harmonic; 24 bands; Q 120; Boost +18 dB; Mix 70% |

For Snare Core/Tom Shift, use Match cut to remove the detected original note.
The presets are also the host's programs, for host preset browsers.
Preset names are retained after edits. **A/B** stores the settings and preset
name of the slot you leave; B initially copies A. Bypass is shared. The host
session keeps both slots and which one is active. Very high Q can ring for over a minute; the host tail report
covers this rather than truncating it to the Ring decay setting.

## Saved state and frozen identifiers

A session restores every setting exactly: all parameters (bit for bit), the
preset, both A/B slots and the active one, and the editor's size and page
(an open editor switches to them at once and drops a value being typed).
Saving, restoring and switching A/B are each one transaction, whichever
threads the host and editor use. The state carries a schema version
(`RootnoteProcessor::kStateSchema`, 2 since 0.4.3); every state from 0.4.0 on
loads with its values (for AU sessions, see the identity note above), missing
values take their defaults, out-of-range ones are clamped, unknown fields are
ignored, a value that is not exactly one number counts as missing, and a
truncated or damaged state changes nothing. A newer schema is read the same
way, which the freeze rule below keeps safe. Selecting a program in the host,
the current one included, loads that preset; restoring a session does not
select a program, so it keeps the edits.

**Freeze rule.** These identify Rootnote to hosts and saved sessions and must
never change: the AU type/codes `aufx RtNt Dwks`, the VST3 class ID (from the
`RtNt`/`Dwks` codes with `JUCE_VST3_CAN_REPLACE_VST2=0`), the bundle ID
`com.dripworks.rootnote`, and every parameter's ID string, version hint (1),
position, range and default. A new parameter is appended with a new ID; a
retired one stays in place. Any state change bumps `kStateSchema` and keeps
reading the older ones, with a fixture in `Tests/fixtures`. The plugin test
"identifiers are frozen" pins all of this, so CI fails on any change.

## AU cache and rescanning

After replacing an AU, restart Logic. In **Settings > Plug-In Manager** select
Rootnote and use **Reset & Rescan Selection**. If it is absent, use **Full Audio
Unit Reset**, then restart Logic. To clear the cache manually, quit audio apps
and move the cache files aside:

```sh
mkdir -p ~/Desktop/Rootnote-AU-cache-backup
mv ~/Library/Caches/AudioUnitCache/*.cache ~/Desktop/Rootnote-AU-cache-backup/
```

Restart macOS and reopen Logic to rebuild the cache. This rescans all installed
AUs, as described in [Apple's troubleshooting guide](https://support.apple.com/en-ca/122179).

## Headless editor snapshot

Render a demo kick, detected Cut and MIDI-controlled Boost at the default
960 × 540 logical size (2× PNG). The optional second path receives an identical
copy; `--snapshot-entry` renders the cut page with a value being typed:

```sh
xvfb-run -a Rootnote/build/RootnotePluginTests --snapshot Rootnote/artifacts/rootnote_editor.png /workspace/rootnote-logs/editor.png
```

The checked-in image is [artifacts/rootnote_editor.png](artifacts/rootnote_editor.png).

## Validation scope

Linux core/plugin tests and VST3 pluginval are automated. The tests include
all factory preset values and state restoration, A/B, headless editor lifetime
and resizing; [comparison rules and review regressions](Tests/README.md) are
documented separately. macOS CI validates the AU and universal binaries.
Listening in Logic and Live, and MIDI routing in Live (VST3), still require
those hosts on macOS/Windows.
This release keeps the minimal editor; it does not implement the visual design
proposed in the original M4 plan.
