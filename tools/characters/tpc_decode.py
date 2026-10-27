"""Decode a KOTOR/KOTOR2 TPC texture to an image (DXT1 / uncompressed 32-bit)."""
import struct
import sys
from pathlib import Path

from PIL import Image


def rgb565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode_dxt1(data, w, h):
    out = bytearray(w * h * 3)
    off = 0
    for by in range(h // 4):
        for bx in range(w // 4):
            c0, c1, bits = struct.unpack_from("<HHI", data, off)
            off += 8
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1:
                pal = [p0, p1,
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            else:
                pal = [p0, p1,
                       tuple((a + b) // 2 for a, b in zip(p0, p1)),
                       (0, 0, 0)]
            for i in range(16):
                x = bx * 4 + (i % 4)
                y = by * 4 + (i // 4)
                col = pal[(bits >> (2 * i)) & 3]
                p = (y * w + x) * 3
                out[p:p + 3] = bytes(col)
    return Image.frombytes("RGB", (w, h), bytes(out))


def load_tpc(path):
    d = Path(path).read_bytes()
    size, = struct.unpack_from("<I", d, 0x00)
    w, h = struct.unpack_from("<2H", d, 0x08)
    enc = d[0x0C]
    mips = d[0x0D]
    payload = d[0x80:0x80 + size]
    if enc == 2:
        img = decode_dxt1(payload, w, h)
    elif enc == 1 or enc == 0:
        # uncompressed BGRA
        img = Image.frombytes("RGBA", (w, h), payload[:w * h * 4])
        b, g, r, a = img.split()
        img = Image.merge("RGB", (r, g, b))
    else:
        raise ValueError("unsupported TPC encoding %d" % enc)
    return img, dict(width=w, height=h, encoding=enc, mips=mips, size=size)


if __name__ == "__main__":
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    img, meta = load_tpc(src)
    print(src.name, meta)
    if len(sys.argv) > 3 and sys.argv[3] == "flip":
        pass
    img.save(dst)
    print("wrote", dst, img.size)
