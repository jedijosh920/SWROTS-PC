#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Build the slim curved LSaberKnight (Serra) hilt: the game's thick, heavily bent version, refined.

    python tools/weapons/lsaberknight_hilt.py <lsaberknight.msh> <lsaberknight.stx> <mods folder>
                                              [--obj] [--atlas]

The two input files come from the user's own copy of the game (dump them as in
docs/modding/dumping-assets.md, from meshes\weapons\lsaberknight\). They are used as templates only:
the model and the texture are generated here, and the parts of the file formats not understood are
copied from the inputs (tools/weapons/msh.py, stx.py).

Writes the replacement hilt for the knight saber (docs/modding/replacing-weapons.md):
    <mods>\meshes\weapons\lsaberknight\lsaberknight.msh, lsaberknight.stx
--obj also writes lsaberknight_hilt.obj, .mtl and .png (for Blender), --atlas the texture as
lsaberknight_atlas.png.

The proportions are measured from the "before vs. after (slim)" turnaround of the hilt: the same length
as the original (y = -7.15 .. 4.52, so the blade leaves the emitter where it did), a slimmer tube, a
shallower curve, three milled slots in the neck, seven bright fins over a dark core, the clamp box with
its copper speaker, red button, brass pin and wire, the short fin stack, the angled guard disc and the
black blade socket.

Coordinates are the game's: the hilt's axis is +y, the blade leaves the top, the curve bends towards +z.
Angles go round the axis: 0 = +x, 90 = +z.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msh  # noqa: E402
import stx  # noqa: E402

TAU = 2.0 * math.pi

# ---------------------------------------------------------------- scale
LENGTH = 11.77
Y_POMMEL = -6.93

# ---------------------------------------------------------------- centreline
BEND = math.radians(13.5)      # tangent angle at the pommel
BEND_END = 7.55                # s where the hilt straightens for the clamp and emitter
_H = 0.005
_S = np.arange(0.0, LENGTH + 3 * _H, _H)
_ANG = np.where(_S < BEND_END, BEND * (1.0 - _S / BEND_END), 0.0)
_Y = Y_POMMEL + np.concatenate([[0.0], np.cumsum(np.cos(_ANG[:-1]) * _H)])
_Z = -np.cumsum(np.concatenate([[0.0], np.sin(_ANG[:-1]) * _H]))
_Z = _Z - _Z[-1]


def cl(s):
    """Point of the centreline at s, plus its tangent angle."""
    y = float(np.interp(s, _S, _Y))
    z = float(np.interp(s, _S, _Z))
    a = float(np.interp(s, _S, _ANG))
    return y, z, a


def frame(s):
    """(tangent, normal, binormal) of the centreline, normal towards +z."""
    y, z, a = cl(s)
    t = np.array([0.0, math.cos(a), -math.sin(a)])
    n = np.array([0.0, math.sin(a), math.cos(a)])
    x = np.array([1.0, 0.0, 0.0])
    return t, n, x


def point(s, th, r):
    """A point r from the centre at angle th (0 = +x, 90 = +z side) at s."""
    y, z, a = cl(s)
    return np.array([r * math.cos(th), y + r * math.sin(th) * math.sin(a), z + r * math.sin(th) * math.cos(a)])


def radial(s, th):
    t, n, x = frame(s)
    return math.cos(th) * x + math.sin(th) * n


# ---------------------------------------------------------------- texture atlas
REGIONS = {
    "chrome": (0, 0, 64, 48),
    "steel": (64, 0, 32, 32),
    "red": (96, 0, 16, 16),
    "brass": (112, 0, 16, 16),
    "knurl": (96, 16, 32, 16),
    "black": (64, 32, 32, 24),
    "dark": (96, 32, 32, 24),
    "mesh": (0, 48, 16, 16),
    "slot": (16, 48, 32, 16),
    "cap": (48, 48, 32, 16),
    "tip": (80, 48, 32, 24),
    "satin": (0, 64, 32, 24),
    "grip": (32, 64, 32, 24),
    "plate": (64, 64, 32, 24),
}
INSET = 1.5
SIZE = 128


def rect(name, fu, fv):
    x, y, w, h = REGIONS[name]
    return [(x + INSET + fu * (w - 2 * INSET)) / SIZE, (y + INSET + fv * (h - 2 * INSET)) / SIZE]


