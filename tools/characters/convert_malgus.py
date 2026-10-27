"""Convert the Movie Duels Malgus/Neimoidian Guard skin onto SWROTS Darth Vader."""
import os
import io
import struct
from pathlib import Path

import numpy as np
from PIL import Image

import sys
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "vendor"))
import fast_simplification
import ghoul2
import slayer_msh

WORK = HERE / "work"
MALGUS_DIR = WORK / "malgus"
GLA_PATH = WORK / "gla_galen" / "_humanoid.gla"
GAME = Path(os.environ.get("SWROTS_DIR", r"C:\Games\SWROTS"))
VADER_TEMPLATE = GAME / "dump" / "meshes" / "chars" / "darthvader"
OUT_CHAR = GAME / "mods" / "meshes" / "chars" / "darthvader"

SCALE = 1.10495 * 1.05
PALETTE_LIMIT = 19
PALETTE_SIZES = (4, 7, 11, 17, 26)
MAX_WEIGHTS = 3
ATLAS_SIZE = 1024
ATLAS_CELL = 256
AXES = np.array([[-1.0, 0, 0], [0, 0, 1.0], [0, -1.0, 0]])


def rgb565(c):
    r, g, b = (int(v) for v in c)
    return (r * 31 + 127) // 255 << 11 | (g * 63 + 127) // 255 << 5 | (b * 31 + 127) // 255


def unpack565(c):
    return np.array([(c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31], float)


def dxt5_alpha(alpha):
    a = np.clip(np.round(alpha.reshape(16)), 0, 255).astype(int)
    a0, a1 = int(a.max()), int(a.min())
    if a0 == a1:
        return bytes([a0, a1]) + bytes(6)
    pal = np.array([a0, a1] + [((6 - j) * a0 + (j + 1) * a1) / 7 for j in range(6)])
    idx = np.abs(a[:, None] - pal[None]).argmin(1)
    bits = 0
    for i in range(16):
        bits |= int(idx[i]) << (3 * i)
    return bytes([a0, a1]) + bits.to_bytes(6, "little")


def dxt5_block(px, alpha):
    abits = struct.unpack("<Q", dxt5_alpha(alpha))[0]
    p = px.reshape(16, 3).astype(float)
    if np.all(p == p[0]):
        c = rgb565(p[0])
        return struct.pack("<QHHI", abits, c, c, 0)
    mean = p.mean(0)
    axis = np.linalg.eigh(np.cov((p - mean).T))[1][:, -1]
    t = (p - mean) @ axis
    c0 = rgb565(np.clip(mean + axis * t.max(), 0, 255))
    c1 = rgb565(np.clip(mean + axis * t.min(), 0, 255))
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack("<QHHI", abits, c0, c1, 0)
    e0, e1 = unpack565(c0), unpack565(c1)
    pal = np.array([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3])
    idx = ((p[:, None, :] - pal[None]) ** 2).sum(2).argmin(1)
    bits = 0
    for i in range(16):
        bits |= int(idx[i]) << (2 * i)
    return struct.pack("<QHHI", abits, c0, c1, bits)


def write_stx(template_path, image, internal_name, out_path):
    hdr = bytearray(template_path.read_bytes()[:0x184])
    w, h = image.size
    struct.pack_into("<IIII", hdr, 0x0C, w, h, w, h)
    hdr[0x08] = 4
    hdr[0x30] = 3
    hdr[0x3C] = 0x52
    struct.pack_into("<I", hdr, 0x34, w * h + 0x80)
    hdr[0x40:0x60] = internal_name.encode("ascii").ljust(0x20, b"\0")
    px = np.asarray(image.convert("RGB"))
    lum = px.astype(float) @ np.array([0.299, 0.587, 0.114])
    alpha = np.clip(lum * 0.6, 0, 160)
    body = bytearray()
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            body += dxt5_block(px[y:y + 4, x:x + 4], alpha[y:y + 4, x:x + 4])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(hdr) + bytes(body))


