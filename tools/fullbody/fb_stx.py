#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Full-body select pictures (s_selduel_fb_<name>_v1/_v2.stx) from fb_render.py's renders (both from the user's own
copy of the game; see fb_render.py).

    python fb_stx.py <render dir> <tag> <out .stx> <texture name>
    e.g. python fb_stx.py out\\yoda yoda_v1 s_selduel_fb_yoda_v1.stx S_selDuel_FB_yoda_V1

The body render, graded as the game's own full bodies are (deep blacks, a cool cast, richer colours, a blue rim
light), its blades drawn over it as the game's (a white core, a narrow band of pure colour), and the game's blue halo
around it, at 128x256, as an STX DXT5 (format 0x52, blocks in row order) with the header the game's own full-body
pictures have. The values (GRADE_*, RIM_*, BLADE_*, HALO_*) were measured on the game's own pictures.
"""
import struct
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

GLOW_SIGMA, GLOW_GAIN = 10.0, 2.5  # at the render's 512x1024


def compose(body_png, glow_png, glow=True):
    a = np.array(Image.open(body_png).convert("RGBA")).astype(float) / 255
    g = np.array(Image.open(glow_png).convert("RGBA")).astype(float) / 255
    gp = g[..., :3] * g[..., 3:]
    gain = GLOW_GAIN if glow else 0.0
    col = np.clip(a[..., :3] * a[..., 3:] + gain * np.dstack([gaussian_filter(gp[..., i], GLOW_SIGMA) for i in range(3)]), 0, 1)
    al = np.clip(a[..., 3] + gain * gaussian_filter(g[..., 3], GLOW_SIGMA), 0, 1)
    img = np.dstack([col, al])
    small = np.stack([np.array(Image.fromarray((img[..., i] * 255 + 0.5).astype(np.uint8)).resize((128, 256), Image.BOX))
                      for i in range(4)], -1).astype(float) / 255
    rgb = np.where(small[..., 3:] > 1e-3, small[..., :3] / np.maximum(small[..., 3:], 1e-3), 0)
    return np.clip(np.dstack([rgb, small[..., 3]]) * 255 + 0.5, 0, 255).astype(np.uint8)


# the game's own full bodies' grade (measured on them): deep blacks, bright whites, a cool cast, richer colours, and a
# blue rim light along the silhouette
GRADE_BLACK, GRADE_WHITE = 0.08, 1.0
GRADE_SAT = 1.6
GRADE_TINT = np.array([0.97, 1.0, 1.07])
RIM_RGB, RIM_GAIN, RIM_WIDTH = np.array([0.55, 0.68, 1.0]), 0.35, 2.5


def grade(rgba):
    from scipy.ndimage import distance_transform_edt
    img = rgba.astype(float) / 255
    rgb, a = img[..., :3], img[..., 3]
    rgb = np.clip((rgb - GRADE_BLACK) / (GRADE_WHITE - GRADE_BLACK), 0, 1)
    lum = (rgb * [0.299, 0.587, 0.114]).sum(-1, keepdims=True)
    rgb = np.clip(lum + (rgb - lum) * GRADE_SAT, 0, 1)
    rgb = np.clip(rgb * GRADE_TINT, 0, 1)
    d = distance_transform_edt(a > 0.5)  # inside: distance to the outside
    rim = np.clip(1 - (d - 1) / RIM_WIDTH, 0, 1) * (a > 0.5) * RIM_GAIN
    rgb = np.clip(rgb + RIM_RGB * rim[..., None], 0, 1)
    return np.clip(np.dstack([rgb, a]) * 255 + 0.5, 0, 255).astype(np.uint8)


# the game's blades (measured on its full bodies): a white core ~3 px wide, then a narrow band of pure colour and a
# short fade, all nearly opaque: drawn from the blade's shape in the glow render, over the graded body
BLADE_COLOURS = {"green": (48, 240, 80), "red": (248, 40, 32), "blue": (24, 68, 224)}
BLADE_CORE = (248, 252, 248)
BLADE_PROFILE = [(1.0, 0.95), (2.0, 0.75), (3.0, 0.35), (4.0, 0.1)]  # (distance, alpha)


def blade_layer(glow_png):
    from scipy.ndimage import distance_transform_edt
    g = np.array(Image.open(glow_png).convert("RGBA")).astype(float) / 255
    lit = g[..., 3] > 0.5
    if not lit.any():
        return None
    mean = (g[..., :3][lit]).mean(0)
    colour = np.array(BLADE_COLOURS[("red", "green", "blue")[int(np.argmax(mean))]], float) / 255
    small = np.array(Image.fromarray((g[..., 3] * 255).astype(np.uint8)).resize((128, 256), Image.BOX)).astype(float) / 255
    core = small > 0.45
    d = distance_transform_edt(~core)
    alpha = np.zeros_like(d)
    prev = 0.0
    for dist, a in BLADE_PROFILE:
        alpha = np.where((d > prev) & (d <= dist), a, alpha)
        prev = dist
    alpha[core] = 1.0
    white = np.clip(1.0 - d / 0.9, 0, 1)[..., None]  # the core and the pixel next to it white, then the colour
    rgb = white * np.array(BLADE_CORE, float) / 255 + (1 - white) * colour
    return np.dstack([rgb, alpha])


def over(top, under):
    ta, ua = top[..., 3:], under[..., 3:]
    oa = ta + ua * (1 - ta)
    rgb = (top[..., :3] * ta + under[..., :3] * ua * (1 - ta)) / np.maximum(oa, 1e-4)
    return np.dstack([rgb, oa])


# the halo the game's own full bodies have around the figure: a blue-grey glow fading over ~7 px (measured on them)
HALO_RGB = np.array([60, 80, 115], float) / 255
HALO_PEAK, HALO_FALL = 0.65, 0.47


def add_halo(rgba):
    from scipy.ndimage import distance_transform_edt
    img = rgba.astype(float) / 255
    a = img[..., 3]
    d = distance_transform_edt(a < 0.9)
    halo = np.where(d >= 1, HALO_PEAK * np.exp(-HALO_FALL * (d - 1)), HALO_PEAK)
    halo[d > 9] = 0
    out_a = a + halo * (1 - a)
    out_rgb = (img[..., :3] * a[..., None] + HALO_RGB * (halo * (1 - a))[..., None]) / np.maximum(out_a, 1e-4)[..., None]
    return np.clip(np.dstack([out_rgb, out_a]) * 255 + 0.5, 0, 255).astype(np.uint8)


def to565(c):
    c = np.clip(np.round(c), 0, 255).astype(int)
    return (c[0] * 31 // 255) << 11 | (c[1] * 63 // 255) << 5 | (c[2] * 31 // 255)


def from565(v):
    return np.array([(v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31], float)


def colour_block(px):
    """8 bytes: endpoints along the block's main axis, four colours."""
    mean = px.mean(0)
    d = px - mean
    w, v = np.linalg.eigh(d.T @ d)
    axis = v[:, -1]
    t = d @ axis
    best = None
    for lo_q, hi_q in ((0, 100), (3, 97), (8, 92)):
        e0, e1 = mean + axis * np.percentile(t, hi_q), mean + axis * np.percentile(t, lo_q)
        c0, c1 = to565(e0), to565(e1)
        if c0 < c1:
            c0, c1 = c1, c0
        if c0 == c1:
            pal = [from565(c0)] * 4
        else:
            p0, p1 = from565(c0), from565(c1)
            pal = [p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3]
        pal = np.array(pal)
        dist = ((px[:, None, :] - pal[None]) ** 2).sum(-1)
        idx = dist.argmin(1)
        err = dist[np.arange(16), idx].sum()
        if best is None or err < best[0]:
            best = (err, c0, c1, idx)
    _, c0, c1, idx = best
    bits = sum(int(i) << (2 * k) for k, i in enumerate(idx))
    return struct.pack("<HHI", c0, c1, bits)


