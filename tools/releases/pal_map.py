"""Generate src/game/pal_map.inc: where the North American executable's addresses are in the PAL one.

The port is written against the North American default.xbe (MD5 6460ef37862de3af364a0563a0a18c4e).
The PAL release (France, MD5 84ae8a86018f3ab4a1b5756b238cefd2; Europe En/Es/It, MD5
a48f65aa84a9848030b8644e1fd35b89; both the same code) is a build of the same sources two days
older, linked a little differently:

  - every section after .text starts 0xA0 bytes lower, with the same contents apart from pointers;
  - .data's zero-filled tail (.bss) is 0xA0, 0xA4 or 0xA8 bytes lower, depending on the variable;
  - .text is the same functions, a few of them a little longer or shorter, so the shift changes from
    function to function, and a few functions sit elsewhere.

This script finds, for every North American address, the PAL one:

  1. Code is compared with every relocatable operand masked: 4-byte values that look like addresses
     in the image, and the targets of relative calls and jumps. Byte windows that occur once in each
     executable pair up, and pairs with the same shift are extended for as long as the code agrees.
  2. Function starts are paired from the pointers both executables keep at the same place in their data
     (vtables, function tables) and from the targets of calls in code already paired, and each function
     takes its own start's shift.
  3. Where the two disagree, the one that agrees with the code around it wins.
  4. .bss shifts come from the addresses the paired code uses.

It then checks every address the port uses (every hex literal in src/ inside the image, except the few
values in NOT_ADDRESSES): each must map, and for code, the PAL bytes around it must be the North American
ones with every difference explained by the relocation (an address operand that maps to the PAL one, or a
relative call or jump whose target maps to the PAL one).

Requires Python 3 with numpy.

Usage: python tools/releases/pal_map.py <North American default.xbe> <PAL default.xbe> [--out <file>]
       (--out defaults to src/game/pal_map.inc; --check only verifies, writes nothing)
"""
import bisect
import collections
import hashlib
import os
import re
import struct
import sys

import numpy as np

USA_MD5 = "6460ef37862de3af364a0563a0a18c4e"
PAL_MD5 = {"84ae8a86018f3ab4a1b5756b238cefd2": "France", "a48f65aa84a9848030b8644e1fd35b89": "Europe"}
IMAGE_END = 0x970000
WINDOW = 32

# Hex literals in src/ inside the image range that are not addresses.
NOT_ADDRESSES = {
    0x40000,  # flag bit tested on game objects
    0x100000,  # a count limit
    0x200000,  # first id of objects the port adds to a level
    0xA0042,  # level file version
    0x52030, 0x52049, 0x90007, 0x9000F, 0x90011, 0x90014, 0x9001C,  # mesh file versions
    0x96E000,  # the end of the image's address range, the same in both
    0x300001, 0x300100, 0x380001, 0x390001,  # ids of objects the port adds to a level
    0x20000, 0x30000, 0x50000, 0x70000, 0x80000, 0xF0000, 0x400000, 0x400100, 0x800000,  # flags and sizes
}
# Section ends: one past the last byte. They map like the last byte.
END_MARKERS = {0x612BC8, 0x962BF8}


class Xbe:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.md5 = hashlib.md5(self.data).hexdigest()
        d = self.data
        u32 = lambda o: struct.unpack_from("<I", d, o)[0]
        base = u32(0x104)
        self.sections = []  # (name, va, virtual size, raw size)
        mem = bytearray(IMAGE_END)
        header = u32(0x108)
        mem[base:base + header] = d[:header]
        for i in range(u32(0x11C)):
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", d, u32(0x120) - base + i * 0x38)
            name = d[name_va - base:d.index(b"\0", name_va - base)].decode()
            self.sections.append((name, va, vsize, rsize))
            mem[va:va + rsize] = d[raw:raw + rsize]
        self.mem = bytes(mem)
        self.text = next((va, va + vs) for n, va, vs, rs in self.sections if n == ".text")

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)


