# silt 0.1.0

A granular effect by daveydidit. An eight-second capture becomes a cloud of up to 128 grains: slow pads, scattered harmonics, small loops and worn textures. Starts fully wet with **soft sediment**. Mix at zero passes the original samples exactly; reported latency is zero. Turn output down when deliberately boosting the trim.

Build with CMake 3.22+, a C++17 compiler and JUCE 8.0.15 (fetched automatically):

```sh
cmake -S Silt -B Silt/build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build Silt/build --config Release --target Silt_AU Silt_VST3 SiltTests
```

For Logic Pro, copy `Silt/build/Silt_artefacts/Release/AU/silt.component` to `~/Library/Audio/Plug-Ins/Components/`, then restart Logic and validate it in Plug-in Manager. It is a MIDI-controlled effect (`aumf`): create a software instrument track, select **MIDI-controlled Effects → daveydidit → silt**, choose the audio track in its Side Chain menu, and set capture to **side** when that is the source to granulate. When the host supplies main-bus audio, capture input uses it; the sidechain can independently trigger grains or supply their audio. AU, VST3 and Standalone only; no AAX or Windows target.

MIDI note-on starts a burst at its sample offset. Held notes transpose new grains relative to MIDI note 60 (C3); the last held note wins, and releasing it resumes the previous one. Velocity controls grain level. All-notes-off clears held notes. Channels share one note stack. The fixed CC map adds smoothed offsets without changing the saved or automated parameters:

| CC | Offset |
| --- | --- |
| 1 | spray, up to +1500 ms |
| 2 | density, up to ×4 |
| 64 | hold while the smoothed value is above halfway |
| 71 | pitch random, up to +12 st |
| 74 | grain size, ×0.25 to ×4 (centre neutral) |
| 91 | reverb mix, up to +100 percentage points |

CC121 resets these offsets. A new prepare clears notes and CC offsets. In **pitch** mode, two smoothly crossfaded resampling taps keep the grain's duration and timeline speed; **speed** mode couples pitch and duration. Pitch snapping follows random pitch. Scan moves the source position independently. Grid **note** maps size and position to note lengths up to two bars (long notes loop smoothly within the captured history); **lock** maps size to the density spacing and position to multiples of the clock period. Clock sync follows host PPQ while playing, and the host BPM (120 without one) while stopped. Density subdivides that clock; jitter delays individual grains within one spacing.

The **macro** word reveals blur, drift, bloom and glint. All default to zero. **rnd** makes a constrained sound and stores the previous one in the other A/B slot; mix, output and macros stay put. Previous/next includes ten factory presets and local user presets; save writes `user nn.xml` to `~/Library/daveydidit/silt/Presets` on macOS (the platform application-data directory elsewhere). Projects save parameters, preset identity, A/B and editor size. Live captured audio and performance CCs are not embedded in the project.

`SiltReverb.h` is self-contained and marked **SILT_REVERB: removable**. Delete that class and the marked handling/call to remove the reverb; leave `verbSize` and `verbMix` in the parameter list as inert entries for session compatibility. Feedback has a DC block, tone filter, diffusion and soft saturation inside its sub-unity loop. Overlap is gain-compensated; there is no limiter.

On Linux, build with Ninja and run `xvfb-run -a Silt/build/SiltTests`. `--screenshot path.png` renders the default cloud at 2× pixels; `--loudness` prints the preset measurement table. Pixel/audio goldens run only on a matching environment fingerprint and are skipped on GitHub Actions. Geist Mono is included under its SIL Open Font License in `Assets/Fonts/OFL.txt`.
