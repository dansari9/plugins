# Plugins

Audio effect plugins. Each plugin is a separate directory with its own CMake project, so another effect can be added later without sharing a build.

| Plugin | Directory | Formats |
| --- | --- | --- |
| silt | [Silt](Silt/) | AU, VST3, Standalone |
| Spring Tank | [SpringReverb](SpringReverb/) | AU, VST3, Standalone, AAX when the Avid SDK is present |

## silt

A granular effect by daveydidit: live capture, clouds, freeze, MIDI and sidechain grains. Logic Pro AU install and the fixed MIDI map are in [Silt/README.md](Silt/README.md). Version 0.1.0.

## Spring Tank

Vintage surf-amp spring reverb. Build and install instructions, including Logic Pro, `auval`, and the impulse-response WAV, are in [SpringReverb/README.md](SpringReverb/README.md).

```bash
cd SpringReverb
```

Run the commands in that README from inside `SpringReverb/`.

## Continuous integration

The identical workflows in [.github/workflows/ci.yml](.github/workflows/ci.yml) and [ci/github-actions-ci.yml](ci/github-actions-ci.yml) build Spring Tank and silt. Silt adds unit tests, Linux ASan/UBSan and pluginval, plus universal macOS AU/VST3 builds, signing checks and auval. The macOS artifact is `silt-macos-universal`.
