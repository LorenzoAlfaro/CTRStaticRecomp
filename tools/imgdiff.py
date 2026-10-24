"""Compare two directories of screenshots (e.g. GPU vs software renderer).

    python tools/imgdiff.py REF_DIR TEST_DIR [--montage out.png] [--frames 2700,4800]

Per frame: share of pixels that differ at all, share that differ by more than 24 (out of 255)
in some channel, and the mean absolute difference. --montage writes reference | test | diff
(amplified) rows for the chosen frames.
"""
import argparse
import os
import sys

from PIL import Image, ImageChops


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("--montage")
    ap.add_argument("--frames", default="")
    a = ap.parse_args()
    names = sorted(n for n in os.listdir(a.ref) if n.endswith(".bmp"))
    rows = []
    want = set(str(int(f)) for f in a.frames.split(",")) if a.frames else None
    for n in names:
        tp = os.path.join(a.test, n)
        if not os.path.exists(tp):
            print(f"{n}: missing")
            continue
        r = Image.open(os.path.join(a.ref, n)).convert("RGB")
        t = Image.open(tp).convert("RGB")
        if r.size != t.size:
            print(f"{n}: size {t.size} vs {r.size}")
            continue
        d = ImageChops.difference(r, t)
        px = list(d.getdata())
        total = len(px)
        anyd = big = 0
        acc = 0
        for p in px:
            m = max(p)
            if m:
                anyd += 1
                acc += sum(p)
                if m > 24:
                    big += 1
        print(f"{n}: differ {100 * anyd / total:6.2f}%  >24 {100 * big / total:6.2f}%  mean {acc / (3 * total):.3f}")
        frame = str(int(n.split(".")[0].split("_")[-1])) if n.split(".")[0].split("_")[-1].isdigit() else n
        if a.montage and (want is None or frame in want or n in want):
            amp = d.point(lambda v: min(255, v * 8))
            rows.append((r, t, amp))
    if a.montage and rows:
        w, h = rows[0][0].size
        out = Image.new("RGB", (w * 3, h * len(rows)))
        for i, (r, t, amp) in enumerate(rows):
            out.paste(r, (0, i * h))
            out.paste(t, (w, i * h))
            out.paste(amp, (2 * w, i * h))
        out.save(a.montage)
        print(f"montage: {a.montage}")


if __name__ == "__main__":
    sys.exit(main())
