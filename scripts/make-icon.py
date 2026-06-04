#!/usr/bin/env python3
"""Generate ssh-server's launcher icon (pak/res/icon.png) with no third-party deps.

A rounded square tile in the Jawaka "Leaf" highlight green carrying a dark
terminal prompt glyph (chevron + underscore) — so SSH Server reads as a
first-class app rather than the generic Leaf "_apps" badge. Rendered with
analytic signed-distance fields for anti-aliased edges, encoded as a PNG using
only the standard library (zlib + struct).

Palette mirrors the Leaf scheme in Jawaka/internal/settings/appearance.c.
Run from the ssh-server repo root:  python3 scripts/make-icon.py
"""
import math
import os
import struct
import zlib

SIZE = 256

# Leaf palette (#RRGGBB).
TILE = (0x7F, 0xB0, 0x69)   # highlight green — the tile fill
GLYPH = (0x0F, 0x16, 0x0E)  # near-black bg green — the prompt glyph


def smoothstep_cover(d):
    """Coverage in [0,1] for an SDF: inside (d<0) -> 1, with ~1px AA band."""
    return min(1.0, max(0.0, 0.5 - d))


def sd_round_box(px, py, cx, cy, half, r):
    qx = abs(px - cx) - (half - r)
    qy = abs(py - cy) - (half - r)
    ax, ay = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


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
    c = SIZE / 2.0
    half = SIZE * 0.43          # tile half-extent (small margin to the edge)
    radius = SIZE * 0.22        # corner radius

    # Terminal prompt glyph ">_", nudged up so the underscore reads as a
    # separate baseline line rather than fusing with the chevron's lower arm.
    chev_cy = c - SIZE * 0.05
    chev_tip_x = c + SIZE * 0.08
    chev_x = c - SIZE * 0.13
    chev_dy = SIZE * 0.14
    stroke = SIZE * 0.044
    us_y = c + SIZE * 0.27
    us_x0, us_x1 = c - SIZE * 0.15, c + SIZE * 0.05

    px = bytearray()
    for y in range(SIZE):
        px.append(0)  # PNG filter byte (None) per scanline
        for x in range(SIZE):
            sx, sy = x + 0.5, y + 0.5
            tile_cov = smoothstep_cover(sd_round_box(sx, sy, c, c, half, radius))
            if tile_cov <= 0.0:
                px.extend((0, 0, 0, 0))
                continue

            # Glyph coverage: two chevron strokes + an underscore capsule.
            d = sd_segment(sx, sy, chev_x, chev_cy - chev_dy, chev_tip_x, chev_cy, stroke)
            d = min(d, sd_segment(sx, sy, chev_x, chev_cy + chev_dy, chev_tip_x, chev_cy, stroke))
            d = min(d, sd_segment(sx, sy, us_x0, us_y, us_x1, us_y, stroke * 0.9))
            glyph_cov = smoothstep_cover(d)

            r, g, b = lerp(TILE, GLYPH, glyph_cov)
            a = int(round(255 * tile_cov))
            px.extend((r, g, b, a))
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