def alpha_block(a):
    a0, a1 = int(a.max()), int(a.min())
    if a0 == a1:
        return struct.pack("<BB", a0, a1) + bytes(6)
    pal = np.array([a0, a1] + [((6 - j) * a0 + (j + 1) * a1) / 7 for j in range(6)])
    idx = np.abs(a[:, None] - pal[None]).argmin(1)
    bits = sum(int(i) << (3 * k) for k, i in enumerate(idx))
    return struct.pack("<BB", a0, a1) + bits.to_bytes(6, "little")


def header(name, size):
    h = bytearray(0x184)
    h[0:4] = b"STX\0"
    struct.pack_into("<IIIIIIII", h, 4, 4, 4, 128, 256, 128, 256, 127, 0)
    struct.pack_into("<II", h, 0x24, 1, 1)
    struct.pack_into("<I", h, 0x30, 3)
    struct.pack_into("<I", h, 0x34, size)  # the BIR block's size (file size = 0x104 + this)
    struct.pack_into("<I", h, 0x38, 0x80)
    h[0x3C] = 0x52
    h[0x40:0x40 + len(name)] = name.encode()
    struct.pack_into("<II", h, 0x68, 2, 10)
    struct.pack_into("<IIII", h, 0x84, 0x8080, 0x80, 0x8080, 0x8080)
    h[0x104:0x108] = b"BIR\0"
    return bytes(h)


def encode(rgba, name):
    h, w = rgba.shape[:2]
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            blk = rgba[by:by + 4, bx:bx + 4].reshape(16, 4).astype(float)
            out += alpha_block(blk[:, 3]) + colour_block(blk[:, :3])
    return header(name, 0x80 + len(out)) + bytes(out)


if __name__ == "__main__":
    src, tag, dst, name = sys.argv[1:5]
    body = grade(compose("%s/%s.png" % (src, tag), "%s/%s_glow.png" % (src, tag), glow=False)).astype(float) / 255
    blades = blade_layer("%s/%s_glow.png" % (src, tag))
    if blades is not None:
        body = over(blades, body)
    rgba = add_halo(np.clip(body * 255 + 0.5, 0, 255).astype(np.uint8))
    open(dst, "wb").write(encode(rgba, name))
    Image.fromarray(rgba).save(dst[:-4] + ".png")