def load_stx_image(path):
    data = path.read_bytes()
    w, h = struct.unpack_from("<II", data, 0x0C)
    dds = bytearray(124)
    struct.pack_into("<I", dds, 0, 124)
    struct.pack_into("<I", dds, 4, 0x81007)
    struct.pack_into("<II", dds, 8, h, w)
    struct.pack_into("<I", dds, 16, len(data) - 0x184)
    struct.pack_into("<I", dds, 72, 32)
    struct.pack_into("<I", dds, 76, 4)
    dds[80:84] = b"DXT5"
    struct.pack_into("<I", dds, 104, 0x1000)
    return Image.open(io.BytesIO(b"DDS " + dds + data[0x184:])).convert("RGB")


def vader_joints(chunk):
    names, pos = [], []
    for name, _parent, m12 in chunk.bones:
        r = np.array(m12[:9]).reshape(3, 3)
        names.append(name)
        pos.append(-(r @ np.array(m12[9:])))
    return names, np.array(pos)


def jka_joints(gla):
    names = [b[0] for b in gla]
    pos = np.array([[row[3] for row in rows] for _n, _p, rows in gla])
    return names, pos @ AXES.T * SCALE


def target_bone_for(jka_name):
    n = jka_name
    side = None
    if n[:2] in ("l_", "r_"):
        side, n = n[0], n[2:]
    elif n[:1] in ("l", "r") and n[1:] in (
            "femurYZ", "femurX", "tibia", "talus", "tarsal", "clavical", "humerus", "humerusX",
            "radius", "radiusX", "hand", "hang_tag_bone"):
        side, n = n[0], n[1:]
    S = {"l": "L", "r": "R"}.get(side)
    table = {
        "femurYZ": "hip", "femurX": "hip", "tibia": "knee", "talus": "ankle", "tarsal": "ankle",
        "clavical": "clav", "humerus": "shoulder", "humerusX": "shoulder", "radius": "elbow",
        "radiusX": "forearmTwist", "hand": "wrist", "hang_tag_bone": "wrist",
        "d1_j1": "thumb1", "d1_j2": "thumb1", "d1_j3": "thumb1",
        "d2_j1": "index1", "d2_j2": "index1", "d2_j3": "index1",
        "d3_j1": "middle1", "d3_j2": "middle1", "d3_j3": "middle1",
        "d4_j1": "middle1", "d4_j2": "middle1", "d4_j3": "middle1",
        "d5_j1": "middle1", "d5_j2": "middle1", "d5_j3": "middle1",
    }
    if S and n in table:
        return "%s_%s_g" % (table[n], S)
    return {
        "pelvis": "root_pelvis_g", "lower_lumbar": "spine1_g", "upper_lumbar": "spine2_g",
        "thoracic": "spine3_g", "cervical": "neck_g", "cranium": "head_g", "face": "head_g",
        "mc5": "wrist_L_g", "mc7": "wrist_R_g", "model_root": "root_pelvis_g", "Motion": "root_pelvis_g",
    }.get(jka_name, "head_g")


def rotation_between(a, b):
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(a @ b)
    if np.linalg.norm(v) < 1e-8:
        return np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


class Segment:
    def __init__(self, jp, vp, rot=np.eye(3), axis=None, stretch=1.0):
        self.jp, self.vp, self.rot = jp, vp, rot
        self.axis, self.stretch = axis, stretch

    def apply(self, x):
        local = x - self.jp
        if self.axis is not None and self.stretch != 1.0:
            along = local @ self.axis
            local = local + np.outer(along * (self.stretch - 1.0), self.axis)
        return local @ self.rot.T + self.vp