def masked(x):
    """The image as int16, with relocatable operands set to -1."""
    m = np.frombuffer(x.mem, dtype=np.uint8)
    d = (m[:-3].astype(np.uint32) | (m[1:-2].astype(np.uint32) << 8) | (m[2:-1].astype(np.uint32) << 16)
         | (m[3:].astype(np.uint32) << 24))
    pos = np.arange(len(d), dtype=np.int64)
    target = pos + 4 + d.astype(np.int32).astype(np.int64)
    prev = np.concatenate([[0], m[:-4]])
    prev2 = np.concatenate([[0, 0], m[:-5]])
    in_text = (target >= x.text[0]) & (target < x.text[1])
    hit = ((d >= 0x10000) & (d < IMAGE_END)) | (((prev == 0xE8) | (prev == 0xE9)) & in_text) \
        | ((prev2 == 0x0F) & ((prev & 0xF0) == 0x80) & in_text)
    mask = np.zeros(len(m), dtype=bool)
    for k in range(4):
        mask[np.nonzero(hit)[0] + k] = True
    out = m.astype(np.int16)
    out[mask] = -1
    return out


def window_hashes(a, lo, hi):
    rng = np.random.default_rng(1)
    weights = rng.integers(1, 2 ** 62, size=WINDOW, dtype=np.uint64)
    v = (a[lo:hi + WINDOW].astype(np.int64) + 2).astype(np.uint64)
    out = np.empty(hi - lo, dtype=np.uint64)
    step = 1 << 20
    for s in range(0, hi - lo, step):
        w = np.lib.stride_tricks.sliding_window_view(v[s:min(hi - lo, s + step) + WINDOW - 1], WINDOW)
        out[s:s + len(w)] = (w * weights).sum(axis=1)
    masked_count = np.convolve((a[lo:hi + WINDOW - 1] < 0).astype(np.int32), np.ones(WINDOW, dtype=np.int32), "valid")
    return out, masked_count[:hi - lo]


def unique_positions(h, good):
    vals, idx, cnt = np.unique(h[good], return_index=True, return_counts=True)
    pos = np.nonzero(good)[0][idx]
    return vals[cnt == 1], pos[cnt == 1]


