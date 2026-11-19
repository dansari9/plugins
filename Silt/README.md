# silt 0.2.0

A granular effect by daveydidit. Eight seconds of captured main input feed up to 128 stereo grains. The first instance opens **05 tide**, fully wet. Freeze holds the history while grains continue; captured audio is not saved, so after a reopened project or a re-prepare it refills 4 s of input before holding. The cloud shows buffer time, pitch, actual grain footprints and age. Every control is visible in one 905×836 window that scales from 50–200% at a fixed aspect and saves its size.

Build with CMake 3.22+, C++17 and JUCE 8.0.15:

```sh
cmake -S Silt -B Silt/build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build Silt/build --config Release --target Silt_AU Silt_VST3 Silt_Standalone SiltTests
```

For Logic Pro, copy `Silt/build/Silt_artefacts/Release/AU/silt.component` to `~/Library/Audio/Plug-Ins/Components/`, restart Logic and validate it in Plug-in Manager. The CI artifact `silt-macos-universal` is only ad-hoc signed: after unzipping it there, run `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/silt.component` first. The AU (`aumf`) appears under **MIDI-controlled Effects → daveydidit → silt**. Route audio to the main input; the optional Side Chain detects transients in **sidechain** trigger mode. Transient and sidechain onsets fire above a −50 dBFS gate. VST3 and Standalone are also built.

**sync** reuses size travel for 1/64 through two bars. Density follows tempo on whole subdivisions per quarter; position follows a ratio of clock spacing referenced to 120 BPM. In sync, density reads in grains per beat and position in beats. The clock locks to host PPQ while playing and free-runs at host BPM (120 without one) while stopped. **shift** changes pitch independently of grain duration and timeline speed; **tape** couples pitch and duration. Scatter distributes readers around position. Alternating streams have a slight size difference and random pan. Overlap gain compensation and filtered, diffused, sub-unity feedback protect levels without a limiter.

The four **macros** are always-visible faders. Motion adds scan, jitter, pan spread and backward spray bias; texture changes window/skew, spray slope and size spread; bloom increases feedback, diffusion, reverb size and width (0.8–1.0); chaos adds chromatically snapped pitch randomisation, reverse probability, spray and jitter.

0.2 appends 20 parameters after the frozen 18: scan, lock (free / overlap / tone), heads and harmony, window shape and tilt, reverse, detune and scale, drop, repeat, band / centre / spectra colour, pan or binaural space, width, feedback, a one-knob filter, spray direction and the full onset. Every default reproduces 0.1.0's sound, and ten more factory presets (11–20) use them.

MIDI note-on fires a grain at its sample offset in every trigger mode. Held notes transpose grains relative to note 60 (**c3**; the midi dot lights while one is held); last-note priority resumes the previous note and velocity when released. Velocity scales level. All-notes-off and all-sound-off clear the shared note stack.

| CC | Smoothed additive macro offset |
| --- | --- |
| 1 | motion, 0–100 percentage points |
| 2 | texture, 0–100 percentage points |
| 3 | bloom, 0–100 percentage points |
| 4 | chaos, 0–100 percentage points |

Offsets clamp at 100%, smooth over 30 ms and never write automation or saved parameters. CC121 clears them; prepare clears notes and offsets.

Oversampling only affects the feedback saturation: it selects preallocated FIR paths there and crossfades changes inside that loop. Grains are not oversampled. Pitch-up grains use Hermite reads from capture levels filtered by 127-tap Kaiser halfbands, with three semitones of headroom before stepping down. Reported latency is always **0 samples**; mix zero is bit-exact dry at every quality, regardless of output trim.

Previous/next includes factory and saved user presets. **save** writes `user nn.xml` to `~/Library/Audio/Presets/daveydidit/silt` on macOS (`~/.config/daveydidit/silt/Presets` on Linux). **randomize** preserves mix, output, trigger, quality and macros and leaves the previous sound in the other A/B slot. State v2 saves all 38 parameters, preset identity, both A/B slots and the editor size; v1 sessions load and sound as they did in 0.1.0; captured audio and MIDI offsets are not embedded. The frozen contract is tested in `Tests/SiltTests.cpp`.

`Source/SiltReverb.h` is self-contained and marked **SILT_REVERB: removable**. Remove its setup/process calls and file to remove the FDN; retain `reverbOn` and `reverb` as inert frozen parameters. Bloom's feedback, diffusion and stereo functions remain useful.

On Linux use Ninja and run `xvfb-run -a Silt/build/SiltTests`. `--core` runs DSP/state regressions; `--screenshot`, `--screenshot-freeze` and `--screenshot-v6` (`path.png [scale]`) render tide after 4.8 s of pad (freeze: then held for 1 s) at 1× (905×836) or 2× (default) pixels; `--screenshot-v6` matches the design mockup's size and illustrated meters. `--loudness`, `--bench` and `--renders /absolute/folder` provide measurements and audio exports. Goldens require the recorded compiler/math/renderer fingerprint and skip on GitHub Actions. Geist Mono is included under the SIL Open Font License in `Assets/Fonts/OFL.txt`.
