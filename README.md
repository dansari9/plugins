# Plugins

Audio effect plugins. Each plugin is a separate directory with its own CMake project, so another effect can be added later without sharing a build.

| Plugin | Directory | Formats |
| --- | --- | --- |
| silt | [Silt](Silt/) | AU, VST3, Standalone |
| Spring Tank | [SpringReverb](SpringReverb/) | AU, VST3, Standalone, AAX when the Avid SDK is present |
| gulp | [Gulp](Gulp/) | AU, VST3, Standalone |

## silt

A granular effect by daveydidit: live capture, clouds, freeze, MIDI and sidechain grains. Logic Pro AU install and the fixed MIDI map are in [Silt/README.md](Silt/README.md). Version 0.2.0.

## Spring Tank

Vintage surf-amp spring reverb. Build and install instructions, including Logic Pro, `auval`, and the impulse-response WAV, are in [SpringReverb/README.md](SpringReverb/README.md).

```bash
cd SpringReverb
```

Run the commands in that README from inside `SpringReverb/`.

## gulp

Sidechain pumping and ducking by daveydidit: tempo, MIDI or sidechain triggered, 12 curves, band split, colour modes, zero latency. See [Gulp/README.md](Gulp/README.md); the commands there run from the repository root.

## Continuous integration

The identical workflows in [.github/workflows/ci.yml](.github/workflows/ci.yml) and [ci/github-actions-ci.yml](ci/github-actions-ci.yml) build Spring Tank, silt and gulp. Spring Tank builds on Linux (VST3 and Standalone) and macOS (AU and VST3). Silt and gulp each add unit tests, Linux ASan/UBSan and pluginval, plus universal macOS AU/VST3 builds, signing checks and auval. The macOS artifacts are `silt-macos-universal` and `gulp-macos-universal`.
