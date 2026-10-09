"""Music tracks (.hwx): list the slots, decode to WAV, encode a WAV to fit a slot, repoint a level's music.

    python tools/audio/hwx.py slots  <sounds>                       list the music tracks: rate, length
    python tools/audio/hwx.py decode <in.hwx> <out.wav> --sounds <sounds>
    python tools/audio/hwx.py encode <in.wav> <out.hwx> --sounds <sounds> [--fade 30]
    python tools/audio/hwx.py cues   <level.slp>                    list the tracks a level's music cues play
    python tools/audio/hwx.py repoint <level.slp> <out.slp> <old track> <new track>

<sounds> is the folder holding ep3.xb_sml and ep3.xb_wml, dumped from your own copy of the game
(dump\\audio\\xbox, see docs/modding/dumping-assets.md). A track is named by its file name without the
extension, e.g. mus_ui_mainmenu_lp.

A .hwx music file is headerless Xbox ADPCM, stereo: blocks of 36 bytes per channel (a 4-byte header of
predictor and step index, then 32 bytes of 4-bit codes), 65 samples per block, decoded as in
src/audio/audio.cpp. Its sample rate and length are not in the file but in the sound table (ep3.xb_wml),
and the game loops a track after the table's byte count, so a replacement must be exactly as long as the
original. `encode` trims or pads the WAV to that length. Only the Python standard library is needed.
"""
import argparse
import os
import struct
import sys
import wave

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
        88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
        658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
        16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
BLOCK = 36          # bytes per channel per block
SAMPLES = 65        # samples per channel per block
CHANNELS = 2


# --- sound table --------------------------------------------------------------------------------------

def load_table(sounds):
    """{track name: (rate, bytes)} for every music track named in ep3.xb_sml and found in ep3.xb_wml."""
    sml = open(os.path.join(sounds, "ep3.xb_sml"), "rb").read()
    wml = open(os.path.join(sounds, "ep3.xb_wml"), "rb").read()
    table = {}
    pos = 0
    while True:
        pos = sml.find(b"mus_", pos)
        if pos < 0:
            break
        end = sml.find(b"\0", pos)
        name = sml[pos:end].decode("ascii", "replace")
        key = sml[pos - 24:pos - 20]
        pos = end
        # The record is the occurrence of the key followed, 16 bytes on, by 0x020A.
        at = 0
        while True:
            at = wml.find(key, at)
            if at < 0:
                break
            rec = at + 16
            if wml[rec:rec + 2] == b"\x0a\x02":
                rate = struct.unpack_from("<H", wml, rec + 2)[0]
                size = struct.unpack_from("<I", wml, rec + 12)[0]
                table.setdefault(name.lower(), (rate, size))
                break
            at += 1
    return table


def lookup(sounds, path):
    name = os.path.splitext(os.path.basename(path))[0].lower()
    table = load_table(sounds)
    if name not in table:
        sys.exit(f"{name}: not a music track in the sound table (see `slots`)")
    return name, table[name]


def seconds(rate, size):
    return size // (BLOCK * CHANNELS) * SAMPLES / rate


# --- ADPCM --------------------------------------------------------------------------------------------