def build_segments(jn, jpos, tn, tpos):
    J = lambda n: jpos[jn.index(n)]
    V = lambda n: tpos[tn.index(n)]
    seg = {}

    def chain(ja, jb, ta, tb, clamp=(0.75, 1.6)):
        d, dv = J(jb) - J(ja), V(tb) - V(ta)
        rot = rotation_between(d, dv)
        stretch = float(np.clip(np.linalg.norm(dv) / np.linalg.norm(d), *clamp))
        return Segment(J(ja), V(ta), rot, d / np.linalg.norm(d), stretch)

    up = np.array([0.0, 1.0, 0.0])
    torso_k = (V("shoulder_R_g")[1] - V("root_pelvis_g")[1]) / (J("rhumerus")[1] - J("pelvis")[1])
    torso = Segment(J("pelvis"), V("root_pelvis_g"), np.eye(3), up, torso_k)
    for n in ("pelvis", "lower_lumbar", "upper_lumbar", "thoracic", "Motion", "model_root"):
        seg[n] = torso
    head = Segment(J("cervical"), V("neck_g"))
    for n in jn:
        seg.setdefault(n, head)
    for s, S in (("l", "L"), ("r", "R")):
        femur = chain(s + "femurYZ", s + "tibia", "hip_%s_g" % S, "knee_%s_g" % S)
        tibia = chain(s + "tibia", s + "talus", "knee_%s_g" % S, "ankle_%s_g" % S)
        talus = Segment(J(s + "talus"), V("ankle_%s_g" % S))
        seg[s + "femurYZ"] = seg[s + "femurX"] = femur
        seg[s + "tibia"] = tibia
        seg[s + "talus"] = talus
        seg[s + "clavical"] = Segment(J(s + "clavical"), V("clav_%s_g" % S))
        humerus = chain(s + "humerus", s + "radius", "shoulder_%s_g" % S, "elbow_%s_g" % S)
        radius = chain(s + "radius", s + "hand", "elbow_%s_g" % S, "wrist_%s_g" % S)
        finger = s + "_d3_j1" if s + "_d3_j1" in jn else s + "_d2_j1"
        ak_f = ("middle1_%s_g" if s + "_d3_j1" in jn else "index1_%s_g") % S
        if ak_f not in tn:
            # The simplified brute-style rigs (Maul, 33 bones) only carry middle1/thumb.
            ak_f = "middle1_%s_g" % S
        hand = chain(s + "hand", finger, "wrist_%s_g" % S, ak_f, (0.85, 1.25))
        seg[s + "humerus"] = seg[s + "humerusX"] = humerus
        seg[s + "radius"] = seg[s + "radiusX"] = radius
        for n in jn:
            if n == s + "hand" or n.startswith(s + "_d") or n == s + "hang_tag_bone":
                seg[n] = hand
        seg["mc5" if s == "l" else "mc7"] = hand
    return seg


def tris_to_strip(tris):
    out = []
    for a, b, c in tris:
        if out:
            out += [out[-1], a]
            if len(out) % 2 == 1:
                out.append(a)
        out += [a, b, c]
    return out


def quantize_weights(links):
    total = sum(w for _, w in links) or 1.0
    q = [int(round(255 * w / total)) for _, w in links]
    q[0] += 255 - sum(q)
    return [(b, w) for (b, _), w in zip(links, q) if w > 0]


def partition(tris, verts, limit):
    groups = []
    order = sorted(tris, key=lambda t: sum(verts[k]["pos"][0] for k in t))
    for t in order:
        bones = {b for k in t for b, _ in verts[k]["links"]}
        for g in groups:
            if len(g["bones"] | bones) <= limit and len(g["tris"]) < 250:
                g["bones"] |= bones
                g["tris"].append(t)
                break
        else:
            groups.append({"bones": set(bones), "tris": [t]})
    return groups


def encode_vertex(v, palette, stride, attrs):
    buf = bytearray(stride)
    if attrs[0] != 255:
        struct.pack_into("<3f", buf, attrs[0], *v["pos"])
    if attrs[1] != 255:
        n = np.clip(np.round(v["nrm"] * 127 + 128), 0, 255).astype(int)
        buf[attrs[1]:attrs[1] + 3] = bytes(n.tolist())
    if attrs[3] != 255:
        struct.pack_into("<2f", buf, attrs[3], *v["uv"])
    if attrs[6] != 255 and attrs[7] != 255:
        links = quantize_weights(v["links"])
        for k, (b, w) in enumerate(links):
            buf[attrs[6] + k] = w
            buf[attrs[7] + k] = palette.index(b) * 3
    return bytes(buf)


