#!/usr/bin/env python3
"""Synthesize the stereo space responses as deterministic 48 kHz PCM."""
from array import array
from math import ceil, exp, pi, sin, sqrt
from pathlib import Path
from random import Random
import re
import sys
import wave

RATE = 48_000
SNAP = Path(__file__).resolve().parents[1]
NAMES = SNAP / "Source/dsp/modules/SpaceConstants.h"

# Delay line scale, stereo spread, high frequency retention, and reflection spacing.
ROOMS = [
    (.38, .27, .48, .003), (.46, .42, .30, .005), (.61, .60, .46, .006),
    (.72, .75, .68, .009), (.93, .83, .79, .012), (.81, .58, .24, .016),
    (1.33, .95, .60, .022), (1.63, .93, .72, .027), (1.49, .61, .53, .033),
    (.91, .74, .70, .020), (1.17, .90, .56, .030), (1.86, .92, .66, .046),
    (.54, .77, .86, .003), (.69, .70, .71, .007), (1.07, .93, .59, .013),
    (1.23, .96, .38, .019), (.87, .91, .66, .010), (.77, .94, .57, .010),
    (.65, .91, .90, .009), (1.51, .85, .18, .014),
]

DELAYS = (389, 487, 593, 719, 823, 947, 1069, 1201)
WET_L2 = .43


def allpass(signal, delay, gain):
    ring = array("d", [0]) * delay
    for i, sample in enumerate(signal):
        old = ring[i % delay]
        ring[i % delay] = value = sample + gain * old
        signal[i] = old - gain * value


def schroeder(signal, delays, brightness, rng):
    for delay in delays:
        line = array("d", [0]) * delay
        for _ in range(17):
            line[rng.randrange(delay)] = rng.uniform(-.8, .8)
        tone = 0.
        for i in range(len(signal)):
            old = line[i % delay]
            line[i % delay] = .9995 * old
            tone += .22 * (old - tone)
            signal[i] += brightness * old + (1 - brightness) * tone


