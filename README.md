# Plugins

Audio effect plugins. Each plugin is a separate directory with its own CMake project, so another effect can be added later without sharing a build.

| Plugin | Directory | Formats |
| --- | --- | --- |
| Spring Tank | [SpringReverb](SpringReverb/) | AU, VST3, Standalone, AAX when the Avid SDK is present |
| gulp | [Gulp](Gulp/) | AU, VST3, Standalone |

## Spring Tank

Vintage surf-amp spring reverb. Build and install instructions, including Logic Pro, `auval`, and the impulse-response WAV, are in [SpringReverb/README.md](SpringReverb/README.md).

```bash
cd SpringReverb
```

Run the commands in that README from inside `SpringReverb/`.

## gulp

Sidechain pumping and ducking by daveydidit: tempo, MIDI or sidechain triggered, 12 curves, band split, colour modes, zero latency. See [Gulp/README.md](Gulp/README.md); the commands there run from the repository root.

## Continuous integration

The Actions workflow is checked in at [ci/github-actions-ci.yml](ci/github-actions-ci.yml), not under `.github/workflows`, because the token used to push this repository cannot create workflow files.

To turn it on, copy that file to `.github/workflows/ci.yml`. In the GitHub web editor, add a file at that path and paste the contents. It builds Spring Tank on Linux (VST3 and Standalone) and macOS (AU and VST3), and gulp on Linux (tests, ASan + UBSan, pluginval) and macOS (universal AU and VST3, tests, auval).
