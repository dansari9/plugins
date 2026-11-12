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

The Actions workflow lives in [.github/workflows/ci.yml](.github/workflows/ci.yml).

It builds Spring Tank and Rootnote on Linux and macOS, runs Rootnote's tests and VST3 pluginval, and signs and validates Rootnote's universal macOS bundles.