def fill_object(obj, draws, verts, sign):
    template = obj.subs[0]
    vdata, remap, subs = bytearray(), {}, []
    for palette, tris in draws:
        key = tuple(palette)
        fixed, sub_indices = [], set()
        for tri in tris:
            a, b, c = tri
            P = [verts[k]["pos"] for k in tri]
            n = sum(verts[k]["nrm"] for k in tri)
            if float(np.cross(P[1] - P[0], P[2] - P[0]) @ n) * sign < 0:
                tri = (a, c, b)
            for k in tri:
                if (k, key) not in remap:
                    remap[(k, key)] = len(vdata) // obj.stride
                    vdata += encode_vertex(verts[k], palette, obj.stride, obj.attrs)
                sub_indices.add(remap[(k, key)])
            fixed.append(tuple(remap[(k, key)] for k in tri))
        sub = slayer_msh.Sub()
        sub.pre = struct.pack("<I", next(n for n in PALETTE_SIZES if n >= len(palette)))
        sub.refs = list(palette)
        sub.indices = tris_to_strip(fixed)
        sub.mid = struct.pack("<I", len(sub.indices))
        sub.is_strip = 1
        sub.post = bytearray(template.post)
        struct.pack_into("<HH", sub.post, 4, len(sub_indices), len(fixed))
        subs.append(sub)
    obj.vdata, obj.subs = bytes(vdata), subs


def empty_object(obj, bone):
    v = dict(pos=np.zeros(3), nrm=np.array([0.0, 1.0, 0.0]), uv=(0.0, 0.0), links=[(bone, 1.0)])
    template = obj.subs[0]
    obj.vdata = encode_vertex(v, [bone], obj.stride, obj.attrs) * 3
    sub = slayer_msh.Sub()
    sub.pre = struct.pack("<I", PALETTE_SIZES[0])
    sub.refs, sub.indices, sub.mid, sub.is_strip = [bone], [0, 1, 2], struct.pack("<I", 3), 1
    sub.post = bytearray(template.post)
    struct.pack_into("<HH", sub.post, 4, 3, 1)
    obj.subs = [sub]


def with_triangle_total(group):
    total = 0
    for obj in group.objs:
        for s in obj.subs:
            ix = s.indices
            total += sum(1 for i in range(len(ix) - 2) if len({ix[i], ix[i + 1], ix[i + 2]}) == 3)
    header = bytearray(group.header)
    struct.pack_into("<I", header, 0x48, total)
    return bytes(header)


def read_skin():
    skin = {}
    for line in (MALGUS_DIR / "model_default.skin").read_text().splitlines():
        if "," in line and not line.strip().startswith("//"):
            surface, tex = line.strip().split(",", 1)
            tex = Path(tex.strip()).stem.lower()
            if tex.startswith("*"):
                continue
            skin[surface.strip()] = tex
    return skin


def atlas_layout(skin):
    categories = {"head": set(), "body": set()}
    for surface, tex in skin.items():
        if surface.startswith("*") or surface.endswith("_off") or surface == "torso_cape":
            continue
        categories["head" if surface in ("head", "head_eyes", "helm") else "body"].add(tex)
    images, layouts = {}, {"head": {}, "body": {}}
    lookup = {p.stem.lower(): p for p in MALGUS_DIR.iterdir() if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".tga")}
    for category, names in categories.items():
        if len(names) > 16:
            raise ValueError("Too many source textures for the 4x4 atlas: " + category)
        atlas = Image.new("RGB", (ATLAS_SIZE, ATLAS_SIZE), (0, 0, 0))
        for i, name in enumerate(sorted(names)):
            if name not in lookup:
                raise FileNotFoundError("Missing Movie Duels texture: " + name)
            col, row = i % 4, i // 4
            image = Image.open(lookup[name]).convert("RGB")
            # Keep the source texture's aspect ratio. Stretching the 2:1 JKA
            # atlases into square cells maps unrelated regions onto the model.
            scale = min(ATLAS_CELL / image.width, ATLAS_CELL / image.height)
            width = max(1, round(image.width * scale))
            height = max(1, round(image.height * scale))
            image = image.resize((width, height), Image.Resampling.LANCZOS)
            x = col * ATLAS_CELL + (ATLAS_CELL - width) // 2
            y = row * ATLAS_CELL + (ATLAS_CELL - height) // 2
            atlas.paste(image, (x, y))
            layouts[category][name] = (x / ATLAS_SIZE, y / ATLAS_SIZE,
                                       width / ATLAS_SIZE, height / ATLAS_SIZE)
        images[category] = atlas
    return layouts, images