def disc_uv(name, sx, sy):
    x, y, w, h = REGIONS[name]
    rho = min(w, h) / 2 - INSET
    return [(x + w / 2 + sx * rho) / SIZE, (y + h / 2 + sy * rho) / SIZE]


# ---------------------------------------------------------------- mesh
class Mesh:
    def __init__(self):
        self.P, self.N, self.UV, self.T = [], [], [], []

    def v(self, p, n, uv):
        p = np.asarray(p, float)
        n = np.asarray(n, float)
        n = n / (np.linalg.norm(n) or 1.0)
        self.P.append(p)
        self.N.append(n)
        self.UV.append((float(uv[0]), float(uv[1])))
        return len(self.P) - 1

    def tri(self, a, b, c):
        A, B, C = self.P[a], self.P[b], self.P[c]
        cr = np.cross(B - A, C - A)
        if np.linalg.norm(cr) < 1e-10:
            return
        nn = self.N[a] + self.N[b] + self.N[c]
        if cr.dot(nn) < 0:
            b, c = c, b
        self.T.append((a, b, c))

    def quad(self, a, b, c, d):
        self.tri(a, b, c)
        self.tri(a, c, d)

    def count(self):
        return len(self.P), len(self.T)


def grid(m, f, us, vs, region, uv=None, closed=False):
    """A surface patch f(u, v) -> (point, normal); uv(i, j) overrides the default rect."""
    rows = []
    for i, u in enumerate(us):
        row = []
        for j, v in enumerate(vs):
            p, nn = f(u, v)
            uv_ = uv(i, j) if uv else rect(region, j / max(len(vs) - 1, 1), i / max(len(us) - 1, 1))
            row.append(m.v(p, nn, uv_))
        rows.append(row)
    for i in range(len(us) - 1):
        for j in range(len(vs) - 1):
            m.quad(rows[i][j], rows[i][j + 1], rows[i + 1][j + 1], rows[i + 1][j])
    return rows


def tube(m, s0, s1, r0, r1, region, n=40, steps=24, bump=None, u0=0.0, u1=1.0, uv=None):
    """A body of revolution along the centreline. bump(s, th) -> extra radius (negative = groove)."""
    def f(s, th):
        r = r0 + (r1 - r0) * ((s - s0) / (s1 - s0)) if s1 != s0 else r0
        if bump:
            r += bump(s, th)
        p = point(s, th, r)
        # normal from the surface's derivatives
        h = 1e-3
        ps = point(min(s + h, s1), th, r + (bump(min(s + h, s1), th) if bump else 0.0))
        ps0 = point(max(s - h, s0), th, r + (bump(max(s - h, s0), th) if bump else 0.0))
        pt = point(s, th + 1e-3, r)
        pt0 = point(s, th - 1e-3, r)
        nn = np.cross(ps - ps0, pt - pt0)
        rad = radial(s, th)
        if nn.dot(rad) < 0:
            nn = -nn
        if np.linalg.norm(nn) < 1e-9:
            nn = rad
        return p, nn

    us = [s0 + (s1 - s0) * i / steps for i in range(steps + 1)]
    vs = [u0 * TAU + (u1 - u0) * TAU * j / n for j in range(n + 1)]
    grid(m, f, us, vs, region, uv=uv)


def revolve(m, s0, profile, region, n=40, angle0=0.0, angle1=1.0):
    """A lathe around the curved axis: profile is [(ds, r), ...] from one end to the other.
    Corners are sharp; normals of each segment are constant round the ring."""
    pts = [(s0 + ds, r) for ds, r in profile]
    for k in range(len(pts) - 1):
        (sa, ra), (sb, rb) = pts[k], pts[k + 1]
        if abs(sb - sa) < 1e-9 and abs(rb - ra) < 1e-9:
            continue
        dr, ds = rb - ra, sb - sa
        length = math.hypot(dr, ds)
        nr, ns = ds / length, -dr / length
        ring0, ring1 = [], []
        for j in range(n + 1):
            th = angle0 * TAU + (angle1 - angle0) * TAU * j / n
            rad = radial(sa, th)
            nb = rad * nr + frame(sa)[0] * ns
            ring0.append(m.v(point(sa, th, ra), nb, rect(region, j / n, 0.0)))
            nc = rad * nr + frame(sb)[0] * ns
            ring1.append(m.v(point(sb, th, rb), nc, rect(region, j / n, 0.6)))
        for j in range(n):
            m.quad(ring0[j], ring0[j + 1], ring1[j + 1], ring1[j])


