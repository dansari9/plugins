# silt 0.1.0

A granular effect by daveydidit. Eight seconds of captured audio feed up to 128 stereo grains. The first instance opens **05 tide**, fully wet, in the full-width cloud editor. The cloud shows four seconds of buffer time, pitch, actual grain footprints and their age. Freeze holds the input history while grains continue. The editor scales from 50–200%, saves its size, and repaints only changed cells.

Build with CMake 3.22+, C++17 and JUCE 8.0.15:

```sh
cmake -S Silt -B Silt/build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build Silt/build --config Release --target Silt_AU Silt_VST3 SiltTests
```

For Logic Pro, copy `Silt/build/Silt_artefacts/Release/AU/silt.component` to `~/Library/Audio/Plug-Ins/Components/`, restart Logic and validate it in Plug-in Manager. The AU is a MIDI-controlled effect (`aumf`), listed under **MIDI-controlled Effects → daveydidit → silt**. Route the source to the effect's main audio input; the optional Side Chain input detects transients when trigger is **sidechain**. Capture always reads the main input. VST3 and Standalone are also built; no AAX or Windows target.

The following order, IDs, ranges, defaults and choice indices are frozen as **state v1**. The size parameter stores milliseconds in free mode and uses the same knob travel for the named sync divisions, from 1/64 through two bars. Its initial stored value selects 1/16. Density is grains/second at 120 BPM; in sync mode it follows the host tempo on the nearest whole subdivision per quarter note. Position uses the displayed time at 120 BPM to choose a quarter-grain spacing ratio in sync mode, then follows tempo with that ratio. With transport stopped, the clock continues at host BPM, or 120 without a playhead.

| ID | Range / choices, in index order | Default |
| --- | --- | --- |
| `mix` | 0–100 % | 100 |
| `output` | −24–12 dB | 0 |
| `size` | 0.5–4000 ms / sync division | 51.78 (1/16) |
| `sizeSync` | sync, free | sync |
| `density` | 1–64 /s | 24 |
| `position` | 0–4000 ms | 1200 |
| `scatter` | 0–100 % | 45 |
| `pitch` | −48–48 st | +7 |
| `pitchMode` | shift, tape | shift |
| `trigger` | clock, transient, sidechain | clock |
| `reverbOn` | off, on | on |
| `reverb` | 0–100 % | 30 |
| `oversample` | off, 2x, 4x, 8x | 2x |
| `freeze` | off, on | off |
| `motion` | 0–100 % | 0 |
| `texture` | 0–100 % | 0 |
| `bloom` | 0–100 % | 0 |
| `chaos` | 0–100 % | 0 |

**shift** changes pitch independently of grain duration and timeline speed; **tape** couples pitch and duration. Scatter distributes readers around position. Alternating left/right streams have a small size difference. Actual overlap is gain compensated, with filtered, diffused, sub-unity feedback and saturation inside that loop. There is no limiter.

The **macros** toggle replaces the preset row with four dotted faders. Zero adds no character beyond the visible grain settings: motion adds scan, timing jitter and pan spread; texture changes window/skew, spray slope and size spread; bloom increases feedback, diffusion, reverb size and width; chaos adds chromatically snapped pitch randomisation, reverse probability, spray and timing jitter. All ten factory sounds use visible parameters only: sub dust, grit, glass, halo, tide, choir, tines, chime, rain, drift.

MIDI note-on triggers a grain at its sample offset in every trigger mode. Held notes transpose subsequent grains relative to MIDI note 60 (**c3**); last-note priority resumes the previous held note when released. Velocity scales grain level. The readout retains the last note received. All-notes-off and all-sound-off clear held notes. Channels share a note stack.

| CC | Smoothed additive macro offset |
| --- | --- |
| 1 | motion, 0–100 percentage points |
| 2 | texture, 0–100 percentage points |
| 3 | bloom, 0–100 percentage points |
| 4 | chaos, 0–100 percentage points |

Offsets clamp at 100%, smooth over 30 ms, and never write automation or saved parameters. CC121 clears them. Prepare clears notes and performance offsets.

Oversampling uses preallocated linear-phase FIR paths on the wet signal and feedback saturation; pitch-up readers also use band-limited capture levels and Hermite interpolation. Reported latency is **0 / 49 / 61 / 65 samples** for off / 2x / 4x / 8x at 44.1, 48 and 96 kHz. Dry is delayed by exactly that amount; mix zero passes those original samples bit for bit, regardless of output trim. Changing quality updates host latency and crossfades the processing paths.

Previous/next includes factory sounds and saved user presets. **save** writes `user nn.xml` to `~/Library/daveydidit/silt/Presets` on macOS (the platform application-data directory elsewhere). **randomize** preserves mix, output, quality and macros and stores the previous sound in the other A/B slot. Projects save every parameter, preset identity, both A/B slots, editor size and macro visibility. Captured audio and MIDI performance offsets are not embedded.

`Source/SiltReverb.h` is self-contained and marked **SILT_REVERB: removable**. Delete its setup/reset/process calls and the file to remove the FDN; retain `reverbOn` and `reverb` as inert frozen parameters. Bloom's feedback, diffusion and stereo functions remain useful without it.

On Linux, build with Ninja and run `xvfb-run -a Silt/build/SiltTests`. `--screenshot path.png` and `--screenshot-macros path.png` render tide after one second of pad input at 1024×410, with 2× pixels. `--loudness` prints preset measurements. Pixel/audio goldens run on their recorded compiler/math/renderer fingerprint and are skipped on GitHub Actions. Geist Mono is included under the SIL Open Font License in `Assets/Fonts/OFL.txt`.
