#!/usr/bin/env python3
"""Render the mono PCM loops in Resources/noise."""

import argparse
import math
from pathlib import Path
import random
import struct
import wave


RATE = 48_000
OVERLAP = round(0.14 * RATE)
WARMUP = round(0.08 * RATE)
TARGET_RMS = 0.125
PEAK_LIMIT = 0.77
OUTPUT_DIR = Path(__file__).resolve().parents[1] / 'Resources' / 'noise'


def biquad(samples, kind, frequency, q=math.sqrt(0.5)):
    angle = math.tau * frequency / RATE
    cosine = math.cos(angle)
    alpha = math.sin(angle) / (2 * q)
    if kind == 'low':
        b0 = (1 - cosine) / 2
        b1, b2 = 2 * b0, b0
    elif kind == 'high':
        b0 = (1 + cosine) / 2
        b1, b2 = -2 * b0, b0
    else:
        b0, b1, b2 = alpha, 0, -alpha

    a0 = 1 + alpha
    b0, b1, b2 = b0 / a0, b1 / a0, b2 / a0
    a1, a2 = -2 * cosine / a0, (1 - alpha) / a0
    z1 = z2 = 0.0
    output = []
    for sample in samples:
        value = b0 * sample + z1
        z1 = b1 * sample - a1 * value + z2
        z2 = b2 * sample - a2 * value
        output.append(value)
    return output


def noise(rng, frames, low, high):
    white = (rng.uniform(-1, 1) for _ in range(frames))
    return biquad(biquad(white, 'high', low), 'low', high)