def disc(m, s, r_out, r_in, region, n=40, up=1):
    """A flat ring at s, facing up (+tangent) or down."""
    t, _, _ = frame(s)
    outer, inner = [], []
    for j in range(n + 1):
        th = TAU * j / n
        nr = t * up
        outer.append(m.v(point(s, th, r_out), nr, disc_uv(region, math.cos(th), math.sin(th))))
        inner.append(m.v(point(s, th, r_in), nr, disc_uv(region, 0.45 * math.cos(th), 0.45 * math.sin(th))))
    for j in range(n):
        if up > 0:
            m.quad(inner[j], inner[j + 1], outer[j + 1], outer[j])
        else:
            m.quad(outer[j], outer[j + 1], inner[j + 1], inner[j])


def flat_disc(m, s, r, region, n=40, up=1):
    """A solid disc across the hilt at s."""
    t, _, _ = frame(s)
    c = np.array([0.0, cl(s)[0], cl(s)[1]])
    centre = m.v(c, t * up, disc_uv(region, 0, 0))
    ring = []
    for j in range(n + 1):
        th = TAU * j / n
        ring.append(m.v(point(s, th, r), t * up, disc_uv(region, math.cos(th), math.sin(th))))
    for j in range(n):
        m.tri(centre, ring[j], ring[j + 1])
    return centre


def torus(m, centre, axis, radius, tube_r, region, n=40, m_seg=10):
    """A ring lying in the plane whose normal is axis."""
    a = np.asarray(axis, float)
    a /= np.linalg.norm(a)
    u = np.array([0.0, 0.0, 1.0])
    if abs(a.dot(u)) > 0.9:
        u = np.array([1.0, 0.0, 0.0])
    u = np.cross(a, u)
    u /= np.linalg.norm(u)
    v = np.cross(a, u)
    grid_p = []
    for i in range(n + 1):
        ph = TAU * i / n
        cdir = math.cos(ph) * u + math.sin(ph) * v
        ring = []
        for j in range(m_seg + 1):
            ps = TAU * j / m_seg
            nrm = math.cos(ps) * cdir + math.sin(ps) * a
            p = np.asarray(centre) + cdir * radius + nrm * tube_r
            ring.append((p, nrm))
        grid_p.append(ring)
    ids = [[m.v(p, nn, rect(region, j / m_seg, i / n)) for j, (p, nn) in enumerate(row)] for i, row in enumerate(grid_p)]
    for i in range(n):
        for j in range(m_seg):
            m.quad(ids[i][j], ids[i][j + 1], ids[i + 1][j + 1], ids[i + 1][j])


def cylinder(m, origin, axis, up, r, h0, h1, region, n=20, side_region=None):
    a = np.asarray(axis, float)
    a = a / np.linalg.norm(a)
    u = np.asarray(up, float)
    u = u - a * u.dot(a)
    u = u / np.linalg.norm(u)
    v = np.cross(a, u)
    o = np.asarray(origin, float)
    rings = []
    for h in (h0, h1):
        ring = []
        for k in range(n + 1):
            ph = TAU * k / n
            rad = math.cos(ph) * u + math.sin(ph) * v
            ring.append(m.v(o + a * h + rad * r, rad, rect(side_region or region, k / n, 0.0 if h == h1 else 1.0)))
        rings.append(ring)
    for k in range(n):
        m.quad(rings[0][k], rings[0][k + 1], rings[1][k + 1], rings[1][k])
    centre = m.v(o + a * h1, a, disc_uv(region, 0, 0))
    top = [m.v(o + a * h1 + (math.cos(TAU * k / n) * u + math.sin(TAU * k / n) * v) * r, a,
               disc_uv(region, math.cos(TAU * k / n), math.sin(TAU * k / n))) for k in range(n + 1)]
    for k in range(n):
        m.tri(centre, top[k], top[k + 1])


