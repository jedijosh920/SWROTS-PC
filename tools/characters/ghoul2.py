"""Minimal reader for Jedi Knight II Ghoul2 models (.glm, LOD 0) and skeletons (.gla)."""
import struct

WEIGHT_SCALE = 1.0 / 1023.0


def _cstr(b):
    return b.split(b"\0")[0].decode("ascii", "replace")


def read_gla_bones(path):
    """[(name, parent, base_pose 3x4 rows)] from a .gla skeleton."""
    d = open(path, "rb").read()
    if d[:4] != b"2LGA":
        raise ValueError("not a GLA file")
    num_bones = struct.unpack_from("<i", d, 4 + 4 + 64 + 4 + 4 + 4)[0]
    header_size = 4 + 4 + 64 + 4 + 4 + 4 + 4 + 4 + 4 + 4
    offs = struct.unpack_from("<%di" % num_bones, d, header_size)
    bones = []
    for o in offs:
        p = header_size + o
        name = _cstr(d[p:p + 64])
        _flags, parent = struct.unpack_from("<Ii", d, p + 64)
        base = struct.unpack_from("<12f", d, p + 72)
        rows = [base[0:4], base[4:8], base[8:12]]
        bones.append((name, parent, rows))
    return bones


def read_glm(path):
    """Surfaces of LOD 0: [{name, shader, verts, normals, uvs, weights, tris}].

    weights is per vertex a list of (gla bone index, weight)."""
    d = open(path, "rb").read()
    if d[:4] != b"2LGM":
        raise ValueError("not a GLM file")
    p = 8 + 64 + 64
    _anim_index, _num_bones, _num_lods, ofs_lods, num_surfaces, ofs_hier, _ofs_end = \
        struct.unpack_from("<7i", d, p)

    header_size = p + 7 * 4
    hier_offs = struct.unpack_from("<%di" % num_surfaces, d, header_size)
    hier = []
    for o in hier_offs:
        q = header_size + o
        name = _cstr(d[q:q + 64])
        flags = struct.unpack_from("<I", d, q + 64)[0]
        shader = _cstr(d[q + 68:q + 132])
        hier.append((name, flags, shader))

    lod = ofs_lods
    surf_offs = struct.unpack_from("<%di" % num_surfaces, d, lod + 4)
    surfaces = []
    for si, so in enumerate(surf_offs):
        s = lod + 4 + so
        (_ident, index, _ofs_header, num_verts, ofs_verts, num_tris, ofs_tris,
         num_refs, ofs_refs, _ofs_end) = struct.unpack_from("<10i", d, s)
        refs = struct.unpack_from("<%di" % num_refs, d, s + ofs_refs)
        tris = [struct.unpack_from("<3i", d, s + ofs_tris + 12 * t) for t in range(num_tris)]
        verts, normals, weights = [], [], []
        for v in range(num_verts):
            q = s + ofs_verts + 32 * v
            n = struct.unpack_from("<3f", d, q)
            x = struct.unpack_from("<3f", d, q + 12)
            packed = struct.unpack_from("<I", d, q + 24)[0]
            low = d[q + 28:q + 32]
            count = ((packed >> 30) & 3) + 1
            links, total = [], 0.0
            for k in range(count):
                bone = refs[(packed >> (5 * k)) & 31]
                if k == count - 1:
                    w = 1.0 - total
                else:
                    raw = low[k] | ((packed >> (12 + 2 * k)) & 0x300)
                    w = raw * WEIGHT_SCALE
                    total += w
                links.append((bone, w))
            verts.append(x)
            normals.append(n)
            weights.append(links)
        uv_at = s + ofs_verts + 32 * num_verts
        uvs = [struct.unpack_from("<2f", d, uv_at + 8 * v) for v in range(num_verts)]
        name, flags, shader = hier[index]
        surfaces.append(dict(name=name, flags=flags, shader=shader, verts=verts,
                             normals=normals, uvs=uvs, weights=weights, tris=tris))
    return surfaces
