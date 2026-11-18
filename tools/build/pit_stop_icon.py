#!/usr/bin/env python3
"""Draws the "MKW: Pit Stop" icon set: our own drawing (a go-kart seen from the front, a wrench and a chequered flag, no
art of the game), lettered in Lato Black Italic (SIL OFL) converted to outlines so the SVGs need no font. Writes the SVGs
to docs/images, docs/images/icon0.png, the 512-pixel PNG that tools/ps5/build_game.py puts in the PS5 title's
sce_sys (the console's home screen icon), and icon-mark.png (256 px, the emblem), from which with icon0.png
tools/build/icon.py makes the PC program's icons. The PNGs are committed so no build needs cairosvg or the font;
run this again after changing the drawing. With --png DIR it also renders the other sizes (1024/256 px icon, 256/64/32 px
mark without lettering, the 1280x640 banner for GitHub's social preview) into DIR.

Usage: uv run --with cairosvg --with fonttools tools/build/pit_stop_icon.py [--font TTF] [--png DIR]
"""

import argparse
import math
from pathlib import Path

import cairosvg
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]
FONT_PATH = "/usr/share/fonts/truetype/lato/Lato-BlackItalic.ttf"   # Debian/Ubuntu fonts-lato
GLYPHS, CMAP, UPM, CAP = None, None, 1000, 0.7   # set by load_font()


def load_font(path: str) -> None:
    global GLYPHS, CMAP, UPM, CAP
    font = TTFont(path)
    GLYPHS, CMAP, UPM = font.getGlyphSet(), font.getBestCmap(), font["head"].unitsPerEm
    CAP = font["OS/2"].sCapHeight / UPM

NAVY, TYRE, RED = "#0a1642", "#262833", "#e62a22"


def text(s: str, size: float, tracking: float = 0.0) -> tuple[str, float]:
    """Outline of s with its baseline at y=0 starting at x=0, and its width."""
    k, x, parts = size / UPM, 0.0, []
    for ch in s:
        name = CMAP[ord(ch)]
        pen = SVGPathPen(GLYPHS, ntos=lambda v: f"{v:.1f}")
        GLYPHS[name].draw(TransformPen(pen, (k, 0, 0, -k, x, 0)))
        parts.append(pen.getCommands())
        x += GLYPHS[name].width * k + tracking
    return " ".join(p for p in parts if p), x - tracking


def lettering(s: str, size: float, cx: float, base: float, fill: str, stroke_w: float, tracking: float = 0.0,
              shadow: float = 0.0) -> str:
    d, w = text(s, size, tracking)
    t = f'transform="translate({cx - w / 2:.1f} {base:.1f})"'
    out = ""
    if shadow:
        out += (f'<path d="{d}" transform="translate({cx - w / 2 + shadow * 0.5:.1f} {base + shadow:.1f})" fill="{NAVY}" '
                f'stroke="{NAVY}" stroke-width="{stroke_w}" stroke-linejoin="round"/>')
    out += f'<path d="{d}" {t} fill="none" stroke="{NAVY}" stroke-width="{stroke_w}" stroke-linejoin="round"/>'
    return out + f'<path d="{d}" {t} fill="{fill}"/>'


DEFS = """<defs>
<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a8dff"/><stop offset="1" stop-color="#1433a6"/></linearGradient>
<linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe34d"/><stop offset="1" stop-color="#f7a40c"/></linearGradient>
<linearGradient id="steel" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#f4f6fa"/><stop offset="1" stop-color="#aab3c6"/></linearGradient>
<linearGradient id="gold" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="0.45" stop-color="#fff3a6"/><stop offset="1" stop-color="#ffc21a"/></linearGradient>
<linearGradient id="red" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f4453a"/><stop offset="1" stop-color="#c8161a"/></linearGradient>
</defs>"""


def rays(cx: float, cy: float, n: int = 14, r: float = 1600) -> str:
    out = []
    for i in range(n):
        a0, a1 = (i + 0.0) * 2 * math.pi / n, (i + 0.5) * 2 * math.pi / n
        out.append(f"M{cx} {cy}L{cx + r * math.cos(a0):.1f} {cy + r * math.sin(a0):.1f}"
                   f"L{cx + r * math.cos(a1):.1f} {cy + r * math.sin(a1):.1f}Z")
    return f'<path d="{"".join(out)}" fill="#ffffff" opacity="0.09"/>'