def render(name, rt60, lead, profile, seed, duration):
    scale, spread, brightness, spacing = profile
    rng = Random(seed)
    count = ceil(RATE * duration)
    left, right = array("d", [0]) * count, array("d", [0]) * count
    lines = [array("d", [0]) * (round(scale * delay) | 1) for delay in DELAYS]
    index, tone, previous = [0] * 8, [0.] * 8, [0.] * 8
    signs = (1, -1, -1, 1, -1, 1, 1, -1)
    for d, line in enumerate(lines):
        # Feed a short, scattered burst into each line. An orthogonal feedback
        # matrix maintains the late field for the decay control to shape.
        for _ in range(12):
            line[rng.randrange(len(line))] = rng.uniform(-.58, .58)
        index[d] = rng.randrange(len(line))

    for i in range(count):
        a, b, c, d, e, f, g, h = (line[j] for line, j in zip(lines, index))
        x0, x1, x2, x3 = a + b + c + d, a - b + c - d, a + b - c - d, a - b - c + d
        x4, x5, x6, x7 = e + f + g + h, e - f + g - h, e + f - g - h, e - f - g + h
        feedback = (x0+x4, x1+x5, x2+x6, x3+x7, x0-x4, x1-x5, x2-x6, x3-x7)
        mid, side = 0., 0.
        for j, sample in enumerate((a, b, c, d, e, f, g, h)):
            lines[j][index[j]] = 0.9999 / sqrt(8) * feedback[j]
            index[j] += 1
            if index[j] == len(lines[j]):
                index[j] = 0
            tone[j] += (.11 + .26 * brightness) * (sample - tone[j])
            output = brightness * sample + (1 - brightness) * tone[j]
            if name in ("sheet", "shimmer"):
                output = .60 * output + (.90 if name == "shimmer" else .40) * (output - previous[j])
            previous[j] = sample
            mid += output
            side += signs[j] * output
        # A growing diffuse field sits behind the discrete reflections.
        buildup = min(1., i / (RATE * spacing * 3))
        left[i], right[i] = buildup * (mid + spread * side), buildup * (mid - spread * side)

    if name == "smear":
        left, right = array("d", [0]) * count, array("d", [0]) * count
        schroeder(left, (1459, 1627, 1777, 1951, 2081, 2333), brightness, rng)
        schroeder(right, (1481, 1609, 1823, 1979, 2141, 2381), brightness, rng)
        allpass(left, 211, .67)
        allpass(right, 197, .67)
        for i in range(count):
            buildup = min(1., i / (RATE * spacing * 3))
            left[i] *= buildup
            right[i] *= buildup
    if name == "sheet":
        for delay in (89, 223, 389):
            allpass(left, delay, .62)
            allpass(right, delay + 18, .62)

    if name not in ("rewind", "unfurl"):
        for j in range(12):
            at = round(RATE * spacing * (1 + j * .76 + rng.uniform(-.2, .2)))
            other = at + round(RATE * spread * rng.uniform(-.0015, .0015))
            hit = rng.choice((-1., 1.)) * exp(-j / 4.2) * .9
            left[at] += hit
            right[other] += hit * (1 - spread * rng.uniform(0., .5))

    if name == "coil":
        modes = (263, 399, 524, 711, 947, 1210, 1597, 2163, 2879, 3911)
        for j, hz in enumerate(modes):
            onset = round(RATE * (.015 + j * .0027))
            gain = rng.choice((-1., 1.)) * .22 / (1 + j * .20)
            for i in range(onset, count):
                t = (i - onset) / RATE
                phase = 2 * pi * (hz * t + 15 * sin(2 * pi * 4.3 * t) * t)
                env = gain * min(1., t / .004) * exp(-t * (2.5 + .25 * j))
                left[i] += env * sin(phase)
                right[i] += env * sin(phase + .7 * min(1., t / .02) * (-1)**j)

    energy = 0.
    for i in range(count):
        t = i / RATE
        shape = 1.
        if name == "gate":
            shape = max(0., min(1., (.265 - t) / .020))
            # A flat body up to a short closing gate.
            shape *= exp(6.907755 * min(t, .265) / rt60)
        elif name == "rewind":
            shape = (t / duration)**2.7 * min(1., (duration - t) / .009)
        elif name == "unfurl":
            shape = min(1., t / lead)**2
        shape *= min(1., (count - i - 1) / (RATE * .01))
        left[i] *= shape
        right[i] *= shape
        weight = exp(-6.907755 * max(0., t - lead) / rt60)
        energy += (left[i]**2 + right[i]**2) * weight**2

    gain = WET_L2 / sqrt(energy / 2)
    return zip(left, right), gain


def main():
    specs = re.findall(r'\{ "([a-z]+)", (\d+\.\d+)(?:, (\d+\.\d+))? \}', NAMES.read_text())
    assert len(specs) == len(ROOMS)
    selected = set(sys.argv[1:])
    if selected.difference(name for name, _, _ in specs):
        raise SystemExit("Unknown space type")
    for i, ((name, decay, onset), profile) in enumerate(zip(specs, ROOMS)):
        if selected and name not in selected:
            continue
        rt60, lead = float(decay), float(onset or 0)
        duration = lead + max(.50, rt60 * 2.15)
        if name == "gate":
            duration = .265
        elif name == "rewind":
            duration = .43
        response, gain = render(name, rt60, lead, profile, 2917 + 157 * i, duration)
        pcm = bytearray()
        for frame in response:
            for x in frame:
                value = round(x * gain * (2**23 - 1))
                pcm.extend(value.to_bytes(3, "little", signed=True))
        with wave.open(str(SNAP / "Resources/irs" / f"{name}.wav"), "wb") as wav:
            wav.setparams((2, 3, RATE, 0, "NONE", "not compressed"))
            wav.writeframes(pcm)


if __name__ == "__main__":
    main()
