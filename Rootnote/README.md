# Rootnote 0.3.0

Dripworks note-based resonant drum-tuning EQ. Cut unwanted resonances, boost a
note and its harmonic, octave or membrane series, and add a transient-excited
Ring tail. This is an EQ/resonator, not a pitch shifter. Zero reported latency;
mono, stereo and mono-to-stereo processing. JUCE 8.0.15, C++17.

The editor is deliberately minimal: a scrollable control for every parameter,
note/Hz/hit readouts, Match cut, Tune to key, factory presets, A/B, spectrum,
band positions, output peaks and limiter reduction. The tuner and display feed
run only while an editor is open. The [UI feed contract](Source/UIFeed.md)
documents how to replace the editor without changing the processor.

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

For a downloaded unsigned macOS AU build you trust, remove its quarantine:

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Rootnote.component
```

For VST3 use the corresponding `~/Library/Audio/Plug-Ins/VST3/Rootnote.vst3`
path. The Standalone runs directly from its build folder and needs audio/MIDI
devices selected in its settings.

## Logic Pro: MIDI-controlled AU (`aumf`)

1. Put the drum audio on an audio track and create a **Software Instrument** track.
2. In that track's **Instrument** slot choose **AU MIDI-controlled Effects >
   Dripworks > Rootnote** (Stereo). Rootnote is an `aumf`, not a MIDI FX insert.
3. In Logic's plugin-window **Side Chain** chooser, select the drum track or a
   bus fed by it. That audio is Rootnote's input; hear the processed output on
   the software instrument track. Route the source's dry output to No Output
   if you do not want an additional dry copy.
4. Record/sequence MIDI on the instrument track, or select/arm it for a keyboard.
   In Rootnote's **Ch 1+2** mode, channel 1 controls Boost and channel 2 Cut.
   Preserve event channels by leaving the track's MIDI channel at All.
   **Omni > Boost** accepts any channel for Boost.

This follows the instrument-slot/side-chain routing described in
[Native Instruments' Logic MIDI-effect guide](https://support.native-instruments.com/support/solutions/articles/69000879791-how-to-set-up-a-midi-controlled-effect-in-logic-pro-x).
Rootnote's AU identity remains `aumf RtNt Dwks`; on macOS validate with
`auval -v aumf RtNt Dwks`.

## Ableton Live

Use the **VST3** on the drum audio track. Create a separate MIDI track with no
instrument, show I/O, set its **MIDI To** upper chooser to the drum track and
the lower chooser to **Rootnote**. Play a MIDI clip, or arm the MIDI track with
Monitor Auto / use Monitor In for a keyboard. Keep drum audio playing: MIDI
selects resonances but does not generate a hit. Use Omni > Boost if Live's
routing does not preserve the incoming MIDI channel. Use manual Cut Note or
Match cut when a separate channel-2 destination is unavailable.
See [Live's routing manual](https://www.ableton.com/en/manual/routing-and-i-o/).

## Tuning and presets

Play a few isolated hits with the editor open. **Match cut** places Cut on the
detected frequency and enables it. **Tune to key** puts Boost on the nearest
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
Preset names are retained after edits. **A/B** stores the settings and preset
name of the slot you leave; B initially copies A. Bypass is shared. Only the
active settings are saved in the host session; restoring starts a fresh A/B
comparison at A. Very high Q can ring for over a minute; the host tail report
covers this rather than truncating it to the Ring decay setting.

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
560 × 760 size (1× PNG). The optional second path receives an identical copy:

```sh
xvfb-run -a Rootnote/build/RootnotePluginTests --snapshot Rootnote/artifacts/rootnote_editor.png /workspace/rootnote-logs/editor.png
```

The checked-in image is [artifacts/rootnote_editor.png](artifacts/rootnote_editor.png).

## Validation scope

Linux core/plugin tests and VST3 pluginval are automated. The tests include
all factory preset values and state restoration, A/B, headless editor lifetime
and resizing; [comparison rules and review regressions](Tests/README.md) are
documented separately. AU validation, universal binary checks and listening/
MIDI routing in Logic and Live still require those hosts on macOS/Windows.
This release keeps the minimal editor; it does not implement the visual design
proposed in the original M4 plan.
