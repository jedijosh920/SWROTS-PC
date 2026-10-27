"""Read and rebuild skinned Slayer-engine character meshes (.msh, ROTS Xbox).

The layout follows the walk of the Slayer toolkit importer (io_import_swe3_msh.parse_msh).
Every byte that is not understood is kept as an opaque span, so a parsed file
rebuilds byte for byte; only vertex buffers, index buffers and bone palettes
are meant to be replaced.
"""
import struct

MAGIC = b"\xa1\x03\xc2\xf8"


class Sub:
    """One draw call of an object: a bone palette and an index buffer."""

    def __init__(self):
        self.pre = b""        # 4 bytes before the palette count
        self.refs = []        # bone palette (indices into the chunk's bones)
        self.mid = b""        # 4 bytes between palette and index count
        self.indices = []
        self.is_strip = 0
        self.post = b""       # bytes after is_strip (flag dependent)


class Obj:
    def __init__(self):
        self.header = bytearray()  # everything before vcnt (header, attrs, pad, stride)
        self.stride = 0
        self.attrs = []
        self.vdata = b""
        self.gap = b""             # 4 bytes after the vertices
        self.subs = []


class Group:
    def __init__(self):
        self.header = b""    # 0x5C + 4 bytes
        self.objs = []
        self.tail = b""      # string blocks, materials (copied)


class Chunk:
    def __init__(self):
        self.header = b""
        self.groups = []
        self.bones_pre = b""
        self.bones = []      # (name, parent, m12)
        self.bones_raw = []  # raw bone records (0x20 name + 48 + 2)
        self.trailer = b""


class Msh:
    def __init__(self):
        self.prefix = b""
        self.flag = 0
        self.head = b""      # from MESH to the end of the size table + 8
        self.chunks = []
        self.suffix = b""


def _u32(d, p):
    return struct.unpack_from("<I", d, p)[0]


def _string_block_end(d, p):
    buf_len = _u32(d, p + 8)
    count = _u32(d, p + 16)
    return p + 20 + 4 * count + buf_len


def parse(data):
    m = Msh()
    start = data.find(MAGIC)
    if start < 0:
        raise ValueError("no mesh magic")
    m.prefix = data[:start]
    p = start
    m.flag = _u32(data, p + 4)
    chunk_count = _u32(data, p + 12)
    p += 16
    group_num = []
    for _ in range(chunk_count):
        ln = _u32(data, p)
        p += 4 + ln + 4
        cnt = _u32(data, p)
        p += 4
        for _ in range(cnt):
            sub = _u32(data, p)
            p += 4 + sub
        group_num.append(cnt)
    p += 8
    m.head = data[start:p]

    for ci in range(chunk_count):
        ch = Chunk()
        q = p
        ln = _u32(data, p)
        p += 4 + ln + 8
        ch.header = data[q:p]
        for _g in range(group_num[ci]):
            g = Group()
            q = p
            p += 0x5C + 4
            object_num = _u32(data, p)
            p += 4
            g.header = data[q:p]
            for _o in range(object_num):
                o = Obj()
                q = p
                p += 4 + 0x49 + 0x16
                o.attrs = list(data[p:p + 0xB])
                p += 0xB + 1
                o.stride = data[p]
                p += 1
                o.header = bytearray(data[q:p])
                vcnt = _u32(data, p)
                p += 4
                o.vdata = data[p:p + vcnt * o.stride]
                p += vcnt * o.stride
                o.gap = data[p:p + 4]
                p += 4
                sub_count = _u32(data, p)
                p += 4
                for _k in range(sub_count):
                    s = Sub()
                    s.pre = data[p:p + 4]
                    p += 4
                    nrefs = _u32(data, p)
                    p += 4
                    s.refs = list(struct.unpack_from("<%dh" % nrefs, data, p))
                    p += 2 * nrefs
                    s.mid = data[p:p + 4]
                    p += 4
                    icnt = _u32(data, p)
                    p += 4
                    s.indices = list(struct.unpack_from("<%dH" % icnt, data, p))
                    p += 2 * icnt
                    s.is_strip = _u32(data, p)
                    p += 4
                    q = p
                    p += 4 + 2
                    if m.flag == 0x90019:
                        p += 4
                    elif m.flag < 0x90012 or m.flag > 0x90014:
                        p += 6
                    s.post = data[q:p]
                    o.subs.append(s)
                g.objs.append(o)
            q = p
            p = _string_block_end(data, p)
            p = _string_block_end(data, p)
            p += 0xC
            mat_num = _u32(data, p)
            p += 4 + 0x38 * mat_num + 0x20
            g.tail = data[q:p]
            ch.groups.append(g)
        q = p
        p += 8
        bone_count = _u32(data, p)
        p += 4
        ch.bones_pre = data[q:p]
        for _ in range(bone_count):
            rec = data[p:p + 0x20 + 48 + 2]
            name = rec[:0x20].split(b"\0")[0].decode("ascii", "replace")
            m12 = struct.unpack_from("<12f", rec, 0x20)
            parent = struct.unpack_from("<h", rec, 0x50)[0]
            ch.bones.append((name, parent, m12))
            ch.bones_raw.append(rec)
            p += len(rec)
        ch.trailer = data[p:p + 0x68]
        p += 0x68
        m.chunks.append(ch)
    m.suffix = data[p:]
    return m


def build(m):
    out = bytearray(m.prefix)
    out += m.head
    for ch in m.chunks:
        out += ch.header
        for g in ch.groups:
            out += g.header
            for o in g.objs:
                out += o.header
                out += struct.pack("<I", len(o.vdata) // o.stride)
                out += o.vdata
                out += o.gap
                out += struct.pack("<I", len(o.subs))
                for s in o.subs:
                    out += s.pre
                    out += struct.pack("<I", len(s.refs))
                    out += struct.pack("<%dh" % len(s.refs), *s.refs)
                    out += s.mid
                    out += struct.pack("<I", len(s.indices))
                    out += struct.pack("<%dH" % len(s.indices), *s.indices)
                    out += struct.pack("<I", s.is_strip)
                    out += s.post
            out += g.tail
        out += ch.bones_pre
        for rec in ch.bones_raw:
            out += rec
        out += ch.trailer
    out += m.suffix
    return bytes(out)