def load_malgus(jn, segs, target_bone_index, skin, layouts):
    categories = {k: ([], []) for k in ("hands", "upper", "center", "lower", "head")}
    for surf in ghoul2.read_glm(MALGUS_DIR / "model.glm"):
        name = surf["name"]
        if (surf["flags"] or name == "stupidtriangle" or name.endswith("_off") or
                name.startswith("*") or name == "torso_cape"):
            continue
        tex = skin.get(name)
        if not tex:
            continue
        if name in ("head", "head_eyes", "head_mask", "mask_piece", "head_hood", "head2", "head2_eyes", "head2_mouth", "head2_neck"):
            category, atlas_category = "head", "head"
        elif name in ("l_hand", "r_hand"):
            category, atlas_category = "hands", "body"
        elif name == "hips_belt_piece":
            category, atlas_category = "center", "body"
        elif name in ("hips", "l_leg", "r_leg", "l_leg_armor", "r_leg_armor"):
            category, atlas_category = "lower", "body"
        else:
            category, atlas_category = "upper", "body"
        verts, tris = categories[category]
        base = len(verts)
        P = np.array(surf["verts"]) @ AXES.T * SCALE
        N = np.array(surf["normals"]) @ AXES.T
        u0, v0, uw, vh = layouts[atlas_category][tex]
        for i, source_links in enumerate(surf["weights"]):
            pos, nrm, out_links = np.zeros(3), np.zeros(3), {}
            for bone, weight in source_links:
                source_bone = jn[bone]
                segment = segs[source_bone]
                pos += weight * segment.apply(P[i][None])[0]
                nrm += weight * (segment.rot @ N[i])
                target_index = target_bone_index[target_bone_for(source_bone)]
                out_links[target_index] = out_links.get(target_index, 0.0) + weight
            nrm /= np.linalg.norm(nrm) or 1.0
            u, v = surf["uvs"][i]
            uv = (u0 + (u % 1.0) * uw, v0 + (v % 1.0) * vh)
            verts.append(dict(pos=pos, nrm=nrm, uv=uv, links=sorted(out_links.items(), key=lambda x: -x[1])[:MAX_WEIGHTS]))
        for a, b, c in surf["tris"]:
            tris.append((base + a, base + c, base + b))
    return categories


def simplify_category(vertices, triangles, target_count):
    if len(triangles) <= target_count:
        return vertices, triangles
    points = np.asarray([v["pos"] for v in vertices], dtype=np.float64)
    faces = np.asarray(triangles, dtype=np.int32)
    simp_points, simp_faces = fast_simplification.simplify(points, faces, target_count=target_count, agg=7.0)
    used = np.unique(simp_faces.reshape(-1))
    compact = np.full(len(simp_points), -1, dtype=np.int32)
    compact[used] = np.arange(len(used), dtype=np.int32)
    simp_faces = compact[simp_faces]
    simp_points = simp_points[used]
    source_ids = []
    for first in range(0, len(simp_points), 128):
        d2 = ((simp_points[first:first + 128, None, :] - points[None, :, :]) ** 2).sum(axis=2)
        source_ids.extend(d2.argmin(axis=1).tolist())
    reduced = []
    for pos, source_i in zip(simp_points, source_ids):
        v = dict(vertices[source_i])
        v["pos"] = pos
        reduced.append(v)
    return reduced, [tuple(map(int, f)) for f in simp_faces]


