"""Minimal BioWare GFF reader: dump the fields of a KOTOR/KOTOR2 .ifo / .res file."""
import struct
import sys
from pathlib import Path

TYPES = {0: "byte", 1: "char", 2: "word", 3: "short", 4: "dword", 5: "int", 6: "float",
         7: "double", 8: "string", 9: "resref", 10: "strref", 11: "vector", 12: "struct",
         13: "list", 14: "orientation", 15: "void"}


class Gff:
    def __init__(self, data):
        self.d = data
        (self.file_type, self.file_version) = (data[0:4].decode("ascii", "replace"),
                                               data[4:8].decode("ascii", "replace"))
        (self.struct_off, self.struct_cnt, self.field_off, self.field_cnt, self.label_off,
         self.label_cnt, self.fdat_off, self.fdat_cnt, self.find_off, self.find_cnt,
         self.lind_off, self.lind_cnt) = struct.unpack_from("<12I", data, 8)

        self.labels = []
        for i in range(self.label_cnt):
            raw = data[self.label_off + i * 16: self.label_off + i * 16 + 16]
            self.labels.append(raw.split(b"\0")[0].decode("ascii", "replace"))

    def field(self, i):
        t, label, data = struct.unpack_from("<3I", self.d, self.field_off + i * 12)
        return t, label, data

    def struct(self, i):
        _t, data, count = struct.unpack_from("<3I", self.d, self.struct_off + i * 12)
        return data, count   # field index start, field count

    def value(self, t, data):
        if t in (0, 1):
            return self.d[self.fdat_off + data]
        if t == 2:
            return struct.unpack_from("<H", self.d, self.fdat_off + data)[0]
        if t == 3:
            return struct.unpack_from("<h", self.d, self.fdat_off + data)[0]
        if t in (4, 5):
            return struct.unpack_from("<i" if t == 5 else "<I", self.d, self.fdat_off + data)[0]
        if t == 6:
            return struct.unpack_from("<f", self.d, self.fdat_off + data)[0]
        if t == 7:
            return struct.unpack_from("<d", self.d, self.fdat_off + data)[0]
        if t in (8, 9):
            off = self.fdat_off + data
            n = self.d[off]
            return self.d[off + 1: off + 1 + n].decode("ascii", "replace")
        if t == 10:
            return struct.unpack_from("<I", self.d, self.fdat_off + data)[0]
        if t in (11, 14):
            return struct.unpack_from("<4f", self.d, self.fdat_off + data)
        return None

    def walk(self, struct_idx, depth=0, want=None, out=None):
        if out is None:
            out = {}
        start, count = self.struct(struct_idx)
        for i in range(start, start + count):
            t, label_id, data = self.field(i)
            name = self.labels[label_id] if label_id < len(self.labels) else "?"
            if want and name not in want:
                continue
            if t == 12:
                self.walk(data, depth + 1, want, out)
            elif t == 13:
                n = struct.unpack_from("<I", self.d, self.lind_off + data * 4)[0]
                items = []
                for k in range(n):
                    si = struct.unpack_from("<I", self.d, self.lind_off + (data + 1 + k) * 4)[0]
                    items.append(self.walk(si, depth + 1, want, {}))
                out[name] = items
            else:
                out[name] = self.value(t, data)
        return out


if __name__ == "__main__":
    path = Path(sys.argv[1])
    g = Gff(path.read_bytes())
    print("file:", path.name, "|", g.file_type, g.file_version,
          "| structs", g.struct_cnt, "fields", g.field_cnt, "labels", g.label_cnt)
    wanted = set(sys.argv[2:]) or None
    for k, v in g.walk(0, want=wanted).items():
        if isinstance(v, str) and len(v) > 60:
            v = v[:60] + "..."
        print("  %-24s %r" % (k, v))