class TextMap:
    def __init__(self, usa, pal):
        self.usa, self.pal = usa, pal
        self.un, self.pn = masked(usa), masked(pal)
        self.lo, self.uhi = usa.text
        self.phi = pal.text[1]
        n = self.uhi - self.lo
        anchors = self.anchors()
        by_runs = self.from_runs(anchors, n)
        by_functions = self.from_functions(anchors, n)
        self.delta = self.combine(by_runs, by_functions, n)

    def anchors(self):
        uh, um = window_hashes(self.un, self.lo, self.uhi - WINDOW)
        ph, pm = window_hashes(self.pn, self.lo, self.phi - WINDOW)
        uv, up = unique_positions(uh, um <= 12)
        pv, pp = unique_positions(ph, pm <= 12)
        _, iu, ip = np.intersect1d(uv, pv, return_indices=True)
        a = np.stack([up[iu] + self.lo, pp[ip] + self.lo], 1)
        return a[np.argsort(a[:, 0])]

    def agree(self, delta):
        """Per North American text byte: the code agrees (operands masked) under this shift."""
        idx = np.arange(self.lo, self.uhi)
        y = idx + delta
        valid = (y >= self.lo) & (y < self.phi) & (delta > -(1 << 40))
        ok = np.zeros(len(idx), dtype=bool)
        a = self.un[self.lo:self.uhi][valid]
        b = self.pn[np.where(valid, y, self.lo)][valid]
        ok[valid] = (a == b) | (a < 0) | (b < 0)
        return ok

    def from_runs(self, anchors, n):
        d = anchors[:, 1] - anchors[:, 0]
        runs, s = [], 0
        for i in range(1, len(anchors) + 1):
            if i == len(anchors) or d[i] != d[s] or anchors[i, 0] - anchors[i - 1, 0] > 256:
                if i - s >= 2:
                    runs.append((int(anchors[s, 0]), int(anchors[i - 1, 0]) + WINDOW, int(d[s])))
                s = i
        by_delta = collections.defaultdict(list)
        for a, b, delta in runs:
            by_delta[delta].append((a, b))
        extended = []
        for delta, items in by_delta.items():
            ok = self.agree(np.int64(delta))
            edges = np.diff(np.concatenate([[0], ok.astype(np.int8), [0]]))
            starts, ends = np.nonzero(edges == 1)[0], np.nonzero(edges == -1)[0]
            for a, b in items:
                k = np.searchsorted(starts, a - self.lo, side="right") - 1
                if k >= 0 and ends[k] >= b - self.lo:
                    extended.append((int(starts[k]) + self.lo, int(ends[k]) + self.lo, delta))
                else:
                    extended.append((a, b, delta))
        out = np.full(n, -(1 << 62), dtype=np.int64)
        taken = np.zeros(n, dtype=bool)
        for a, b, delta in sorted(extended, key=lambda r: r[0] - r[1]):
            free = ~taken[a - self.lo:b - self.lo]
            out[a - self.lo:b - self.lo][free] = delta
            taken[a - self.lo:b - self.lo] |= free
        return out

    def from_functions(self, anchors, n):
        um, pm = self.usa.mem, self.pal.mem
        votes = collections.defaultdict(collections.Counter)
        for (un_, uva, uvs, urs), (pn_, pva, pvs, prs) in zip(self.usa.sections, self.pal.sections):
            if un_ == ".text":
                continue
            for off in range(0, min(urs, prs) - 3):
                vu = struct.unpack_from("<I", um, uva + off)[0]
                vp = struct.unpack_from("<I", pm, pva + off)[0]
                if self.lo <= vu < self.uhi and self.lo <= vp < self.phi and abs(vp - vu) < 0x200000:
                    votes[vu][vp] += 1
        for x, y in anchors:
            x, y = int(x), int(y)
            for k in range(WINDOW - 4):
                if um[x + k] == 0xE8 and pm[y + k] == 0xE8:
                    tu = x + k + 5 + struct.unpack_from("<i", um, x + k + 1)[0]
                    tp = y + k + 5 + struct.unpack_from("<i", pm, y + k + 1)[0]
                    if self.lo <= tu < self.uhi and self.lo <= tp < self.phi:
                        votes[tu][tp] += 1
        entries = {}
        for tu, c in votes.items():
            (tp, best), = c.most_common(1)
            if best * 2 <= sum(c.values()) or tp + 24 > self.phi:
                continue
            a, b = self.un[tu:tu + 24], self.pn[tp:tp + 24]
            if (((a == b) | (a < 0) | (b < 0)).all()) and ((a == b) & (a >= 0)).sum() >= 8:
                entries[tu] = tp
        out = np.full(n, -(1 << 62), dtype=np.int64)
        starts = sorted(entries)
        for i, e in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else self.uhi
            out[e - self.lo:end - self.lo] = entries[e] - e
        return out

    def combine(self, a, b, n):
        def agreement(delta):
            idx = np.arange(self.lo, self.uhi)
            y = idx + delta
            valid = (y >= self.lo) & (y < self.phi) & (delta > -(1 << 40))
            ok = np.zeros(n, dtype=bool)
            x1 = self.un[self.lo:self.uhi][valid]
            x2 = self.pn[np.where(valid, y, self.lo)][valid]
            ok[valid] = (x1 == x2) | (x1 < 0) | (x2 < 0)
            return np.convolve(ok, np.ones(16), "same")
        return np.where(agreement(b) >= agreement(a), b, a)

    def ranges(self):
        out, i, n = [], 0, len(self.delta)
        while i < n:
            j = i
            while j < n and self.delta[j] == self.delta[i]:
                j += 1
            if self.delta[i] > -(1 << 40):
                out.append((i + self.lo, j + self.lo, int(self.delta[i])))
            i = j
        return out


class Map:
    def __init__(self, ranges):
        self.ranges = sorted(ranges)
        self.starts = [r[0] for r in self.ranges]

    def __call__(self, x):
        i = bisect.bisect_right(self.starts, x) - 1
        if i >= 0 and self.ranges[i][0] <= x < self.ranges[i][1]:
            return x + self.ranges[i][2]
        return None


