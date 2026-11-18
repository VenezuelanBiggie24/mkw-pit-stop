#!/usr/bin/env python3
"""The program's icons: mkw.exe's .ico (256, 48, 32 and 16 px), the window's (a .c name) and the AppImage's (.png),
from the "MKW: Pit Stop" drawing the PS5 title uses (tools/build/pit_stop_icon.py): docs/images/icon0.png (512 px,
with its lettering) for 256 and 128 px, docs/images/icon-mark.png (256 px, the emblem alone) for 48 px and under,
where the lettering does not read. Both PNGs are committed, so no SVG renderer or font is needed here; they are
scaled down by area (premultiplied alpha), in plain Python. tools/build/build.py --target windows calls it before the
resource (runtime/platform/mkw.rc names "mkw.ico"); the output is made in build/, never committed. A .c name writes
app_icon_set(SDL_Window *), which gives the window a 128-pixel copy (build.py, every target): Wayland compositors with
xdg_toplevel_icon (KWin) show it in the taskbar. With an .svg name it still writes upstream's chequered-flag logo as
vector shapes (docs/images/logo.svg; the start screen draws that logo itself, launcher_logo.cpp). tools/prepare/steam.py
draws Steam's library images with render() (the flag) and png().

Usage: uv run tools/build/icon.py OUT.ico | OUT.svg | OUT.png | OUT.c
"""

import math
import struct
import sys
import zlib
from pathlib import Path

BG_TOP, BG_BOTTOM = (36, 84, 196), (18, 38, 110)   # blue, lighter at the top
POLE = (235, 235, 240)


def coverage(x: float, y: float) -> tuple[int, int, int, int]:
    """Colour at (x, y) in a unit square, (0, 0) top left; alpha 0 outside the rounded square."""
    r = 0.18   # corner radius
    cx, cy = min(max(x, r), 1 - r), min(max(y, r), 1 - r)
    if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
        return (0, 0, 0, 0)
    # the flag: 4x3 checks on a waving cloth between the pole and the right side
    fx, fy = (x - 0.30) / 0.52, y - 0.20 - 0.045 * math.sin(x * 2 * math.pi * 1.1)
    if 0 <= fx <= 1 and 0 <= fy <= 0.42:
        light = (int(fx * 4) + int(fy / 0.42 * 3)) % 2 == 0
        shade = 1 - 0.18 * math.cos(x * 2 * math.pi * 1.1)   # folds of the cloth
        c = (250, 250, 250) if light else (20, 20, 24)
        return (*[min(255, int(v * shade)) for v in c], 255)
    if 0.24 <= x <= 0.30 and 0.16 <= y <= 0.86:   # the pole
        return (*POLE, 255)
    t = y
    return (*[int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)], 255)


