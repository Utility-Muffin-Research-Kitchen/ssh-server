#!/usr/bin/env python3
"""Generate ssh-server's launcher icon (pak/res/icon.png) with no third-party deps.

A dark terminal window filling the icon — a title bar with three dots and a green
">_" prompt — so SSH Server reads as a first-class app rather than the generic
Leaf "_apps" badge. Rendered with analytic signed-distance fields for anti-aliased
edges, encoded as a PNG using only the standard library (zlib + struct).

Palette mirrors the Leaf scheme in Jawaka/internal/settings/appearance.c.
Run from the ssh-server repo root:  python3 scripts/make-icon.py
"""
import math
import os
import struct
import zlib

SIZE = 256

# Leaf palette (#RRGGBB).
WINDOW = (0x15, 0x24, 0x0E)  # near-black bg green — the terminal window
PROMPT = (0x8F, 0xD2, 0x7E)  # light green — the prompt + first dot
DOT_DIM = (0x57, 0x84, 0x49) # muted green — the other two dots
FRAME = (0x7F, 0xB0, 0x69)   # leaf highlight green — thin frame (holds shape on dark themes)
FRAME_W = 7                  # frame thickness (px at 256)


def cover(d):
    """Coverage in [0,1] for an SDF: inside (d<0) -> 1, with ~1px AA band."""
    return min(1.0, max(0.0, 0.5 - d))


def sd_round_box(px, py, cx, cy, hx, hy, r):
    qx = abs(px - cx) - (hx - r)
    qy = abs(py - cy) - (hy - r)
    ax, ay = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


def sd_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def sd_segment(px, py, ax, ay, bx, by, thick):
    """Distance to a thick capsule (line segment of half-width `thick`)."""
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    denom = bax * bax + bay * bay
    h = 0.0 if denom == 0 else max(0.0, min(1.0, (pax * bax + pay * bay) / denom))
    dx, dy = pax - bax * h, pay - bay * h
    return math.hypot(dx, dy) - thick


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def render():
    s = SIZE / 256.0
    c = SIZE / 2.0
    whalf, wr = 108 * s, 46 * s             # window fills the icon (small margin)
    dot_y, dot_r = 56 * s, 9 * s
    dots = [(58 * s, dot_y, PROMPT), (86 * s, dot_y, DOT_DIM), (114 * s, dot_y, DOT_DIM)]
    st = 11 * s                             # prompt stroke half-width
    chev = [(62 * s, 104 * s), (108 * s, 138 * s), (62 * s, 172 * s)]
    us_x0, us_x1, us_y = 122 * s, 180 * s, 178 * s

    px = bytearray()
    for y in range(SIZE):
        px.append(0)  # PNG filter byte (None) per scanline
        for x in range(SIZE):
            sx, sy = x + 0.5, y + 0.5
            win_d = sd_round_box(sx, sy, c, c, whalf, whalf, wr)
            win_cov = cover(win_d)
            if win_cov <= 0.0:
                px.extend((0, 0, 0, 0))
                continue

            # Leaf-green frame at the edge, dark window filling the interior.
            col = FRAME
            col = lerp(col, WINDOW, cover(win_d + FRAME_W * s))
            for dx, dy, dc in dots:
                col = lerp(col, dc, cover(sd_circle(sx, sy, dx, dy, dot_r)))
            d = sd_segment(sx, sy, chev[0][0], chev[0][1], chev[1][0], chev[1][1], st)
            d = min(d, sd_segment(sx, sy, chev[1][0], chev[1][1], chev[2][0], chev[2][1], st))
            d = min(d, sd_segment(sx, sy, us_x0, us_y, us_x1, us_y, st * 0.9))
            col = lerp(col, PROMPT, cover(d))

            px.extend((col[0], col[1], col[2], int(round(255 * win_cov))))
    return bytes(px)


def write_png(path, raw, size):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(__file__), "..", "pak", "res", "icon.png")
    write_png(os.path.abspath(out), render(), SIZE)
    print("wrote", os.path.abspath(out))