def bss_ranges(usa, pal, text_map):
    """Shifts of .data's zero-filled tail, from the absolute addresses paired code uses."""
    name, va, vsize, rsize = usa.section(".data")
    section_delta = pal.section(".data")[1] - va
    raw_end, end = va + rsize, va + vsize
    um, pm = usa.mem, pal.mem
    votes = collections.defaultdict(collections.Counter)
    for i in range(usa.text[0] + 2, usa.text[1] - 4):
        t = text_map(i)
        if t is None or um[i - 1] != pm[t - 1] or um[i - 2] != pm[t - 2]:
            continue
        vu = struct.unpack_from("<I", um, i)[0]
        if raw_end <= vu < end:
            vp = struct.unpack_from("<I", pm, t)[0]
            if abs(vp - vu - section_delta) <= 0x40:  # constants that only look like addresses do not shift
                votes[vu][vp - vu] += 1
    points = sorted((vu, c.most_common(1)[0][0]) for vu, c in votes.items())
    # Between two variables with the same shift, everything shifts alike; where the shift changes,
    # the bytes between the two variables are left unmapped.
    out = [(va, raw_end, section_delta)]
    edge, edge_delta = raw_end, section_delta
    for vu, delta in points + [(end, section_delta)]:
        if delta == edge_delta:
            out.append((edge, vu, delta))
        else:
            out.append((edge, edge + 4, edge_delta))
        edge, edge_delta = vu, delta
    merged = []
    for a, b, d in out:
        if b <= a:
            continue
        if merged and merged[-1][2] == d and merged[-1][1] >= a:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b), d)
        else:
            merged.append((a, b, d))
    return merged


def build(usa, pal):
    text = TextMap(usa, pal)
    text_ranges = text.ranges()
    other = [(0x10000, usa.text[0], 0)]
    for (un_, uva, uvs, urs), (pn_, pva, pvs, prs) in zip(usa.sections, pal.sections):
        if un_ not in (".text", ".data"):
            other.append((uva, uva + uvs, pva - uva))
    bss = bss_ranges(usa, pal, Map(text_ranges + other))
    return Map(text_ranges + other + bss)


class Checker:
    def __init__(self, usa, pal, m):
        self.um, self.pm, self.map = usa.mem, pal.mem, m
        self.text = usa.text

    def explained(self, i):
        t = self.map(i)
        if t is None:
            return False
        if self.um[i] == self.pm[t]:
            return True
        for s in range(i - 3, i + 1):
            ts = self.map(s)
            if ts is None or ts != t - (i - s):
                continue
            vu = struct.unpack_from("<I", self.um, s)[0]
            vp = struct.unpack_from("<I", self.pm, ts)[0]
            if 0x10000 <= vu < IMAGE_END and self.map(vu) == vp:
                return True
            tu = (s + 4 + struct.unpack_from("<i", self.um, s)[0]) & 0xFFFFFFFF
            tp = (ts + 4 + struct.unpack_from("<i", self.pm, ts)[0]) & 0xFFFFFFFF
            if self.text[0] <= tu < self.text[1] and self.map(tu) == tp:
                return True
        return False

    def address(self, a):
        """None if the address is fine, else why not."""
        probe = a - 1 if a in END_MARKERS else a
        if self.map(probe) is None:
            return "has no PAL address"
        if self.text[0] <= a < self.text[1] and a != 0x4FEB00:  # a bound near the end of .text, not code
            bad = [x for x in range(a - 12, a + 20) if self.text[0] <= x < self.text[1] and not self.explained(x)]
            if bad:
                return f"PAL code differs near {bad[0]:08X}"
        return None


def used_addresses(root):
    used = collections.defaultdict(list)
    for folder, _, files in os.walk(os.path.join(root, "src")):
        for f in files:
            if not f.endswith((".cpp", ".h", ".inc")) or f == "pal_map.inc":
                continue
            path = os.path.join(folder, f)
            for n, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
                code = line.split("//")[0]
                for m in re.finditer(r"\b0x([0-9A-Fa-f]{5,8})\b", code):
                    v = int(m.group(1), 16)
                    if 0x11000 <= v < IMAGE_END and v not in NOT_ADDRESSES:
                        used[v].append(f"{os.path.relpath(path, root)}:{n}")
    return used


