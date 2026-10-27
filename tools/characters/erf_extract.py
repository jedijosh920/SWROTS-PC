"""Extract resources from a BioWare ERF archive (random access; handles multi-GB packs)."""
import struct
import sys
from pathlib import Path


class ErfFile:
    def __init__(self, path):
        self.path = Path(path)
        with self.path.open("rb") as f:
            head = f.read(0x40)
        self.magic = head[0:4].decode("ascii", "replace")
        self.version = head[4:8].decode("ascii", "replace")
        self.count, = struct.unpack_from("<I", head, 0x10)
        self.key_off, = struct.unpack_from("<I", head, 0x18)
        self.res_off, = struct.unpack_from("<I", head, 0x1C)

    def keys(self):
        with self.path.open("rb") as f:
            f.seek(self.key_off)
            keys = f.read(self.count * 24)
            f.seek(self.res_off)
            res = f.read(self.count * 8)
        for i in range(self.count):
            b = keys[i * 24:(i + 1) * 24]
            ref = b[0:16].split(b"\0")[0].decode("ascii", "replace")
            rtype, = struct.unpack_from("<H", b, 20)
            off, size = struct.unpack_from("<2I", res, i * 8)
            yield ref, rtype, off, size

    def read(self, ref, rtype=None):
        want = ref.lower()
        for r, t, off, size in self.keys():
            if r.lower() == want and (rtype is None or t == rtype):
                with self.path.open("rb") as f:
                    f.seek(off)
                    return f.read(size)
        return None


if __name__ == "__main__":
    erf = ErfFile(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else "list"
    if mode == "list":
        pat = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for ref, t, off, size in sorted(erf.keys(), key=lambda x: x[0]):
            if pat in ref.lower():
                print("   %-22s type=%-5d size=%d" % (ref, t, size))
    else:
        out = Path(sys.argv[3])
        data = erf.read(sys.argv[2])
        if data is None:
            print("NOT FOUND")
        else:
            out.write_bytes(data)
            print("wrote", out, len(data), "bytes")
            print("first 0x90:", data[:0x90].hex())
