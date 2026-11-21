# The riso skin's textures (Resources/riso): the 1440 x 840 ink field the editor samples for every accent, and the six
# 212 x 104 visualizer backdrops. Deterministic; run `python3 Tools/generate_riso.py` from Snap/ to regenerate.
import numpy as np
from PIL import Image

W, H, VW, VH = 1440, 840, 212, 104
INKS = ['#5b7cff', '#2fd3c4', '#a98bff', '#ffc83d', '#ff6a2f', '#8a8a8a']  # sub/hiss shift space press drive push


def hx(h): return np.array([int(h[i:i + 2], 16) for i in (1, 3, 5)], float)


B4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.


def bayer(h, w): return np.tile(B4, (h // 4 + 1, w // 4 + 1))[:h, :w]


def fbm(h, w, octaves, seed):
    r = np.random.default_rng(seed)
    out = np.zeros((h, w))
    for o in range(octaves):
        s = 2 ** o * 3
        g = r.random((s + 1, int(s * w / h) + 2))
        out += np.asarray(Image.fromarray((g * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)) / 255. / 2 ** o
    return (out - out.min()) / (np.ptp(out) + 1e-9)


def field():
    """One ink per 240 px module column, edges warped by fbm and ordered-dithered between neighbours, slight grain."""
    cols = np.stack([hx(c) for c in INKS])
    y, x = np.mgrid[0:H, 0:W].astype(float)
    n = fbm(H, W, 5, 3)
    u = np.clip((x + (n - .5) * 150 + np.sin(y / 90) * 30) / 240.0 - .5, 0, 5.0)
    i0 = np.floor(u).astype(int)
    fr = np.clip((u - i0 - .35) / .3, 0, 1)  # hold an ink across most of a column, blend at the edges
    pick = np.where(fr > bayer(H, W), np.clip(i0 + 1, 0, 5), i0)
    return cols[pick] * (0.88 + 0.12 * n[..., None])


def halftone(img, cell, ang):
    """AM halftone: coverage 0..1 -> dots on a rotated screen."""
    h, w = img.shape
    y, x = np.mgrid[0:h, 0:w].astype(float)
    a = np.deg2rad(ang)
    xr, yr = x * np.cos(a) + y * np.sin(a), -x * np.sin(a) + y * np.cos(a)
    return (np.hypot((xr % cell) - cell / 2, (yr % cell) - cell / 2) / (cell / 1.414) < np.sqrt(img) * 0.95).astype(float)


def diffuse(img):
    """Floyd-Steinberg to two levels."""
    a = img.copy()
    h, w = a.shape
    for yy in range(h):
        for xx in range(w):
            o = a[yy, xx]
            a[yy, xx] = q = np.round(o)
            e = o - q
            if xx + 1 < w: a[yy, xx + 1] += e * 7 / 16
            if yy + 1 < h:
                if xx > 0: a[yy + 1, xx - 1] += e * 3 / 16
                a[yy + 1, xx] += e * 5 / 16
                if xx + 1 < w: a[yy + 1, xx + 1] += e / 16
    return np.clip(a, 0, 1)


def textures(seed=1):
    y, x = np.mgrid[0:VH, 0:VW].astype(float) / np.array([VH, VW])[:, None, None]
    c = [hx(k) for k in INKS[:5]] + [hx('#6a6a6a')]
    # sub/hiss: low aurora bands, ordered dither
    v = np.clip(np.sin((x * 3 + np.sin(y * 5) * .4) * 3.1) * .5 + .5, 0, 1) * (1 - y) ** .6 * .9 + fbm(VH, VW, 4, seed) * .25
    yield (v > bayer(VH, VW) * 1.1)[..., None] * c[0]
    # shift: moire of two ring fields, rotated halftone
    v = (np.sin(np.hypot(x - .2, (y - .5) * .5) * 60) * np.sin(np.hypot(x - .8, (y - .4) * .5) * 55) * .5 + .5) ** 2 * .6
    yield halftone(v, 4, 15)[..., None] * c[1]
    # space: clouds, halftone
    yield halftone(np.clip(fbm(VH, VW, 6, seed + 5) * 1.4 - .35, 0, 1), 3, 45)[..., None] * c[2]
    # press: horizontal line screen
    v = np.clip(1 - np.abs(y - .5) * 1.6, 0, 1) * (.4 + .6 * x)
    yield ((np.mgrid[0:VH, 0:VW][0] % 3) / 3.0 < v * 0.9)[..., None] * c[3]
    # drive: a diagonal streak in three inks, error-diffused
    d = y * .9 - x * .55 + .25
    out = np.zeros((VH, VW, 3))
    for k, (cc, off) in enumerate([(c[4], 0), (hx('#ff4f9a'), .06), (c[0], -.09)]):
        out = np.maximum(out, diffuse(np.exp(-((d + off) / .10) ** 2) * .85 + fbm(VH, VW, 3, seed + k) * .12)[..., None] * cc)
    yield out
    # push: faint neutral dither
    yield (fbm(VH, VW, 4, seed + 9) * .5 > bayer(VH, VW) * 1.6)[..., None] * c[5]


if __name__ == '__main__':
    Image.fromarray(np.clip(field(), 0, 255).astype(np.uint8)).save('Resources/riso/field.png')
    for k, t in enumerate(textures()):
        alpha = (t.max(-1) > 0).astype(np.uint8) * 255
        Image.fromarray(np.dstack([np.clip(t, 0, 255).astype(np.uint8), alpha])).save(f'Resources/riso/viz{k}.png')