def envelope(rng, frames, rate, floor):
    """Interpolate random levels at a fixed rate with smooth joins."""
    step = round(RATE / rate)
    levels = [rng.uniform(floor, 1.0) for _ in range(frames // step + 2)]
    values = []
    for i in range(frames):
        k, j = divmod(i, step)
        x = j / step
        values.append(levels[k] + (levels[k + 1] - levels[k]) * x * x * (3 - 2 * x))
    return values


def arrivals(rng, frames, rate):
    """Place events with exponentially distributed interarrival times."""
    position = rng.expovariate(rate)
    while (start := int(position * RATE)) < frames:
        yield start
        position += rng.expovariate(rate)


def grain(samples, rng, start, duration, low, high, gain, attack, decay):
    length = max(2, round(duration * RATE))
    burst = noise(rng, min(length, len(samples) - start), low, high)
    for i, value in enumerate(burst):
        t = i / (length - 1)
        if t < attack:
            window = math.sin(math.pi / 2 * t / attack) ** 2
        else:
            window = math.cos(math.pi / 2 * (t - attack) / (1 - attack)) ** decay
        samples[start + i] += gain * window * value


def ring(samples, rng, start, modes, gain, tuning=1.0, attack=0.002):
    """Add damped modes described by frequency, relative gain and decay time."""
    for frequency, level, decay in modes:
        length = min(round(decay * RATE * 7), len(samples) - start)
        phase = rng.uniform(0, math.tau)
        phase_step = math.tau * frequency * tuning / RATE
        damping = math.exp(-1 / (decay * RATE))
        amplitude = level * gain
        for i in range(length):
            onset = min(1.0, i / (attack * RATE))
            samples[start + i] += amplitude * onset * onset * math.sin(phase + i * phase_step)
            amplitude *= damping


def fizz(rng, frames):
    air = noise(rng, frames, 2800, 7400)
    motion = envelope(rng, frames, 31, 0.65)
    samples = [0.33 * x * e for x, e in zip(air, motion)]
    for start in arrivals(rng, frames, 175):
        center = rng.uniform(2900, 6150)
        grain(samples, rng, start, rng.uniform(0.003, 0.014),
              center * 0.75, center * 1.12, rng.uniform(0.18, 0.65), 0.18, 3)
    return samples


def hush(rng, frames):
    low = noise(rng, frames, 145, 1150)
    soft = noise(rng, frames, 600, 2650)
    motion = envelope(rng, frames, 3.6, 0.77)
    return [(0.93 * a + 0.34 * b) * e for a, b, e in zip(low, soft, motion)]


def drizzle(rng, frames):
    samples = [0.22 * x for x in noise(rng, frames, 480, 4700)]
    modes = ((920, 0.35, 0.017), (1660, 0.19, 0.011), (2555, 0.11, 0.005))
    for start in arrivals(rng, frames, 43):
        strength = rng.uniform(0.15, 0.86)
        tuning = rng.uniform(0.62, 1.5)
        grain(samples, rng, start, rng.uniform(0.009, 0.031),
              900 * tuning, 4450, strength, 0.13, 4)
        ring(samples, rng, start, modes, strength, tuning)
    return samples


def sizzle(rng, frames):
    bed = noise(rng, frames, 1250, 5400)
    motion = envelope(rng, frames, 7, 0.38)
    samples = [0.57 * x * e for x, e in zip(bed, motion)]
    for start in arrivals(rng, frames, 81):
        center = rng.uniform(1250, 3550)
        grain(samples, rng, start, rng.uniform(0.008, 0.042),
              center, 7300, rng.uniform(0.32, 1.32), 0.06, 5)
    return samples


def grit(rng, frames):
    samples = [0.25 * x for x in noise(rng, frames, 185, 1450)]
    for start in arrivals(rng, frames, 53):
        center = rng.uniform(320, 1170)
        gain = rng.uniform(0.35, 1.2)
        grain(samples, rng, start, rng.uniform(0.017, 0.054),
              center, center * 2.6, gain, 0.08, 4)
        grain(samples, rng, start, rng.uniform(0.004, 0.012),
              1400, 3800, 0.2 * gain, 0.12, 4)
    return samples


def rustle(rng, frames):
    body = noise(rng, frames, 540, 2500)
    edge = noise(rng, frames, 1900, 5500)
    fold = envelope(rng, frames, 12, 0.04)
    sweep = envelope(rng, frames, 4.2, 0.31)
    samples = [(a + 0.46 * b * f) * f * e for a, b, f, e in zip(body, edge, fold, sweep)]
    for start in arrivals(rng, frames, 24):
        grain(samples, rng, start, rng.uniform(0.025, 0.090),
              1200, rng.uniform(2500, 5900), rng.uniform(0.18, 0.65), 0.26, 2)
    return samples


def crinkle(rng, frames):
    samples = [0.19 * x for x in noise(rng, frames, 980, 4450)]
    for start in arrivals(rng, frames, 12):
        size = rng.uniform(0.45, 1.25)
        for _ in range(rng.randint(4, 10)):
            offset = start + round(rng.uniform(0, 0.059) * RATE)
            if offset < frames:
                grain(samples, rng, offset, rng.uniform(0.004, 0.022),
                      rng.uniform(960, 3200), rng.uniform(4400, 7400),
                      size * rng.uniform(0.36, 1), 0.07, 5)
    return samples


def spray(rng, frames):
    air = noise(rng, frames, 1530, 7170)
    body = noise(rng, frames, 560, 2950)
    jets = envelope(rng, frames, 6.4, 0.13)
    return [(a * e + 0.64 * b) * e for a, b, e in zip(air, body, jets)]


def steam(rng, frames):
    air = noise(rng, frames, 750, 5250)
    pressure = envelope(rng, frames, 2.6, 0.68)
    whistle = biquad(air, 'band', 2750, 4.8)
    shimmer = biquad(air, 'band', 4700, 6.0)
    return [(0.38 * a + 0.84 * w * e + 0.31 * s * e) * e
            for a, w, s, e in zip(air, whistle, shimmer, pressure)]


def clatter(rng, frames):
    samples = [0.21 * x for x in noise(rng, frames, 285, 1800)]
    modes = ((280, 0.52, 0.034), (510, 0.41, 0.026), (853, 0.32, 0.030),
             (1390, 0.17, 0.021), (2210, 0.14, 0.012))
    for start in arrivals(rng, frames, 19):
        size = rng.uniform(0.29, 1.0)
        tuning = rng.uniform(0.73, 1.42)
        ring(samples, rng, start, modes, size, tuning, attack=0.0008)
        grain(samples, rng, start, 0.019, 420, 5050, 0.68 * size, 0.07, 5)
    return samples


def jangle(rng, frames):
    samples = [0.12 * x for x in noise(rng, frames, 950, 4620)]
    modes = ((725, 0.28, 0.083), (1275, 0.29, 0.064), (1866, 0.32, 0.049),
             (3230, 0.24, 0.042), (4790, 0.13, 0.024))
    for start in arrivals(rng, frames, 22):
        size = rng.uniform(0.25, 0.85)
        tuning = rng.choice((0.89, 1.0, 1.18)) * rng.uniform(0.97, 1.03)
        ring(samples, rng, start, modes, size, tuning, attack=0.001)
        grain(samples, rng, start, 0.012, 2200, 6500, 0.37 * size, 0.08, 5)
    return samples


def scrape(rng, frames):
    body = noise(rng, frames, 280, 2950)
    rasp = noise(rng, frames, 1100, 5300)
    force = envelope(rng, frames, 5.3, 0.10)
    chatter = envelope(rng, frames, 73, 0.23)
    resonant = biquad(body, 'band', 1180, 3.4)
    return [(0.55 * a + 0.88 * b * c + 0.45 * r) * e * e
            for a, b, c, r, e in zip(body, rasp, chatter, resonant, force)]


def breath(rng, frames):
    chest = noise(rng, frames, 110, 860)
    air = noise(rng, frames, 630, 3600)
    turbulence = envelope(rng, frames, 19, 0.61)
    samples = []
    for i, (c, a, e) in enumerate(zip(chest, air, turbulence)):
        inhale = 0.5 + 0.5 * math.cos(math.tau * (1.75 * i / RATE + 0.25))
        samples.append((0.17 + 0.83 * inhale) * (0.97 * c * e + 0.62 * a * inhale))
    return samples


def buzz(rng, frames):
    body = noise(rng, frames, 185, 1780)
    drive = envelope(rng, frames, 13, 0.58)
    samples = []
    for i, (x, e) in enumerate(zip(body, drive)):
        phase = math.tau * 138 * i / RATE + 0.12 * math.sin(math.tau * 6.7 * i / RATE)
        tone = (math.sin(phase) + 0.64 * math.sin(2 * phase + 0.3)
                + 0.27 * math.sin(5 * phase) + 0.13 * math.sin(9 * phase))
        samples.append((0.21 * tone + 0.54 * x) * e)
    return samples


def flutter(rng, frames):
    air = noise(rng, frames, 330, 3050)
    edge = noise(rng, frames, 1550, 4300)
    speed = envelope(rng, frames, 5.7, 0.13)
    lift = envelope(rng, frames, 7.1, 0.29)
    phase = 0.0
    samples = []
    for a, b, flutter_rate, amplitude in zip(air, edge, speed, lift):
        phase += math.tau * (14 + 28 * flutter_rate) / RATE
        wing = (0.5 + 0.5 * math.sin(phase)) ** 3
        samples.append(amplitude * ((0.15 + 0.85 * wing) * a + 0.46 * wing * b))
    return samples


def sand(rng, frames):
    samples = [0.12 * x for x in noise(rng, frames, 630, 3250)]
    flow = envelope(rng, frames, 8.7, 0.26)
    for start in arrivals(rng, frames, 290):
        center = rng.uniform(630, 2600)
        grain(samples, rng, start, rng.uniform(0.004, 0.016),
              center, center * 2.1, flow[start] * rng.uniform(0.21, 0.82), 0.32, 2)
    return samples


def chime(rng, frames):
    air = noise(rng, frames, 1000, 4500)
    samples = [0.15 * x for x in air]
    modes = ((1060, 0.48, 0.145), (1594, 0.27, 0.125), (2230, 0.19, 0.091),
             (3122, 0.11, 0.070), (4318, 0.07, 0.040))
    start = rng.randrange(round(0.045 * RATE), round(0.11 * RATE))
    for tuning in (1.0, 1.27, 0.81, 1.0, 1.15, 0.81, 1.27, 1.0):
        if start >= frames:
            break
        ring(samples, rng, start, modes, rng.uniform(0.55, 0.93), tuning, attack=0.013)
        start += round(rng.uniform(0.19, 0.28) * RATE)
    return samples


def swarm(rng, frames):
    samples = [0.35 * x for x in noise(rng, frames, 465, 4450)]
    for frequency in (168, 219, 315, 471, 623):
        drift = envelope(rng, frames, rng.uniform(3.2, 7.5), 0.0)
        distance = envelope(rng, frames, rng.uniform(4.4, 9.1), 0.08)
        phase = rng.uniform(0, math.tau)
        for i, (d, a) in enumerate(zip(drift, distance)):
            phase += math.tau * frequency * (0.82 + 0.37 * d) / RATE
            tone = math.sin(phase) + 0.48 * math.sin(2 * phase) + 0.22 * math.sin(3 * phase + 0.9)
            samples[i] += 0.17 * a * a * tone
    return samples


# Order matches the host's noise selector.
TEXTURES = (
    (fizz, 1.41), (hush, 1.74), (drizzle, 1.82), (sizzle, 1.57),
    (grit, 1.48), (rustle, 1.79), (crinkle, 1.63), (spray, 1.51),
    (steam, 1.85), (clatter, 1.70), (jangle, 1.89), (scrape, 1.59),
    (breath, 1.71), (buzz, 1.44), (flutter, 1.61), (sand, 1.54),
    (chime, 1.94), (swarm, 1.91),
)


def loop_overlap(samples, frames):
    """Fade the continuation of the take into its opening at the wrap."""
    loop = samples[:frames]
    for i in range(OVERLAP):
        angle = math.pi / 2 * i / (OVERLAP - 1)
        loop[i] = math.cos(angle) * samples[frames + i] + math.sin(angle) * samples[i]
    return loop


def center(samples):
    mean = math.fsum(samples) / len(samples)
    return [x - mean for x in samples]


def normalize(samples):
    samples = center(samples)
    rms = math.sqrt(math.fsum(x * x for x in samples) / len(samples))
    gain = TARGET_RMS / rms
    knee = 0.56
    span = PEAK_LIMIT - knee
    leveled = []
    for x in samples:
        x *= gain
        if abs(x) > knee:
            x = math.copysign(knee + span * math.tanh((abs(x) - knee) / span), x)
        leveled.append(x)
    leveled = center(leveled)
    trim = min(1.0, PEAK_LIMIT / max(abs(x) for x in leveled))
    return [round(x * trim * 32767) for x in leveled]


def render(recipe, seconds, seed):
    frames = round(seconds * RATE)
    take = recipe(random.Random(seed), WARMUP + frames + OVERLAP)
    # Fourth-order Butterworth low-pass before pitch-shifted playback.
    for q in (0.541196100146197, 1.306562964876377):
        take = biquad(take, 'low', 7300, q)
    pcm = normalize(loop_overlap(take[WARMUP:], frames))
    return struct.pack(f'<{frames}h', *pcm)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=OUTPUT_DIR)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for index, (recipe, seconds) in enumerate(TEXTURES):
        pcm = render(recipe, seconds, 20260713 + 1009 * index)
        with wave.open(str(args.output_dir / f'{recipe.__name__}.wav'), 'wb') as file:
            file.setparams((1, 2, RATE, len(pcm) // 2, 'NONE', 'not compressed'))
            file.writeframes(pcm)


if __name__ == '__main__':
    main()