def patch_texture_names(group):
    data = bytearray(group.tail)
    buf_len = struct.unpack_from("<I", data, 8)[0]
    count = struct.unpack_from("<I", data, 16)[0]
    offs = struct.unpack_from("<%dI" % count, data, 20)
    base = 20 + 4 * count
    names = [data[base + off:].split(b"\0", 1)[0].decode("ascii") for off in offs]
    # The original cape is remapped into a tile in VaderBody's atlas below.
    replacements = {0: "VaderBody", 4: "VaderBody", 10: "VaderBody"}
    for index, value in replacements.items():
        off = offs[index]
        old = names[index].encode("ascii") + b"\0"
        new = value.encode("ascii") + b"\0"
        if len(old) != len(new):
            raise ValueError("Replacement material name must retain string-table size")
        data[base + off:base + off + len(old)] = new
    group.tail = bytes(data)
    cape = group.objs[5]
    uv_offset = cape.attrs[3]
    vdata = bytearray(cape.vdata)
    for i in range(len(vdata) // cape.stride):
        u, v = struct.unpack_from("<2f", vdata, i * cape.stride + uv_offset)
        struct.pack_into("<2f", vdata, i * cape.stride + uv_offset,
                         0.5 + (u % 1.0) * 0.25, 0.5 + (v % 1.0) * 0.25)
    cape.vdata = bytes(vdata)


def build_textures(images):
    cape_image = load_stx_image(VADER_TEMPLATE / "vaderhead.stx").resize(
        (ATLAS_CELL, ATLAS_CELL), Image.Resampling.LANCZOS)
    images["body"].paste(cape_image, (2 * ATLAS_CELL, 2 * ATLAS_CELL))
    write_stx(VADER_TEMPLATE / "vaderbody.stx", images["body"], "VaderBody", OUT_CHAR / "vaderbody.stx")
    write_stx(VADER_TEMPLATE / "vaderhead.stx", images["head"], "VaderHead", OUT_CHAR / "vaderhead.stx")


def main():
    print("Loading stock Vader mesh and skeleton...")
    mesh = slayer_msh.parse((VADER_TEMPLATE / "darthvader.msh").read_bytes())
    chunk = mesh.chunks[0]
    target_names, target_positions = vader_joints(chunk)
    target_bone_index = {name: i for i, name in enumerate(target_names)}
    for n in ("root_pelvis_g", "shoulder_R_g", "head_g", "wrist_L_g", "wrist_R_g"):
        if n not in target_bone_index:
            raise ValueError("Vader skeleton missing required bone " + n)

    gla = ghoul2.read_gla_bones(GLA_PATH)
    source_names, source_positions = jka_joints(gla)
    segments = build_segments(source_names, source_positions, target_names, target_positions)
    skin = read_skin()
    layouts, images = atlas_layout(skin)
    cats = load_malgus(source_names, segments, target_bone_index, skin, layouts)

    targets = {"hands": 450, "upper": 1250, "center": 180, "lower": 1100, "head": 1200}
    for name in cats:
        cats[name] = simplify_category(*cats[name], targets[name])
    print("Reduced triangles:", ", ".join("%s=%d" % (k, len(v[1])) for k, v in cats.items()))

    g0 = chunk.groups[0]
    destinations = (("hands", 0), ("upper", 1), ("center", 2), ("head", 3), ("lower", 4))
    for cat, obj_index in destinations:
        verts, tris = cats[cat]
        if tris:
            draws = partition(tris, verts, PALETTE_LIMIT)
            fill_object(g0.objs[obj_index], [(sorted(d["bones"]), d["tris"]) for d in draws], verts, -1)
        else:
            empty_object(g0.objs[obj_index], target_bone_index["root_pelvis_g"])

    # Keep Vader's own dynamic cape draw and the stock shadow group intact.
    g0.header = with_triangle_total(g0)
    patch_texture_names(g0)
    out_path = OUT_CHAR / "darthvader.msh"
    OUT_CHAR.mkdir(parents=True, exist_ok=True)
    out_bytes = slayer_msh.build(mesh)
    out_path.write_bytes(out_bytes)
    assert slayer_msh.build(slayer_msh.parse(out_bytes)) == out_bytes
    build_textures(images)
    print("Wrote", out_path, "(", len(out_bytes), "bytes )")


if __name__ == "__main__":
    main()