def render(size: int, ss: int = 4) -> bytes:
    """RGBA rows, each pixel the mean of ss x ss samples (antialiasing)."""
    rows = bytearray()
    for py in range(size):
        rows.append(0)   # PNG filter: none
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    c = coverage((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    a = c[3]
                    acc[0] += c[0] * a
                    acc[1] += c[1] * a
                    acc[2] += c[2] * a
                    acc[3] += a
            a = acc[3]
            rows += bytes([acc[0] // a, acc[1] // a, acc[2] // a, a // (ss * ss)] if a else [0, 0, 0, 0])
    return bytes(rows)


ROOT = Path(__file__).resolve().parents[2]
ICON = ROOT / "docs/images/icon0.png"       # 512 px, lettered
MARK = ROOT / "docs/images/icon-mark.png"   # 256 px, the emblem alone
SOURCES = (ICON, MARK)                      # build.py rebuilds the icons when these change


def read_png(path: Path) -> tuple[int, int, list[float]]:
    """Width, height and premultiplied RGBA floats of an 8-bit RGBA, non-interlaced PNG (what cairosvg writes)."""
    data = path.read_bytes()
    pos, idat = 8, b""
    w = h = 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, ctype, interlace) != (8, 6, 0):
                raise SystemExit(f"{path}: not an 8-bit RGBA PNG without interlacing")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride, prev, out = zlib.decompress(idat), w * 4, bytearray(w * 4), []
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b, c = prev[i], prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for i in range(0, stride, 4):
            al = line[i + 3] / 255
            out += [line[i] * al, line[i + 1] * al, line[i + 2] * al, al]
        prev = line
    return w, h, out


def scale(src: tuple[int, int, list[float]], n: int) -> bytes:
    """n x n RGBA bytes, each pixel the area-weighted mean of the source pixels it covers."""
    w, h, px = src

    def spans(size: int) -> list[list[tuple[int, float]]]:
        k, res = size / n, []
        for o in range(n):
            a, b, cell = o * k, (o + 1) * k, []
            for i in range(int(a), min(size, math.ceil(b))):
                cover = min(b, i + 1) - max(a, i)
                if cover > 0:
                    cell.append((i, cover / k))
            res.append(cell)
        return res

    xs, ys, out = spans(w), spans(h), bytearray()
    for cy in ys:
        for cx in xs:
            acc = [0.0, 0.0, 0.0, 0.0]
            for y, wy in cy:
                row = y * w
                for x, wx in cx:
                    q, wt = (row + x) * 4, wx * wy
                    acc[0] += px[q] * wt
                    acc[1] += px[q + 1] * wt
                    acc[2] += px[q + 2] * wt
                    acc[3] += px[q + 3] * wt
            a = acc[3]
            out += bytes([round(min(255, acc[0] / a)), round(min(255, acc[1] / a)), round(min(255, acc[2] / a)),
                          round(a * 255)] if a > 0 else [0, 0, 0, 0])  # fmt: skip
    return bytes(out)


def pixels(size: int) -> bytes:
    """The icon at size px: the lettered one from 128 px up, the emblem below (the lettering does not read there)."""
    return scale(read_png(ICON if size >= 128 else MARK), size)


def png(size: int) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    px, stride = pixels(size), size * 4
    rows = b"".join(b"\0" + px[y * stride : (y + 1) * stride] for y in range(size))   # filter none
    head = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)   # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def wave(x: float) -> float:
    """The cloth's vertical offset at x, as in coverage()."""
    return 0.045 * math.sin(x * 2 * math.pi * 1.1)


def svg() -> str:
    """The icon as SVG in a 256-unit square: each check a polygon whose edges follow the wave."""
    s, steps = 256, 8

    def pt(fx: float, fy: float) -> str:
        x = 0.30 + 0.52 * fx
        return f"{x * s:.1f},{(0.20 + wave(x) + fy) * s:.1f}"

    top, bottom = "#%02x%02x%02x" % BG_TOP, "#%02x%02x%02x" % BG_BOTTOM
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {s} {s}" width="{s}" height="{s}">',
        f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{top}"/>'
        f'<stop offset="1" stop-color="{bottom}"/></linearGradient></defs>',
        f'<rect width="{s}" height="{s}" rx="{0.18 * s:.1f}" fill="url(#bg)"/>',
        f'<rect x="{0.24 * s:.1f}" y="{0.16 * s:.1f}" width="{0.06 * s:.1f}" height="{0.70 * s:.1f}" '
        'fill="#ebebf0"/>',
    ]
    for row in range(3):
        for col in range(4):
            x0, x1, y0, y1 = col / 4, (col + 1) / 4, row * 0.14, (row + 1) * 0.14
            xs = [x0 + (x1 - x0) * i / steps for i in range(steps + 1)]
            points = [pt(x, y0) for x in xs] + [pt(x, y1) for x in reversed(xs)]
            fill = "#fafafa" if (col + row) % 2 == 0 else "#141418"
            out.append(f'<polygon points="{" ".join(points)}" fill="{fill}"/>')
    return "\n".join([*out, "</svg>", ""])


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    if sys.argv[1].endswith(".svg"):
        Path(sys.argv[1]).write_text(svg())
        return
    if sys.argv[1].endswith(".png"):
        Path(sys.argv[1]).write_bytes(png(256))
        return
    if sys.argv[1].endswith(".c"):
        n = 128
        px = pixels(n)
        body = ",".join(str(b) for b in px)
        Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
        Path(sys.argv[1]).write_text(
            "// From docs/images/icon0.png by tools/build/icon.py: the window's icon, RGBA rows. Generated, never committed.\n"
            "#include <SDL3/SDL.h>\n"
            f"static const unsigned char px[{len(px)}] = {{{body}}};\n"
            "void app_icon_set(SDL_Window *w) {\n"
            f"    SDL_Surface *s = SDL_CreateSurfaceFrom({n}, {n}, SDL_PIXELFORMAT_RGBA32, (void *)px, {n * 4});\n"
            "    if (s) SDL_SetWindowIcon(w, s), SDL_DestroySurface(s);   // SDL keeps a copy\n"
            "}\n"
        )
        return
    sizes = (256, 48, 32, 16)
    images = [png(s) for s in sizes]
    out = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    for s, data in zip(sizes, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_bytes(out + b"".join(images))


if __name__ == "__main__":
    main()
