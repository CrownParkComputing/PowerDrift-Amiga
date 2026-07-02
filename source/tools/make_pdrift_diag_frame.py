#!/usr/bin/env python3
"""Convert binary PPM Power Drift frames to an Amiga C header.

Builds one shared 256-entry palette across all frames: pen 0 is reserved
black, pen 255 reserved white (for the diagnostic text overlay), and the
remaining 254 pens come from a frequency-weighted median cut over every
distinct colour in the input frames. Much closer to the host composite
than the old fixed RGB332 mapping, which banded the sky gradients.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path


def next_token(data: bytes, pos: int) -> tuple[str, int]:
    n = len(data)
    while pos < n:
        c = data[pos]
        if c == ord("#"):
            while pos < n and data[pos] not in b"\r\n":
                pos += 1
        elif chr(c).isspace():
            pos += 1
        else:
            break
    start = pos
    while pos < n and not chr(data[pos]).isspace():
        pos += 1
    return data[start:pos].decode("ascii"), pos


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    magic, pos = next_token(data, 0)
    if magic != "P6":
        raise SystemExit(f"{path}: expected P6 PPM, got {magic!r}")
    width_s, pos = next_token(data, pos)
    height_s, pos = next_token(data, pos)
    max_s, pos = next_token(data, pos)
    width, height, max_v = int(width_s), int(height_s), int(max_s)
    if max_v != 255:
        raise SystemExit(f"{path}: expected max value 255, got {max_v}")
    while pos < len(data) and chr(data[pos]).isspace():
        pos += 1
    rgb = data[pos:]
    want = width * height * 3
    if len(rgb) != want:
        raise SystemExit(f"{path}: expected {want} bytes, got {len(rgb)}")
    return width, height, rgb


def median_cut(hist: dict[tuple[int, int, int], int], want: int) -> list[tuple[int, int, int]]:
    """Frequency-weighted median cut of the colour histogram into `want` boxes."""
    boxes = [list(hist.items())]
    while len(boxes) < want:
        # split the box with the largest weighted spread on its widest channel
        best = best_axis = best_score = None
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for axis in range(3):
                vals = [c[axis] for c, _ in box]
                spread = max(vals) - min(vals)
                weight = sum(n for _, n in box)
                score = spread * weight
                if best_score is None or score > best_score:
                    best, best_axis, best_score = i, axis, score
        if best is None:
            break
        box = boxes.pop(best)
        box.sort(key=lambda item: item[0][best_axis])
        total = sum(n for _, n in box)
        acc = 0
        split = 1
        for j, (_, n) in enumerate(box):
            acc += n
            if acc * 2 >= total:
                split = min(max(j + 1, 1), len(box) - 1)
                break
        boxes.append(box[:split])
        boxes.append(box[split:])
    out = []
    for box in boxes:
        total = sum(n for _, n in box)
        r = round(sum(c[0] * n for c, n in box) / total)
        g = round(sum(c[1] * n for c, n in box) / total)
        b = round(sum(c[2] * n for c, n in box) / total)
        out.append((r, g, b))
    return out


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        raise SystemExit("usage: make_pdrift_diag_frame.py output.h frame.ppm [frame.ppm ...]")
    dst = Path(argv[1])
    srcs = [Path(arg) for arg in argv[2:]]

    frames_rgb: list[bytes] = []
    width = height = None
    hist: Counter = Counter()
    for src in srcs:
        w, h, rgb = read_ppm(src)
        if width is not None and (w != width or h != height):
            raise SystemExit(f"{src}: expected {width}x{height}, got {w}x{h}")
        width, height = w, h
        frames_rgb.append(rgb)
        for i in range(0, len(rgb), 3):
            hist[(rgb[i], rgb[i + 1], rgb[i + 2])] += 1

    assert width is not None and height is not None

    # pen 0 = black (backdrop), pen 255 = white (text); median-cut the rest
    hist.pop((0, 0, 0), None)
    hist.pop((255, 255, 255), None)
    mids = median_cut(dict(hist), 254) if hist else []
    palette = [(0, 0, 0)] + mids + [(85, 85, 85)] * (254 - len(mids)) + [(255, 255, 255)]

    # nearest-pen lookup for every distinct colour
    lut: dict[tuple[int, int, int], int] = {(0, 0, 0): 0, (255, 255, 255): 255}
    worst = 0.0
    for col in hist:
        best_pen, best_d = 0, 1 << 30
        for pen, (r, g, b) in enumerate(palette):
            d = (col[0] - r) ** 2 + (col[1] - g) ** 2 + (col[2] - b) ** 2
            if d < best_d:
                best_pen, best_d = pen, d
        lut[col] = best_pen
        worst = max(worst, best_d ** 0.5)

    frames: list[bytearray] = []
    for rgb in frames_rgb:
        pens = bytearray(width * height)
        for i in range(width * height):
            pens[i] = lut[(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2])]
        frames.append(pens)

    dst.parent.mkdir(parents=True, exist_ok=True)
    with dst.open("w", encoding="ascii") as f:
        f.write("/* Generated by tools/make_pdrift_diag_frame.py. */\n")
        f.write("#ifndef PDRIFT_DIAG_FRAME_H\n#define PDRIFT_DIAG_FRAME_H\n\n")
        f.write(f"#define PDRIFT_DIAG_W {width}\n")
        f.write(f"#define PDRIFT_DIAG_H {height}\n")
        f.write(f"#define PDRIFT_DIAG_FRAMES {len(frames)}\n")
        f.write("#define PDRIFT_DIAG_PEN_BLACK 0\n")
        f.write("#define PDRIFT_DIAG_PEN_WHITE 255\n\n")
        f.write("static const char *const pdrift_diag_frame_names[PDRIFT_DIAG_FRAMES] = {\n")
        for i, src in enumerate(srcs):
            m = re.search(r"(\d+)$", src.stem)
            name = m.group(1) if m else str(i)
            f.write(f'    "{name}",\n')
        f.write("};\n\n")
        f.write("static const unsigned char pdrift_diag_palette[256][3] = {\n")
        for r, g, b in palette:
            f.write(f"    {{ 0x{r:02x}, 0x{g:02x}, 0x{b:02x} }},\n")
        f.write("};\n\n")
        f.write("static const unsigned char pdrift_diag_frames[PDRIFT_DIAG_FRAMES][PDRIFT_DIAG_W * PDRIFT_DIAG_H] = {\n")
        for frame in frames:
            f.write("  {\n")
            for off in range(0, len(frame), 16):
                row = ", ".join(f"0x{v:02x}" for v in frame[off : off + 16])
                f.write(f"    {row},\n")
            f.write("  },\n")
        f.write("};\n\n")
        f.write("#endif\n")
    print(
        f"wrote {dst} ({width}x{height}, {len(frames)} frames, "
        f"{len(hist) + 2} distinct colours -> 256 pens, worst match {worst:.1f} RGB)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