def decode(data):
    """Bytes to a list of interleaved stereo samples."""
    out = []
    p = 0
    for _ in range(len(data) // (BLOCK * CHANNELS)):
        pred = [0, 0]
        index = [0, 0]
        for c in range(CHANNELS):
            pred[c] = struct.unpack_from("<h", data, p)[0]
            index[c] = min(88, data[p + 2])
            p += 4
        out += pred
        for _group in range(8):
            decoded = [[0] * 8, [0] * 8]
            for c in range(CHANNELS):
                code = struct.unpack_from("<I", data, p)[0]
                p += 4
                pc, ic = pred[c], index[c]
                for j in range(8):
                    n = code & 15
                    code >>= 4
                    step = STEP[ic]
                    diff = step >> 3
                    if n & 4: diff += step
                    if n & 2: diff += step >> 1
                    if n & 1: diff += step >> 2
                    pc = max(-32768, min(32767, pc - diff if n & 8 else pc + diff))
                    ic = max(0, min(88, ic + INDEX[n]))
                    decoded[c][j] = pc
                pred[c], index[c] = pc, ic
            for j in range(8):
                out += (decoded[0][j], decoded[1][j])
    return out


def encode(samples):
    """Interleaved stereo samples (a whole number of blocks) to bytes."""
    out = bytearray()
    index = [0, 0]
    frames = len(samples) // CHANNELS
    for b in range(frames // SAMPLES):
        base = b * SAMPLES * CHANNELS
        pred = [samples[base], samples[base + 1]]
        for c in range(CHANNELS):
            out += struct.pack("<hBB", pred[c], index[c], 0)
        for group in range(8):
            for c in range(CHANNELS):
                pc, ic = pred[c], index[c]
                code = 0
                for j in range(8):
                    target = samples[base + (1 + group * 8 + j) * CHANNELS + c]
                    step = STEP[ic]
                    delta = target - pc
                    n = 8 if delta < 0 else 0
                    delta = abs(delta)
                    diff = step >> 3
                    if delta >= step: n |= 4; delta -= step; diff += step
                    if delta >= step >> 1: n |= 2; delta -= step >> 1; diff += step >> 1
                    if delta >= step >> 2: n |= 1; diff += step >> 2
                    pc = max(-32768, min(32767, pc - diff if n & 8 else pc + diff))
                    ic = max(0, min(88, ic + INDEX[n]))
                    code |= n << (4 * j)
                pred[c], index[c] = pc, ic
                out += struct.pack("<I", code)
    return bytes(out)


# --- commands -----------------------------------------------------------------------------------------

def cmd_slots(args):
    table = load_table(args.sounds)
    print(f"{'track':36} {'rate':>6} {'bytes':>9} {'length':>9}")
    for name in sorted(table):
        rate, size = table[name]
        s = seconds(rate, size)
        print(f"{name:36} {rate:6} {size:9} {int(s // 60)}:{s % 60:06.3f}")


def cmd_decode(args):
    name, (rate, size) = lookup(args.sounds, args.input)
    samples = decode(open(args.input, "rb").read())
    with wave.open(args.output, "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    print(f"{args.output}: {len(samples) // CHANNELS / rate:.3f} s at {rate} Hz")


def cmd_encode(args):
    name, (rate, size) = lookup(args.sounds, args.output)
    with wave.open(args.input, "rb") as w:
        if w.getnchannels() != CHANNELS or w.getsampwidth() != 2:
            sys.exit("the WAV must be 16-bit stereo")
        if w.getframerate() != rate:
            sys.exit(f"{name} plays at {rate} Hz: resample the WAV to {rate} Hz first "
                     f"(it is {w.getframerate()} Hz), or it will play at the wrong speed")
        raw = w.readframes(w.getnframes())
    samples = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    frames = size // (BLOCK * CHANNELS) * SAMPLES
    have = len(samples) // CHANNELS
    if have > frames:
        print(f"trimming {(have - frames) / rate:.3f} s from the end (the slot is {frames / rate:.3f} s)")
    elif have < frames:
        print(f"padding {(frames - have) / rate:.3f} s of silence (the slot is {frames / rate:.3f} s)")
    samples = (samples + [0] * (frames * CHANNELS))[:frames * CHANNELS]
    fade = int(args.fade / 1000 * rate)
    for i in range(fade):  # a short fade into the loop point, against a click
        g = (fade - i) / fade
        for c in range(CHANNELS):
            k = (frames - fade + i) * CHANNELS + c
            samples[k] = int(samples[k] * g)
    data = encode(samples)
    assert len(data) == size
    open(args.output, "wb").write(data)
    print(f"{args.output}: {len(data)} bytes, {frames / rate:.3f} s at {rate} Hz")


def cmd_cues(args):
    data = open(args.input, "rb").read()
    low = data.lower()
    found = {}
    at = low.find(b"music\\mus_")
    while at >= 0:
        end = at + 6
        while end < len(data) and 32 < data[end] < 127:
            end += 1
        name = data[at + 6:end].decode("ascii")
        found[name] = found.get(name, 0) + 1
        at = low.find(b"music\\mus_", end)
    for name, n in found.items():
        print(f"{name:36} {n} cue(s)")


def cmd_repoint(args):
    def full(name):
        name = os.path.splitext(name)[0]
        return "music\\" + (name if name.lower().startswith("mus_") else "mus_" + name)
    old = full(args.old).lower().encode("ascii")
    new = full(args.new).encode("ascii")
    if len(old) != len(new):
        sys.exit(f"{args.old} and {args.new} differ in length: the level stores names with their length, "
                 "so only a name of the same length can replace it")
    data = bytearray(open(args.input, "rb").read())
    low = bytes(data).lower()
    count = 0
    at = low.find(old)
    while at >= 0:
        # Keep the cue's own capitals where the names agree (ingameBattle21 -> ingameBattle02).
        cue = bytes(data[at:at + len(new)])
        data[at:at + len(new)] = bytes(c if chr(c).lower() == chr(n).lower() else n for c, n in zip(cue, new))
        count += 1
        at = low.find(old, at + len(old))
    if not count:
        sys.exit(f"{args.old}: not found in {args.input}")
    open(args.output, "wb").write(data)
    print(f"{args.output}: {count} cue(s) now play {args.new}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("slots", help="list the music tracks with their rate and length")
    p.add_argument("sounds")
    p.set_defaults(run=cmd_slots)
    p = sub.add_parser("decode", help="a track to WAV")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--sounds", required=True)
    p.set_defaults(run=cmd_decode)
    p = sub.add_parser("encode", help="a WAV to a track, fitted to the slot (named by the output file)")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--sounds", required=True)
    p.add_argument("--fade", type=float, default=30, help="fade-out at the loop point, in ms (default 30)")
    p.set_defaults(run=cmd_encode)
    p = sub.add_parser("cues", help="list the tracks a level's music cues play")
    p.add_argument("input")
    p.set_defaults(run=cmd_cues)
    p = sub.add_parser("repoint", help="make a level's music cues play another track of the same name length")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("old")
    p.add_argument("new")
    p.set_defaults(run=cmd_repoint)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
