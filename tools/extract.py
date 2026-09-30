#!/usr/bin/env python3
"""Extract the code the recompiler needs from a CTR (USA, SCUS-94426) disc image.

Usage: python tools/extract.py <disc.cue|disc.bin> [outdir=data]

Writes:
  <outdir>/SCUS_944.26         main executable (PS-X EXE)
  <outdir>/ovr/<N>.bin         overlays 221..233 from BIGFILE.BIG
  <outdir>/disc.json           ISO directory (name -> lba, size)
"""
import json
import os
import struct
import sys

RAW = 2352

# overlay index -> load address (from CTR-ModSDK symbols/syms926.txt)
OVR_REGION1 = 0x8009F6FC  # 221-225 (end of race)
OVR_REGION2 = 0x800A0CB8  # 226-229 (LOD / level drawing)
OVR_REGION3 = 0x800AB9F0  # 230-233 (menu, race, adventure, cutscene)


def overlay_addr(i):
    if 221 <= i <= 225:
        return OVR_REGION1
    if 226 <= i <= 229:
        return OVR_REGION2
    if 230 <= i <= 233:
        return OVR_REGION3
    raise ValueError(i)


def resolve_bin(path):
    if path.lower().endswith(".cue"):
        with open(path) as f:
            for line in f:
                if line.strip().upper().startswith("FILE"):
                    return os.path.join(os.path.dirname(path), line.split('"')[1])
        raise SystemExit("no FILE in cue")
    return path


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")

    def sector(self, lba):
        self.f.seek(lba * RAW)
        return self.f.read(RAW)[24:24 + 2048]

    def read(self, lba, size):
        return b"".join(self.sector(lba + i) for i in range((size + 2047) // 2048))[:size]

    def walk(self, lba, size, path=""):
        buf = self.read(lba, size)
        i = 0
        while i < len(buf):
            n = buf[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = buf[i:i + n]
            elba, esize = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0]
            name = rec[33:33 + rec[32]]
            if name not in (b"\0", b"\1"):
                name = name.decode().split(";")[0]
                full = f"{path}/{name}" if path else name
                if rec[25] & 2:
                    yield from self.walk(elba, esize, full)
                else:
                    yield full, elba, esize
            i += n


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else "data"
    disc = Disc(resolve_bin(sys.argv[1]))
    pvd = disc.sector(16)
    assert pvd[1:6] == b"CD001", "not an ISO9660 image"
    assert pvd[40:50] == b"SCUS-94426", "expected CTR USA (SCUS-94426)"
    root = pvd[156:190]
    files = {n: (l, s) for n, l, s in disc.walk(struct.unpack("<I", root[2:6])[0],
                                               struct.unpack("<I", root[10:14])[0])}
    os.makedirs(os.path.join(out, "ovr"), exist_ok=True)
    with open(os.path.join(out, "disc.json"), "w") as f:
        json.dump(files, f, indent=1)

    lba, size = files["SCUS_944.26"]
    with open(os.path.join(out, "SCUS_944.26"), "wb") as f:
        f.write(disc.read(lba, size))

    big_lba, _ = files["BIGFILE.BIG"]
    hdr = disc.read(big_lba, 0x4000)
    count = struct.unpack("<I", hdr[4:8])[0]
    meta = {}
    for i in range(221, 234):
        off, sz = struct.unpack("<2I", hdr[8 + i * 8:16 + i * 8])
        assert i < count
        data = disc.read(big_lba + off, sz)
        with open(os.path.join(out, "ovr", f"{i}.bin"), "wb") as f:
            f.write(data)
        meta[i] = {"addr": overlay_addr(i), "size": sz}
        print(f"overlay {i}: {sz:#x} bytes @ {overlay_addr(i):#010x}")
    with open(os.path.join(out, "ovr", "overlays.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print("ok")


if __name__ == "__main__":
    main()