def tyre(x: float, y: float, w: float, h: float, side: int) -> str:
    """A fat tyre seen from the front: a rounded block with chevron tread; side -1 left, 1 right."""
    o = [f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{w * 0.36:.1f}" fill="{TYRE}" stroke="{NAVY}" stroke-width="9"/>']
    n = int(h // 20)
    for i in range(n):
        yy = y + 16 + i * (h - 26) / max(n - 1, 1)
        o.append(f'<path d="M{x + 10} {yy + 5:.1f}L{x + w / 2} {yy - 5:.1f}L{x + w - 10} {yy + 5:.1f}" fill="none" '
                 'stroke="#4b4e5e" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>')
    hx = x + (w * 0.2 if side < 0 else w * 0.8)
    o.append(f'<path d="M{hx:.1f} {y + 16}V{y + h * 0.5:.1f}" stroke="#ffffff" stroke-width="6" stroke-linecap="round" opacity="0.22"/>')
    return "".join(o)


def kart(x: float, ground: float, k: float) -> str:
    """Our own chunky go-kart seen from the front, empty seat; (x, ground) is the point under its middle."""
    st = f'stroke="{NAVY}" stroke-width="9" stroke-linejoin="round"'
    o = [f'<g transform="translate({x} {ground}) scale({k})">']
    o.append(tyre(-150, -136, 62, 104, -1) + tyre(88, -136, 62, 104, 1))   # rear tyres, behind
    for s in (-1, 1):   # exhausts
        o.append(f'<g transform="translate({s * 66} -150) rotate({s * 14})"><rect x="-13" y="-52" width="26" height="70" rx="12" '
                 f'fill="url(#steel)" {st}/><ellipse cx="0" cy="-42" rx="7" ry="5" fill="{NAVY}"/></g>')
    o.append(f'<rect x="-98" y="-132" width="196" height="84" rx="20" fill="#59607a" {st}/>')   # engine and rear body
    o.append(f'<path d="M-44 -96L-50 -182Q-50 -202 -30 -202L30 -202Q50 -202 50 -182L44 -96Z" fill="#fff6d8" {st}/>')   # seat
    o.append(f'<path d="M-30 -160Q0 -150 30 -160" fill="none" stroke="{NAVY}" stroke-width="5" stroke-linecap="round" opacity="0.35"/>')
    o.append(f'<rect x="-118" y="-62" width="236" height="44" rx="18" fill="url(#red)" {st}/>')   # fairing between the wheels
    nose = "M-74 -30Q-84 -86 -44 -110Q0 -124 44 -110Q84 -86 74 -30Q62 -8 0 -8Q-62 -8 -74 -30Z"
    o.append(f'<path d="{nose}" fill="url(#red)" {st}/>')
    o.append('<path d="M-56 -70Q-48 -96 -18 -104" fill="none" stroke="#ffffff" stroke-width="8" stroke-linecap="round" opacity="0.5"/>')
    # steering wheel over the cowl
    o.append(f'<path d="M0 -92V-118" stroke="{NAVY}" stroke-width="16" stroke-linecap="round"/>')
    o.append(f'<ellipse cx="0" cy="-126" rx="44" ry="24" fill="none" stroke="{NAVY}" stroke-width="22"/>')
    o.append(f'<ellipse cx="0" cy="-126" rx="44" ry="24" fill="none" stroke="{TYRE}" stroke-width="11"/>')
    o.append(f'<path d="M-42 -124H42" stroke="{TYRE}" stroke-width="9"/><circle cx="0" cy="-124" r="10" fill="url(#rim)" '
             f'stroke="{NAVY}" stroke-width="5"/>')
    for s in (-1, 1):   # headlights
        o.append(f'<circle cx="{s * 40}" cy="-52" r="15" fill="url(#rim)" stroke="{NAVY}" stroke-width="7"/>'
                 f'<circle cx="{s * 40 - 4}" cy="-57" r="5" fill="#ffffff" opacity="0.8"/>')
    o.append(f'<rect x="-22" y="-62" width="44" height="22" rx="9" fill="#ffffff" stroke="{NAVY}" stroke-width="6"/>')   # blank plate
    o.append(f'<rect x="-92" y="-28" width="184" height="24" rx="12" fill="url(#steel)" {st}/>')   # bumper
    o.append(tyre(-172, -96, 74, 96, -1) + tyre(98, -96, 74, 96, 1))   # front tyres
    o.append("</g>")
    return "".join(o)


def mark(cx: float, cy: float, k: float) -> str:
    """The emblem: flag and wrench crossed behind a kart seen from the front; centred on the crossing."""
    sw = 9   # outline width in emblem units
    o = [f'<g transform="translate({cx} {cy}) scale({k})" stroke-linejoin="round"><g>']

    # chequered flag, leaning left
    o.append('<g transform="rotate(-45)">')
    o.append(f'<rect x="-8" y="-212" width="16" height="390" rx="8" fill="#f4f6fa" stroke="{NAVY}" stroke-width="{sw}"/>')
    cols, rows, w, h, top = 4, 3, 100, 78, -204
    o.append(f'<rect x="{-8 - w}" y="{top}" width="{w}" height="{h}" fill="#fafafa" stroke="{NAVY}" stroke-width="{sw}"/>')
    for r in range(rows):
        for c in range(cols):
            if (r + c) % 2 == 0:
                o.append(f'<rect x="{-8 - w + c * w / cols:.1f}" y="{top + r * h / rows:.1f}" width="{w / cols:.1f}" '
                         f'height="{h / rows:.1f}" fill="#16171d"/>')
    o.append("</g>")

    # wrench, leaning right
    head = "M-17 -225.6L-17 -190A17 17 0 0 0 17 -190L17 -225.6A44 44 0 1 1 -17 -225.6Z"
    handle = '<rect x="-18" y="-160" width="36" height="340" rx="14"'
    o.append('<g transform="rotate(45)">')
    o.append(f'{handle} fill="{NAVY}" stroke="{NAVY}" stroke-width="{sw * 2}"/>')
    o.append(f'<path d="{head}" fill="{NAVY}" stroke="{NAVY}" stroke-width="{sw * 2}"/>')
    o.append(f'{handle} fill="url(#steel)"/><path d="{head}" fill="url(#steel)"/>')
    o.append("</g></g>")

    o.append(kart(0, 116, 1.0))
    o.append("</g>")
    return "".join(o)


def ribbon(cx: float, cy: float, w: float, h: float, label: str, size: float) -> str:
    out = (f'<g transform="translate({cx} {cy}) skewX(-10)"><rect x="{-w / 2}" y="{-h / 2}" width="{w}" height="{h}" rx="14" '
           f'fill="url(#red)" stroke="{NAVY}" stroke-width="9" stroke-linejoin="round"/>'
           f'<rect x="{-w / 2 + 10}" y="{-h / 2 + 7}" width="{w - 20}" height="6" rx="3" fill="#ffffff" opacity="0.3"/></g>')
    d, tw = text(label, size, size * 0.06)
    return out + (f'<path d="{d}" transform="translate({cx - tw / 2:.1f} {cy + size * CAP / 2:.1f})" fill="#ffffff" '
                  f'stroke="{NAVY}" stroke-width="2.5" stroke-linejoin="round" paint-order="stroke"/>')


def square(with_text: bool) -> str:
    s = 512
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {s} {s}" width="{s}" height="{s}">', DEFS,
         '<clipPath id="tile"><rect width="512" height="512" rx="92"/></clipPath><g clip-path="url(#tile)">',
         '<rect width="512" height="512" fill="url(#bg)"/>']
    if with_text:
        o += [rays(256, 186), mark(256, 186, 0.94),
              lettering("MKW", 136, 250, 408, "url(#gold)", 20, tracking=2, shadow=7),
              ribbon(256, 452, 318, 58, "PIT STOP", 42)]
    else:
        o += [rays(256, 256), mark(256, 262, 1.22)]
    o += ['<rect x="4" y="4" width="504" height="504" rx="88" fill="none" stroke="#ffffff" stroke-width="8" opacity="0.14"/>',
          "</g></svg>"]
    return "\n".join(o) + "\n"


def banner() -> str:
    w, h = 1280, 640
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">', DEFS,
         f'<rect width="{w}" height="{h}" fill="url(#bg)"/>', rays(330, 300), mark(330, 300, 1.2)]
    cx = 905
    o.append(lettering("MKW:", 190, cx - 8, 318, "url(#gold)", 24, tracking=3, shadow=9))
    o.append(ribbon(cx, 420, 520, 104, "PIT STOP", 78))
    # a chequered strip along the bottom edge
    n, c = 40, w / 40
    o.append(f'<rect y="{h - 2 * c}" width="{w}" height="{2 * c}" fill="#fafafa"/>')
    for i in range(n):
        o.append(f'<rect x="{i * c}" y="{h - 2 * c + (i % 2) * c}" width="{c}" height="{c}" fill="#16171d"/>')
    o.append(f'<rect y="{h - 2 * c - 8}" width="{w}" height="8" fill="{NAVY}"/>')
    o.append("</svg>")
    return "\n".join(o) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--font", default=FONT_PATH, help="Lato Black Italic .ttf")
    parser.add_argument("--png", type=Path, help="also render every PNG size into this folder (keep it under build/)")
    args = parser.parse_args()
    load_font(args.font)
    files = {"icon": square(True), "icon-mark": square(False), "banner": banner()}
    images = ROOT / "docs/images"
    for name, svg in files.items():
        (images / f"{name}.svg").write_text(svg)
    cairosvg.svg2png(bytestring=files["icon"].encode(), write_to=str(images / "icon0.png"), output_width=512,
                     output_height=512)
    # the emblem's 256 px PNG: tools/build/icon.py makes the program's small icons (48 px and under) from it
    cairosvg.svg2png(bytestring=files["icon-mark"].encode(), write_to=str(images / "icon-mark.png"), output_width=256,
                     output_height=256)
    if args.png:
        args.png.mkdir(parents=True, exist_ok=True)
        for name, size in (("icon", 1024), ("icon", 512), ("icon", 256), ("icon-mark", 256), ("icon-mark", 64),
                           ("icon-mark", 32)):
            cairosvg.svg2png(bytestring=files[name].encode(), write_to=str(args.png / f"{name}-{size}.png"),
                             output_width=size, output_height=size)
        cairosvg.svg2png(bytestring=files["banner"].encode(), write_to=str(args.png / "banner-1280x640.png"))


if __name__ == "__main__":
    main()
