# Spring Tank

Vintage surf-amp spring reverb, in the `SpringReverb/` directory of the Plugins repository. One C++ codebase builds an Audio Unit for Logic Pro, a VST3, a standalone app, and an AAX target when the Avid SDK is present.

Run the commands below from this directory (`cd SpringReverb` if you cloned the repository).

The tank is a real-time take on the dispersive spring models in Abel, Berners, Costello & Smith (AES 121, 2006) and Välimäki, Parker & Abel, *Parametric Spring Reverberation Effect*, J. Audio Eng. Soc. 58.7/8 (2010). Three springs run in parallel. Each one is a pair of feedback delays. A short cascade of stretched allpasses — a spectral delay filter — sits in the loop, and a longer cascade on the bright pickup turns the attack into a falling chirp. Negative allpass coefficients hold the lows back, which is the drip. Tension sets how strong that chirp is. Tone is the damping. Decay is the RT60.

JUCE 8.0.15 is fetched at configure time. It is dual-licensed (AGPLv3 / commercial). Builds without a commercial licence show the JUCE splash screen.

## Parameters

All seven are automatable `AudioProcessorValueTreeState` parameters and are stored in the host session.

| Knob | Range | What it does |
| --- | --- | --- |
| Decay | 0.15–5.5 s | How long the tank rings (RT60) |
| Tension | loose → tight | Chirp length. Loose is a slow boing, tight is a ping |
| Tone | dark → bright | Per-trip damping and the transducer peak |
| Drip | 0–100 | Level of the bright, falling attack chirp |
| Pre-delay | 0–100 ms | Silence before the springs speak |
| Mix | 0–100% | Equal-power dry/wet |
| Output | −24 to +12 dB | Level after the mix |

Mono in/out and stereo in/out are both supported. Parameter moves are smoothed. The processing block does not allocate, and denormals are flushed.

Double-click a knob to return it to the surf default.

## Build on macOS (AU and VST3)

You need Xcode (for the Audio Unit SDK) and CMake 3.22 or newer. The first configure downloads JUCE.

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Release --target SpringTank_AU SpringTank_VST3 SpringTank_Standalone
```

Products:

```text
build/SpringTank_artefacts/Release/AU/Spring Tank.component
build/SpringTank_artefacts/Release/VST3/Spring Tank.vst3
build/SpringTank_artefacts/Release/Standalone/Spring Tank.app
```

A universal binary (Apple silicon and Intel), which Logic on either machine can load:

```bash
cmake -S . -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target SpringTank_AU SpringTank_VST3
```

Ninja works too, if you only want a native release build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target SpringTank_AU SpringTank_VST3
```

With the Ninja (single-config) generator the `Release` directory is still in the path, because JUCE always appends the configuration name.

### Install

```bash
cp -R "build/SpringTank_artefacts/Release/AU/Spring Tank.component" \
      ~/Library/Audio/Plug-Ins/Components/

cp -R "build/SpringTank_artefacts/Release/VST3/Spring Tank.vst3" \
      ~/Library/Audio/Plug-Ins/VST3/
```

Create the destination folders if they are not there yet. After copying, clear quarantine and ad-hoc sign, or Logic and `auval` will refuse the bundle:

```bash
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/Spring\ Tank.component
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Spring\ Tank.vst3

codesign --force --deep --sign - ~/Library/Audio/Plug-Ins/Components/Spring\ Tank.component
codesign --force --deep --sign - ~/Library/Audio/Plug-Ins/VST3/Spring\ Tank.vst3
```

Pass `-DSPRINGTANK_COPY_AFTER_BUILD=ON` at configure time if you would rather have the build copy the plug-ins into those user folders itself.

Rescan in Logic Pro (Plug-in Manager) if the tank does not appear. The Audio Unit type is an effect (`aufx`).

### Validate with auval

```bash
killall -9 AudioComponentRegistrar 2>/dev/null || true
auval -v aufx SpTk Dwks
```

`SpTk` is the subtype and `Dwks` is the manufacturer code (Dripworks). `auval -a` lists everything the registrar can see if the component was copied somewhere unexpected.

A clean run ends with `AU VALIDATION SUCCEEDED`.

## AAX

AAX is wired up and left off, because the Avid SDK cannot be redistributed and AAX only builds on macOS and Windows. Point CMake at a local SDK (the directory that contains `Interfaces/ACF`):

```bash
cmake -S . -B build -G Xcode \
  -DSPRINGTANK_BUILD_AAX=ON \
  -DSPRINGTANK_AAX_SDK_PATH=/path/to/aax-sdk
cmake --build build --config Release --target SpringTank_AAX
```

The category is Reverb and the identifier is `com.dripworks.springtank`. Pro Tools also requires PACE signing, which this project does not run.

## Linux (VST3, Standalone, impulse response)

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libasound2-dev libfreetype6-dev libfontconfig1-dev \
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev \
  libgtk-3-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

```text
build/SpringTank_artefacts/Release/VST3/Spring Tank.vst3
build/SpringTank_artefacts/Release/Standalone/Spring Tank
build/SpringTankIRTest
```

There is no AU target on Linux.

## Impulse response

`SpringTankIRTest` runs the tank offline (no audio device), checks that the tail decays, that mono and stereo at 44.1 kHz and 192 kHz stay finite, and that fully dry mix is bit-transparent. Given a path, it writes a 48 kHz, 24-bit stereo WAV of a fully wet surf setting, normalised to about −1 dBFS so it can be auditioned:

```bash
./build/SpringTankIRTest artifacts/spring_tank_impulse.wav
```

Listen for a bright falling chirp on the attack, then a longer, softer boing. It should not sound like a room.

A rendered copy is checked in at `artifacts/spring_tank_impulse.wav`.

## Continuous integration

The workflow source is [ci/github-actions-ci.yml](../ci/github-actions-ci.yml) at the repository root. Copy it to `.github/workflows/ci.yml` to enable Actions. It builds this directory: VST3 and Standalone on Linux, AU and VST3 on macOS, runs the impulse-response check, and uploads the plug-ins plus the WAV.
