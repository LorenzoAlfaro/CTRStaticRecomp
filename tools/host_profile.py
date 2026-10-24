"""Symbolize host_samples.txt (written by ctr.exe --host-sample A B).

    python tools/host_profile.py [host_samples.txt] [--lines 40] [--file flat_regions.c]

Prints the hottest functions and the hottest source lines (generated code included). With
--file, the line ranking only counts lines of that file.
"""
import argparse
import collections
import glob
import os
import subprocess


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("samples", nargs="?", default="host_samples.txt")
    ap.add_argument("--lines", type=int, default=40)
    ap.add_argument("--file", default="")
    ap.add_argument("--exe", default="ctr.exe")
    a = ap.parse_args()
    rows = []
    for line in open(a.samples):
        if line.startswith("#"):
            continue
        off, n = line.split()
        rows.append((int(off, 16), int(n)))
    total = sum(n for _, n in rows)
    sym = glob.glob(os.path.expandvars(
        r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*\llvm-mingw-*\bin\llvm-symbolizer.exe"))[0]
    addrs = "\n".join(f"0x{0x140000000 + off:x}" for off, _ in rows)
    out = subprocess.run([sym, f"--obj={a.exe}", "--no-inlines", "-f"], input=addrs, capture_output=True,
                         text=True).stdout.split("\n\n")
    funcs = collections.Counter()
    lines = collections.Counter()
    for (off, n), block in zip(rows, out):
        parts = block.strip().split("\n")
        fn = parts[0] if parts else "?"
        loc = parts[1] if len(parts) > 1 else "?"
        funcs[fn] += n
        path, _, rest = loc.rpartition(":")  # file:line:col
        path, _, ln = path.rpartition(":")
        name = os.path.basename(path)
        if not a.file or name == a.file:
            lines[f"{name}:{ln}"] += n
    print(f"{total} samples\n-- functions --")
    for fn, n in funcs.most_common(30):
        print(f"{100 * n / total:6.2f}%  {fn}")
    print("-- lines --")
    for ln, n in lines.most_common(a.lines):
        print(f"{100 * n / total:6.2f}%  {ln}")


if __name__ == "__main__":
    main()
