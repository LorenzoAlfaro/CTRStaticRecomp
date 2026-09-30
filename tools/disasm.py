#!/usr/bin/env python3
"""Disassemble a range of the CTR executable or an overlay.

Usage: python tools/disasm.py <start_hex> <end_hex> [overlay_index]
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "gen"))
from mips import Insn  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..")


def load(ovr=None):
    if ovr is None:
        d = open(os.path.join(ROOT, "data", "SCUS_944.26"), "rb").read()
        return 0x80010000, d[0x800:]
    meta = json.load(open(os.path.join(ROOT, "data", "ovr", "overlays.json")))
    return meta[str(ovr)]["addr"], open(os.path.join(ROOT, "data", "ovr", f"{ovr}.bin"), "rb").read()


def main():
    start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
    base, data = load(int(sys.argv[3]) if len(sys.argv) > 3 else None)
    for a in range(start, end, 4):
        w = struct.unpack_from("<I", data, a - base)[0]
        print(f"{a:08x}: {w:08x}  {Insn(a, w)}")


if __name__ == "__main__":
    main()