def box(m, s0, s1, x0, x1, r0, r1, region, side=-1):
    """A block in the hilt's frame, on the +z side (side=1) or the -z side (-1)."""
    c0 = np.array([0.0, cl(s0)[0], cl(s0)[1]])
    c1 = np.array([0.0, cl(s1)[0], cl(s1)[1]])
    t0, n0, x0v = frame(s0)
    t1, n1, x1v = frame(s1)
    n0, n1 = n0 * side, n1 * side
    pts = [c0 + n0 * r0 + x0v * x0, c0 + n0 * r0 + x0v * x1, c0 + n0 * r1 + x0v * x1, c0 + n0 * r1 + x0v * x0,
           c1 + n1 * r0 + x1v * x0, c1 + n1 * r0 + x1v * x1, c1 + n1 * r1 + x1v * x1, c1 + n1 * r1 + x1v * x0]
    centre = np.mean([c0, c1], axis=0) + n0 * ((r0 + r1) / 2)
    faces = [(0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    for f in faces:
        ps = [pts[i] for i in f]
        nrm = np.cross(ps[1] - ps[0], ps[2] - ps[0])
        if nrm.dot(np.mean(ps, 0) - centre) < 0:
            ps = ps[::-1]
            nrm = -nrm
        nrm /= np.linalg.norm(nrm)
        ids = [m.v(p, nrm, rect(region, (0.2 + 0.6 * (k % 2)), (0.2 + 0.6 * (k // 2)))) for k, p in enumerate(ps)]
        m.quad(*ids)


def wire(m, p0, p1, sag, r, region, n=24, m_seg=8):
    """A thin tube along a quadratic curve from p0 to p1, pulled by the vector `sag`."""
    p0 = np.asarray(p0, float)
    p1 = np.asarray(p1, float)
    mid = (p0 + p1) / 2 + np.asarray(sag, float)
    pts = [(1 - u) ** 2 * p0 + 2 * (1 - u) * u * mid + u * u * p1 for u in np.linspace(0, 1, n + 1)]
    rings = []
    for i, p in enumerate(pts):
        q = pts[min(i + 1, len(pts) - 1)] - pts[max(i - 1, 0)]
        q = q / (np.linalg.norm(q) or 1)
        u = np.array([0.0, 0.0, 1.0])
        u = u - q * u.dot(q)
        if np.linalg.norm(u) < 1e-6:
            u = np.array([1.0, 0.0, 0.0])
        u /= np.linalg.norm(u)
        v = np.cross(q, u)
        ring = []
        for k in range(m_seg + 1):
            ph = TAU * k / m_seg
            nrm = math.cos(ph) * u + math.sin(ph) * v
            ring.append(m.v(p + nrm * r, nrm, rect(region, k / m_seg, i / n)))
        rings.append(ring)
    for i in range(n):
        for k in range(m_seg):
            m.quad(rings[i][k], rings[i][k + 1], rings[i + 1][k + 1], rings[i + 1][k])


# ---------------------------------------------------------------- slots
SIDE = -1                      # the clamp block sits on the -z side
SLOTS = [  # (centre s, angle deg, half length, half width)
    (2.62, 152, 0.80, 0.072),
    (3.52, 197, 0.92, 0.072),
    (4.36, 242, 0.80, 0.072),
    (3.10, 292, 0.70, 0.072),
]
SLOT_DEPTH = 0.085


def slot_mask(s, th):
    """1 in the middle of a milled slot, 0 away from one."""
    best = 0.0
    r = 0.5
    for sc, deg, hl, hw in SLOTS:
        tc = math.radians(deg)
        dth = ((th - tc + math.pi) % TAU) - math.pi
        ds = s - sc
        darc = dth * r
        along = max(abs(ds) - (hl - hw), 0.0)
        side = max(abs(darc) - hw, 0.0)
        d = math.hypot(along, side)
        best = max(best, min(max(1.0 - d / 0.006, 0.0), 1.0))
    return best


def slot_uv(s, th, region):
    if slot_mask(s, th) > 0.02:
        return rect("slot", 0.5, 0.5)
    return None


def build():
    m = Mesh()

    # pommel: knurled cap, small nose, collar
    revolve(m, 0.00, [(0.00, 0.30), (0.030, 0.32), (0.075, 0.34), (0.10, 0.40),
                      (0.135, 0.46), (0.150, 0.47), (0.175, 0.64), (0.205, 0.65), (0.96, 0.65),
                      (1.00, 0.62), (1.045, 0.56)], "knurl", n=20)
    flat_disc(m, 0.0, 0.30, "cap", n=24, up=-1)
    revolve(m, 1.055, [(0.0, 0.55), (0.0, 0.52), (0.02, 0.52)], "steel", n=40)

    # recessed window on the pommel's camera side
    win_s0, win_s1 = 0.34, 0.86
    win_z0, win_z1 = -0.15, 0.15
    x_face = -0.658
    q = [(win_s0, win_z0), (win_s1, win_z0), (win_s1, win_z1), (win_s0, win_z1)]
    plate = []
    for ss, zz in q:
        yy, zc, _ = cl(ss)
        plate.append(m.v((x_face, yy, zc + zz), np.array([-1.0, 0.0, 0.0]), rect("dark", 0.35, 0.35)))
    m.quad(plate[0], plate[1], plate[2], plate[3])
    # black band
    tube(m, 1.055, 1.30, 0.535, 0.545, "black", n=16, steps=3)

    # neck: chrome, tapering, with milled slots
    def groove(s, th):
        return -SLOT_DEPTH * slot_mask(s, th)

    def neck_uv(i, j):
        s = 1.30 + (5.16 - 1.30) * i / NECK_STEPS
        th = TAU * j / NECK_N
        m = slot_mask(s, th)
        if m > 0.45:
            return rect("slot", 0.5, 0.5)
        return rect("chrome", j / NECK_N, 0.5 + 0.5 * (s - 1.3) / 3.86)

    NECK_STEPS, NECK_N = 16, 22
    tube(m, 1.30, 5.16, 0.560, 0.520, "chrome", n=NECK_N, steps=NECK_STEPS, bump=groove, uv=neck_uv)

    # step ring into the fins
    revolve(m, 5.16, [(0.0, 0.505), (0.0, 0.545), (0.04, 0.545), (0.05, 0.49)], "steel", n=16)

    # fin stack: dark core with seven bright fins
    tube(m, 5.21, 7.20, 0.470, 0.470, "dark", n=16, steps=6)
    for i in range(7):
        sf = 5.36 + i * 0.278
        revolve(m, sf, [(-0.038, 0.470), (-0.038, 0.552), (0.038, 0.552), (0.038, 0.470)], "chrome", n=14)

    # clamp body under the box
    tube(m, 7.20, 8.50, 0.490, 0.490, "dark", n=16, steps=6)

    # the clamp block on the -z side
    box(m, 7.18, 8.18, -0.33, 0.33, 0.20, 0.845, "satin", side=SIDE)
    # a thin raised frame on its face
    s_btn, s_pin = 7.44, 7.84
    t, n, x = frame(s_btn)
    c = np.array([0.0, cl(s_btn)[0], cl(s_btn)[1]])
    cylinder(m, c + n * SIDE * 0.842, n * SIDE, t, 0.10, 0.0, 0.085, "red", n=12)
    t2, n2, x2 = frame(s_pin)
    c2 = np.array([0.0, cl(s_pin)[0], cl(s_pin)[1]])
    cylinder(m, c2 + n2 * SIDE * 0.842, n2 * SIDE, t2, 0.08, 0.0, 0.07, "brass", n=10)

    # copper speaker mesh on the +x face
    sm = 7.68
    t3, n3, x3 = frame(sm)
    c3 = np.array([0.0, cl(sm)[0], cl(sm)[1]])
    c3 = c3 + n3 * SIDE * 0.58 - x3 * 0.325
    cylinder(m, c3, -x3, t3, 0.235, 0.0, 0.055, "mesh", n=16)

    # terminals and wire
    base = c3 - n3 * SIDE * 0.20 - x3 * 0.02
    p0 = base + t3 * -0.26
    p1 = base + t3 * 0.28
    cylinder(m, p0, -x3, t3, 0.06, 0.0, 0.10, "mesh", n=10)
    cylinder(m, p1, -x3, t3, 0.06, 0.0, 0.10, "mesh", n=10)
    wire(m, p0 - x3 * 0.10, p1 - x3 * 0.10, n3 * -SIDE * 0.14, 0.018, "red", n=12, m_seg=6)

    # short dark fin stack behind the clamp
    tube(m, 8.50, 8.96, 0.505, 0.505, "dark", n=16, steps=4)
    for i in range(5):
        sf = 8.55 + i * 0.093
        revolve(m, sf, [(-0.015, 0.505), (-0.015, 0.535), (0.015, 0.535), (0.015, 0.505)], "steel", n=12)

    # angled shroud: a plate whose faces are slightly conical
    s_sh = 9.00
    tilt = math.radians(-20.0)
    t, n, x = frame(s_sh)
    c = np.array([0.0, cl(s_sh)[0], cl(s_sh)[1]])
    n0 = t * math.cos(tilt) + n * math.sin(tilt)
    n0 /= np.linalg.norm(n0)
    in_u = x
    in_v = np.cross(n0, in_u)
    in_v /= np.linalg.norm(in_v)
    r_out, r_in = 1.18, 0.52
    ring_p = {}
    for name, rr, hh, side in (("in_f", r_in, 0.015, +1), ("out_f", r_out, 0.095, +1),
                               ("out_b", r_out, 0.045, -1), ("in_b", r_in, -0.035, -1)):
        ring_p[name] = [c + n0 * hh + (math.cos(TAU * j / 48) * in_u + math.sin(TAU * j / 48) * in_v) * rr
                        for j in range(49)]
    nf = n0
    nb = -n0
    nr = [math.cos(TAU * j / 48) * in_u + math.sin(TAU * j / 48) * in_v for j in range(49)]
    ids = {}
    for name in ("in_f", "out_f", "out_b", "in_b"):
        ids[name] = []
        for j, p in enumerate(ring_p[name]):
            nrm = nf if name in ("in_f", "out_b") else (0.45 * nf + 0.9 * nr[j])
            ids[name].append(m.v(p, nrm, disc_uv("satin", math.cos(TAU * j / 48), math.sin(TAU * j / 48))))
    for a, b in (("in_f", "out_f"), ("out_f", "out_b"), ("out_b", "in_b"), ("in_b", "in_f")):
        for j in range(48):
            m.quad(ids[a][j], ids[a][j + 1], ids[b][j + 1], ids[b][j])

    # emitter: chrome barrel, flare, bright step, black socket
    revolve(m, 8.96, [(0.00, 0.505), (0.05, 0.525), (1.30, 0.55), (1.38, 0.59), (1.60, 0.715),
                      (1.68, 0.715), (1.71, 0.62), (1.72, 0.515)], "chrome", n=22)
    revolve(m, 10.68, [(0.0, 0.515), (0.04, 0.53), (0.06, 0.53), (0.08, 0.515)], "chrome", n=16)
    revolve(m, 10.76, [(0.0, 0.515), (LENGTH - 10.76 - 0.17, 0.515), (LENGTH - 10.76 - 0.13, 0.53),
                       (LENGTH - 10.76 - 0.11, 0.53), (LENGTH - 10.76, 0.49)], "tip", n=22)
    # socket opening and its inner wall
    disc(m, LENGTH, 0.45, 0.255, "tip", n=16, up=1)
    revolve(m, LENGTH - 0.22, [(0.0, 0.255), (0.22, 0.255)], "slot", n=16)

    # D-ring at the pommel, hanging below the end
    t, n, x = frame(0.0)
    c0 = np.array([0.0, cl(0.0)[0], cl(0.0)[1]])
    ring_c = c0 - t * 0.10 + n * 0.47
    torus(m, ring_c, np.array([1.0, 0.0, 0.0]), 0.40, 0.045, "chrome", n=18, m_seg=6)
    cylinder(m, c0 + n * 0.12, n, t, 0.085, 0.0, 0.20, "steel", n=12)

    return m


def paint_atlas():
    rng = np.random.default_rng(4)
    A = np.zeros((SIZE, SIZE, 3))

    def region(name):
        x, y, w, h = REGIONS[name]
        X, Y = np.meshgrid(np.arange(w), np.arange(h))
        return x, y, w, h, X, Y

    def put(name, rgb):
        x, y, w, h = REGIONS[name]
        A[y:y + h, x:x + w] = np.clip(rgb, 0, 255)

    x, y, w, h, X, Y = region("chrome")
    f = X / w
    lum = (182 + 42 * np.sin(2 * np.pi * f * 1.5 + 0.4) + 18 * np.sin(2 * np.pi * f * 4.2 + 1.1)
           + 55 * np.exp(-((f - .30) / .04) ** 2) - 45 * np.exp(-((f - .68) / .07) ** 2)
           + 7 * (0.5 - Y / h) + rng.normal(0, 1.4, (h, w)))
    put("chrome", np.stack([lum, lum, lum + 2], 2))

    x, y, w, h, X, Y = region("steel")
    lum = np.clip(120 + 40 * np.sin(2 * np.pi * X / w * 1.2 + .5) + 25 * np.sin(2 * np.pi * Y / h) + rng.normal(0, 2, (h, w)), 40, 210)
    put("steel", np.stack([lum, lum, lum + 2], 2))

    x, y, w, h, X, Y = region("knurl")
    flute = np.abs(((X % 4) / 4.0) - .5) * 2
    lum = 120 + 95 * (1 - flute) ** 1.4 + 10 * np.sin(2 * np.pi * Y / h * 3) + rng.normal(0, 1.2, (h, w))
    lum = np.clip(lum, 55, 232)
    put("knurl", np.stack([lum, lum, lum + 2], 2))

    x, y, w, h, X, Y = region("black")
    put("black", np.stack([np.full((h, w), 24.0) + rng.normal(0, 1.2, (h, w))] * 2 + [
        np.full((h, w), 27.0) + rng.normal(0, 1.2, (h, w))], 2))

    x, y, w, h, X, Y = region("dark")
    lum = 52 + 8 * np.sin(2 * np.pi * X / w) + rng.normal(0, 1.5, (h, w))
    put("dark", np.stack([lum, lum, lum + 2], 2))

    x, y, w, h, X, Y = region("satin")
    lum = 120 + 30 * np.sin(2 * np.pi * X / w * .8 + .3) + 12 * np.sin(2 * np.pi * Y / h * 2) + rng.normal(0, 1.8, (h, w))
    put("satin", np.stack([lum, lum, lum + 2], 2))

    x, y, w, h, X, Y = region("red")
    rr = np.hypot(X - w / 2, Y - h / 2) / (w / 2)
    red = np.stack([150 - 25 * rr, 52 - 8 * rr, 38 - 6 * rr], 2) + rng.normal(0, 1.5, (h, w, 1))
    put("red", red)

    x, y, w, h, X, Y = region("brass")
    rr = np.hypot(X - w / 2, Y - h / 2) / (w / 2)
    brass = np.stack([200 - 30 * rr, 160 - 26 * rr, 70 - 16 * rr], 2) + rng.normal(0, 1.5, (h, w, 1))
    put("brass", brass)

    x, y, w, h, X, Y = region("mesh")
    rr = np.hypot(X - w / 2 + .5, Y - h / 2 + .5) / (w / 2)
    dots = (np.sin(X * 2.2) * np.sin(Y * 2.2)) > 0.25
    copper = np.stack([168 + 40 * dots, 100 + 26 * dots, 58 + 16 * dots], 2)
    copper = np.where((rr > .95)[..., None], np.stack([90 * np.ones_like(rr)] * 3, 2), copper)
    put("mesh", copper + rng.normal(0, 3, (h, w, 1)))

    x, y, w, h, X, Y = region("slot")
    lum = 12 + 5 * (Y / h) + rng.normal(0, .8, (h, w))
    put("slot", np.stack([lum, lum, lum + 1], 2))

    x, y, w, h, X, Y = region("cap")
    Yy, Xx = np.mgrid[0:h, 0:w]
    rr = np.hypot(Xx + .5 - w / 2, Yy + .5 - h / 2) / (w / 2)
    lum = 160 + 25 * np.cos(rr * 2 * np.pi * 2.0) + 20 * (Xx - Yy) / w
    lum -= 60 * np.exp(-((rr - .55) / .05) ** 2) + 50 * np.exp(-((rr - .82) / .04) ** 2)
    put("cap", np.stack([lum, lum, lum + 3], 2))

    x, y, w, h, X, Y = region("tip")
    lum = 68 + 9 * np.sin(2 * np.pi * X / w) + 5 * (Y / h) + rng.normal(0, 1.0, (h, w))
    put("tip", np.stack([lum, lum, lum + 1], 2))

    x, y, w, h, X, Y = region("grip")
    put("grip", np.stack([np.full((h, w), 30.0) + rng.normal(0, 1, (h, w))] * 3, 2))

    return Image.fromarray(np.clip(A, 0, 255).astype(np.uint8))


def to_obj(m, path):
    with open(path, "w") as f:
        f.write("# LSaberKnight slim hilt\nmtllib lsaberknight.mtl\no LSaberKnight\n")
        for p in m.P:
            f.write("v %.6f %.6f %.6f\n" % (p[0], p[1], p[2]))
        for uv in m.UV:
            f.write("vt %.6f %.6f\n" % (uv[0], 1.0 - uv[1]))
        for n in m.N:
            f.write("vn %.6f %.6f %.6f\n" % (n[0], n[1], n[2]))
        for a, b, c in m.T:
            f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a + 1, a + 1, a + 1, b + 1, b + 1, b + 1, c + 1, c + 1, c + 1))



# --- the game's mesh ----------------------------------------------------------------------------------------
def to_msh(mesh, template):
    """The generated surface as the game's static weapon mesh, keeping the template's header and tail."""
    m = msh.Mesh().from_template(template)
    m.verts = [dict(pos=[float(v) for v in p], nb=msh.pack_normal(n), pad=0,
                    uv=[float(uv[0]), float(uv[1])], uv1=bytes(8), col=bytes([0, 0, 0, 255]))
               for p, n, uv in zip(mesh.P, mesh.N, mesh.UV)]
    m.tris = [tuple(int(i) for i in t) for t in mesh.T]
    return m


def write_obj(mesh, path, atlas_path):
    """OBJ + MTL + the atlas, for looking at the hilt in Blender (the game's v axis is down)."""
    base = os.path.splitext(path)[0]
    mtl = os.path.basename(base) + ".mtl"
    with open(path, "w") as f:
        f.write("# slim LSaberKnight hilt\nmtllib %s\no LSaberKnight\n" % mtl)
        for p in mesh.P:
            f.write("v %.6f %.6f %.6f\n" % (p[0], p[1], p[2]))
        for uv in mesh.UV:
            f.write("vt %.6f %.6f\n" % (uv[0], 1.0 - uv[1]))
        for n in mesh.N:
            f.write("vn %.6f %.6f %.6f\n" % (n[0], n[1], n[2]))
        f.write("usemtl LSaberKnight\n")
        for a, b, c in mesh.T:
            f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n"
                    % (a + 1, a + 1, a + 1, b + 1, b + 1, b + 1, c + 1, c + 1, c + 1))
    with open(os.path.join(os.path.dirname(path), mtl), "w") as f:
        f.write("newmtl LSaberKnight\nKd 1 1 1\nKs 0.4 0.4 0.4\nNs 80\n"
                "illum 2\nmap_Kd %s\n" % os.path.abspath(atlas_path).replace("\\", "/"))


def main(args):
    flags = {a for a in args if a.startswith("--")}
    paths = [a for a in args if not a.startswith("--")]
    if len(paths) != 3 or flags - {"--obj", "--atlas"}:
        sys.exit(__doc__)
    msh_in, stx_in, mods = paths
    template = msh.parse(open(msh_in, "rb").read())
    mesh = build()
    game = to_msh(mesh, template)
    data = msh.build(game)
    atlas = paint_atlas()
    texture = stx.encode(open(stx_in, "rb").read(), atlas)

    folder = os.path.join(mods, "meshes", "weapons", "lsaberknight")
    os.makedirs(folder, exist_ok=True)
    open(os.path.join(folder, "lsaberknight.msh"), "wb").write(data)
    open(os.path.join(folder, "lsaberknight.stx"), "wb").write(texture)
    print("wrote", folder)
    atlas_png = os.path.abspath("lsaberknight_hilt.png")
    if "--obj" in flags or "--atlas" in flags:
        atlas.save(atlas_png)
    if "--obj" in flags:
        write_obj(mesh, "lsaberknight_hilt.obj", atlas_png)
        print("wrote lsaberknight_hilt.obj, .mtl and lsaberknight_hilt.png")
    if "--atlas" in flags:
        atlas.save("lsaberknight_atlas.png")
        print("wrote lsaberknight_atlas.png")
    ys = [v["pos"][1] for v in game.verts]
    print(f"{len(game.verts)} vertices, {len(game.tris)} triangles; y {min(ys):.2f} .. {max(ys):.2f}; "
          f"texture name {stx.texture_name(texture)}")


if __name__ == "__main__":
    main(sys.argv[1:])
