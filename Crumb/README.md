# Crumb

A vintage-sampler bit crusher by daveydidit. Version 0.1.0 (milestone 1: DSP, parameters and state; the editor
is a generic placeholder). Zero latency, mono or stereo, both channels share the settings.

Signal path: preamp (0 … +48 dB) into a soft ADC clip (tanh shoulder above 0.5, antiderivative antialiasing) →
filter (pre) → approx (anti-alias) lowpass → sample & hold on a jittered virtual clock → dither and quantizer →
fixed vintage DAC (ladder errors on the top bits, coupling-cap DC removal) → images lowpass → filter (post) → mix → output.

The hold steps are bandlimited with a minimum-phase BLEP, and the virtual ADC reads the input through the same
causal kernel, so the only aliasing is the emulated fold-down at the virtual rate. No oversampling, no lookahead.
When the rate is below the host rate the wet path has about 7 host samples of group delay plus half a hold period.
At rate = host rate (no jitter) the sample & hold is the identity.

## Controls

| ID | Control | Range | Default |
| --- | --- | --- | --- |
| `preamp` | Preamp | 0 … 48 dB | 0 dB |
| `bits` | Resolution | 0 (OFF), 1 … 24 bit | 12 |
| `dcShift` | DC Shift | off = mid-riser, on = mid-tread | off |
| `dither` | Dither | 0 … 100 % of ½ LSB | 0 % |
| `rate` | Rate | 44 Hz … 44.1 kHz (log) | 26.04 kHz |
| `jitter` | Jitter | 0 … 100 % (up to ±3 % of the rate) | 0 % |
| `approxOn` | Approx filter | on/off | on |
| `approxDev` | Approx deviation | −100 … +100 % = cutoff Nyquist(rate) · 2^(±1 oct) | 0 % |
| `imagesOn` | Images filter | on/off | off |
| `imagesShift` | Images shift | 0 … 100 % = cutoff Nyquist(rate) · 2^(0 … 2 oct) | 0 % |
| `fType` | Filter type | OFF, LP, BP, HP, BR | OFF |
| `fOrder` | Filter order | Pre, Post | Post |
| `cutoff` | Cutoff | 20 Hz … 20 kHz (log) | 20 kHz |
| `reso` | Reso / bandwidth | 0 … 100 % (Q 0.5 … 20) | 0 % |
| `mix` | Mix | 0 … 100 % | 100 % |
| `output` | Output | −24 … +12 dB | 0 dB |
| `bypass` | Bypass (host bypass parameter) | on/off | off |

## Build

```bash
cmake -S Crumb -B Crumb/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Crumb/build --target Crumb_VST3 Crumb_Standalone CrumbTests
xvfb-run -a Crumb/build/CrumbTests        # Linux; run it directly on macOS
```

On macOS, use `-G Xcode` and build `Crumb_AU` and `Crumb_VST3`. `auval -v aufx Crmb Dwks` validates the AU.

## Frozen identifiers

| What | Value |
| --- | --- |
| AU type | `aufx` (Audio FX, no MIDI) |
| Manufacturer code | `Dwks` |
| Plug-in code | `Crmb` |
| Bundle ID | `com.dripworks.crumb` |
| Product name | `Crumb` |
| Parameter IDs | the 17 IDs above, in that order (version hint 1) |

## State versioning

The state is the parameter tree as XML (`<CRUMB stateVersion="1">`), read by a non-recursive scanner. Loading
ignores unknown entries, clamps out-of-range values, resets missing or unreadable values to their default, and
leaves the state alone for data that is not a complete Crumb state.

- Never rename, remove or reorder a parameter ID, or change a range so that old values move. Only append.
- Independent L/R (pending): append `link` (default on) and per-channel twins (`rateR`, `bitsR`, …). Old
  sessions lack them and load linked, identical on both channels.
- Bump `stateVersion` only when stored data changes meaning; translate older data in the migration hook in
  `setStateInformation`. The v1 fixture in `Tests/CrumbTests.cpp` must keep loading unchanged.
