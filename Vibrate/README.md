# vibrate 0.1.0

Stereo movement for background parts. Five lanes add the same signal to the left and subtract it from the right, leaving the mono sum dry. Accepts mono or stereo input with stereo output. Reports zero latency.

| Dial | Adds |
| --- | --- |
| pitch | Two taps with independent slow pitch wander and faster flutter |
| pan | Short, shallow changes in left/right level |
| delay | Two taps whose time difference drifts slowly |
| phase | The difference between two four-stage allpass sweeps |
| side | Eight fixed taps with a slow change in level |
| mix | Linear gain on the sum of the five lanes |

The display shows measured audio-thread activity. Turning off a lane clears its trace. The preset name opens the grouped preset list. The arrows step through factory and user presets. Use A and B to compare settings. Drag, scroll or use the arrow keys on a dial. Double-click to reset it, or select the value to type a percentage.

## Build and check

```sh
cmake -S Vibrate -B Vibrate/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Vibrate/build --target Vibrate_VST3 Vibrate_Standalone VibrateTests
xvfb-run -a Vibrate/build/VibrateTests
Vibrate/build/VibrateTests --bench
xvfb-run -a Vibrate/build/VibrateTests --bench-editor
```

Set `FETCHCONTENT_SOURCE_DIR_JUCE` to use a local JUCE 8.0.15 checkout. On macOS, CMake defaults to universal binaries and also builds the AU as `Vibrate_AU`.

`VibrateTests --render in.f32 out.f32 preset=0 rate_hz=48000` renders interleaved stereo float32 in 128-frame blocks. `--screenshot out.png 1440 1 in.f32 1920 0` captures the real editor after 1920 blocks. Use `-` instead of the input file to show the stopped editor and add `menu` to open the preset list.

## State

APVTS XML `VIBRATE`, state version 1. Factory programs are indexed 0–15. User presets are XML files in `daveydidit/vibrate/Presets` under the JUCE user application data directory.

Plugin identifiers are `com.dripworks.vibrate`, AU `aufx/Vbrt/Dwks`, and VST3 category `Fx Modulation`. Parameter IDs are `pitch`, `pan`, `delay`, `phase`, `side`, `mix` and host `bypass`.