# Rows of tables whose North American addresses are translated where they are used (game::Va).
TRANSLATED_ROWS = [
    ("src/game/game.h", r"^\s*\{ 0x[0-9A-F]{8}, \d+, FsKind::"),
    ("src/game/roster.cpp", r"^\s*(constexpr TableSite \w+ = )?\{ 0x[0-9A-F]{8}, \{"),
    ("src/game/customchars.cpp", r'^\s*\{ "I\w+", "\w+", 0x[0-9A-F]{8}, 0x[0-9A-F]{8}, "\w+" \},'),
    ("src/game/characters.cpp", r"^\s*\{ 0x[0-9A-F]{8}, 0x[0-9A-F]{3}, 0 \},"),
]


def untranslated(root):
    """Address literals in code that nothing translates to the running release's address."""
    found = []
    arrays = re.compile(r"GAME_VAS\((\w+)\)")
    for folder, _, files in os.walk(os.path.join(root, "src")):
        for f in files:
            if not f.endswith((".cpp", ".h")) or f.startswith("release."):
                continue
            path = os.path.join(folder, f)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            text = open(path, encoding="utf-8", errors="replace").read()
            registered = set(arrays.findall(text))
            in_array = False
            for n, line in enumerate(text.splitlines(), 1):
                code = re.sub(r'"(\\.|[^"\\])*"', '""', line.split("//")[0])
                m = re.match(r"\s*(?:static\s+)?uint32_t (\w+)\[\] = \{", code)
                if m:
                    in_array = m.group(1) in registered
                literals = [int(v, 16) for v in re.findall(r"\b0x([0-9A-Fa-f]{5,8})\b", code)]
                literals = [v for v in literals if 0x11000 <= v < IMAGE_END and v not in NOT_ADDRESSES]
                if literals and not in_array:
                    bare = re.sub(r"(GAME_VA(_END)?\(\w+, |Va\()0x[0-9A-Fa-f]+", "", code)
                    left = [v for v in re.findall(r"\b0x([0-9A-Fa-f]{5,8})\b", bare)
                            if 0x11000 <= int(v, 16) < IMAGE_END and int(v, 16) not in NOT_ADDRESSES]
                    if left and not any(rel == f_ and re.match(r, line) for f_, r in TRANSLATED_ROWS):
                        found.append(f"{rel}:{n}: {line.strip()[:100]}")
                if in_array and "};" in code:
                    in_array = False
    return found


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(root, "src", "game", "pal_map.inc")
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
        args.remove(out)
    if len(args) != 2:
        sys.exit(__doc__)
    usa, pal = Xbe(args[0]), Xbe(args[1])
    if usa.md5 != USA_MD5:
        sys.exit(f"{args[0]}: MD5 {usa.md5}, not the North American release ({USA_MD5})")
    if pal.md5 not in PAL_MD5:
        sys.exit(f"{args[1]}: MD5 {pal.md5}, not a known PAL release")
    m = build(usa, pal)
    check = Checker(usa, pal, m)
    problems = []
    used = used_addresses(root)
    for a in sorted(used):
        why = check.address(a)
        if why:
            problems.append(f"{a:08X} {why} ({', '.join(used[a][:3])})")
    for where in untranslated(root):
        problems.append(f"address not translated for the running release: {where}")
    print(f"{len(m.ranges)} ranges; {len(used)} addresses used by the port checked; {len(problems)} problems")
    for p in problems:
        print("  " + p)
    if "--check" in sys.argv:
        sys.exit(1 if problems else 0)
    with open(out, "w", newline="\n") as f:
        f.write("// Generated by tools/releases/pal_map.py -- do not edit.\n")
        f.write(f"// North American default.xbe MD5 {USA_MD5} -> PAL default.xbe (France MD5\n")
        f.write("// 84ae8a86018f3ab4a1b5756b238cefd2, Europe MD5 a48f65aa84a9848030b8644e1fd35b89: the same code)\n")
        f.write("// PAL_RANGE(first North American address, end, PAL address minus North American address)\n")
        for a, b, d in m.ranges:
            f.write(f"PAL_RANGE(0x{a:08X}, 0x{b:08X}, {d})\n")
    print(f"wrote {out}")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
