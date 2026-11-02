# Plugins

Audio effect plugins. Each plugin is a separate directory with its own CMake project, so another effect can be added later without sharing a build.

| Plugin | Directory | Formats |
| --- | --- | --- |
| Spring Tank | [SpringReverb](SpringReverb/) | AU, VST3, Standalone, AAX when the Avid SDK is present |

## Spring Tank

Vintage surf-amp spring reverb. Build and install instructions, including Logic Pro, `auval`, and the impulse-response WAV, are in [SpringReverb/README.md](SpringReverb/README.md).

```bash
cd SpringReverb
```

Run the commands in that README from inside `SpringReverb/`.

## Continuous integration

The Actions workflow is checked in at [ci/github-actions-ci.yml](ci/github-actions-ci.yml), not under `.github/workflows`, because the token used to push this repository cannot create workflow files.

Replace `.github/workflows/ci.yml` with that file (the web editor can do it; this token cannot push workflow changes). The proposed workflow builds Spring Tank on Linux (VST3 and Standalone) and macOS (AU and VST3), runs the impulse-response check and a headless editor check, uploads `artifacts/spring_tank_editor.png`, and on macOS runs `auval -v aufx SpTk Dwks`.
