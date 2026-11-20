"""Builds docs/i18n/README.<lang>.md from a translated body and the language's strings, or checks they are current.

Why: the README is read in the languages of most of the players who report here; a translation is written by hand once
(docs/i18n/<lang>.body.md) and this keeps the parts every language repeats in step: the centred title, the download
buttons, the link row and the language bar (the same as README.md's, its own language in bold), the body's relative
links moved two folders up, and a "translated from" line with the hash of the README.md it follows and of the body.
A rebuild keeps a language's README.md hash while its body is unchanged, so that adding a language or changing the
bar does not mark every old translation as current; an edited body takes the current one. check.py runs the
check and warns, without refusing the commit, when README.md changed after a translation (Adrià 13/11/2026).

Usage: uv run tools/docs/i18n_readme.py            (writes every docs/i18n/README.<lang>.md)
       uv run tools/docs/i18n_readme.py --check    (lists the translations older than README.md; exit 0 either way)
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
I18N = ROOT / "docs/i18n"
REPO = "https://github.com/exploitizm/mkw-pit-stop"
# (code, name in its own language, right to left)
LANGS = [("zh-CN", "简体中文", False), ("es", "Español", False), ("ar", "العربية", True), ("hi", "हिन्दी", False),
         ("pt-BR", "Português (Brasil)", False), ("pt-PT", "Português (Portugal)", False),
         ("de", "Deutsch", False)]  # fmt: skip
# per language: tagline, download, latest for PS5, and the link row's names (LINKS' order)
# fmt: off
STRINGS = {
    "zh-CN": ("《马力欧赛车 世界》（Nintendo Switch 2）的原生 PlayStation 5 自制移植版，基于静态重编译。", "下载",
              "PS5 最新版", ["发布", "更新日志", "参与贡献", "路线图", "PS5 说明", "问题"]),
    "es": ("Un port homebrew nativo para PlayStation 5 de Mario Kart World (Nintendo Switch 2), por recompilación estática.",
           "Descargar", "última para PS5", ["Versiones", "Cambios", "Contribuir", "Hoja de ruta", "Notas de PS5", "Issues"]),
    "ar": ("منفذ homebrew أصلي لـ PlayStation 5 للعبة Mario Kart World (Nintendo Switch 2)، بإعادة تجميع ثابتة.", "تنزيل",
           "أحدث إصدار لـ PS5", ["الإصدارات", "سجل التغييرات", "المساهمة", "خارطة الطريق", "ملاحظات PS5", "المشكلات"]),
    "hi": ("Mario Kart World (Nintendo Switch 2) का नेटिव PlayStation 5 होमब्रू पोर्ट, स्टैटिक रीकम्पाइलेशन से।",
           "डाउनलोड", "PS5 के लिए नवीनतम", ["रिलीज़", "बदलाव", "योगदान", "रोडमैप", "PS5 नोट्स", "इश्यू"]),
    "pt-BR": ("Um port homebrew nativo para PlayStation 5 de Mario Kart World (Nintendo Switch 2), por recompilação estática.",
              "Baixar", "mais recente para PS5", ["Versões", "Mudanças", "Contribuir", "Roteiro", "Notas do PS5", "Issues"]),
    "pt-PT": ("Um port homebrew nativo para PlayStation 5 de Mario Kart World (Nintendo Switch 2), por recompilação estática.",
              "Transferir", "mais recente para PS5", ["Versões", "Alterações", "Contribuir", "Roteiro", "Notas da PS5", "Issues"]),
    "de": ("Ein nativer PlayStation-5-Homebrew-Port von Mario Kart World (Nintendo Switch 2), per statischer Rekompilierung.",
           "Download", "neueste für PS5", ["Releases", "Änderungen", "Mitwirken", "Roadmap", "PS5-Hinweise", "Issues"]),
}
# fmt: on
LINKS = [
    f"{REPO}/releases",
    "../../CHANGELOG.md",
    "../../CONTRIBUTING.md",
    "../../docs/project/roadmap.md",
    "../../docs/ps5/ps5.md",
    f"{REPO}/issues",
]  # fmt: skip
# The body's links to its own sections: (#@key) in a body becomes the anchor GitHub makes from that language's heading
# (the README's level-2 headings in order: Status, What you need, Installing on Windows, Installing on PS5 from Linux,
# Pit Stop, Playing on PC, Building from source, Performance, Reporting bugs, Licenses).
SECTION_KEYS = {"windows": 2, "linux": 3, "pc": 5, "performance": 7}
# Scripts whose letters join (Arabic) or stack (Devanagari): their buttons stay in English (13/11/2026, seen on GitHub)
LATIN_BUTTONS = {"ar", "hi"}
SOURCE = re.compile(r"<!-- translated from README.md ([0-9a-f]{12})(?: body ([0-9a-f]{12}))? -->")
REL_MD = re.compile(r"(\]\()(?!https?:|#|mailto:|\.\./)([^)\s]+)")
REL_HTML = re.compile(r'((?:href|src)=")(?!https?:|#|mailto:|\.\./)([^"]+)')


def readme_hash() -> str:
    return hashlib.sha256((ROOT / "README.md").read_bytes()).hexdigest()[:12]


def source_stamp(code: str, body: str) -> str:
    """The "translated from" line: the README.md hash the built file already names while its body is unchanged (a
    file stamped before the body hash existed counts as unchanged), the current one for a new or edited body."""
    body_hash = hashlib.sha256(body.encode("utf-8")).hexdigest()[:12]
    built = I18N / f"README.{code}.md"
    m = SOURCE.search(built.read_text(encoding="utf-8")) if built.is_file() else None
    source = m.group(1) if m and m.group(2) in (None, body_hash) else readme_hash()
    return f"<!-- translated from README.md {source} body {body_hash} -->"


def badge(text: str) -> str:
    return text.replace("-", "--").replace("_", "__").replace(" ", "_")


def bar(code: str) -> str:
    items = ['<a href="../../README.md">English</a>']
    for c, name, _ in LANGS:
        items.append(f"<b>{name}</b>" if c == code else f'<a href="README.{c}.md">{name}</a>')
    return '<!-- i18n-bar:start -->\n<p align="center">\n  ' + " ·\n  ".join(items) + "\n</p>\n<!-- i18n-bar:end -->"


def slug(heading: str) -> str:
    """GitHub's anchor for a heading: lower case, letters, digits, marks, spaces, hyphens and underscores kept, spaces
    to hyphens."""
    import unicodedata

    t = heading.strip().lower()
    t = "".join(ch for ch in t if ch in " -_" or unicodedata.category(ch)[0] in "LNM")
    return t.replace(" ", "-")


def section_links(body: str) -> str:
    heads = [ln[3:] for ln in body.splitlines() if ln.startswith("## ")]

    def repl(m: re.Match) -> str:
        return "(#" + slug(heads[SECTION_KEYS[m.group(1)]]) + ")"

    return re.sub(r"\(#@([a-z]+)\)", repl, body)


def build(code: str, rtl: bool) -> str:
    tag, dl, ps5, names = STRINGS[code]
    if code in LATIN_BUTTONS:  # shields.io's for-the-badge spaces and capitalises letters one by one: it breaks
        dl, ps5 = "Download", "latest for PS5"
    from urllib.parse import quote

    def button(what: str, color: str) -> str:
        url = "https://img.shields.io/badge/" + quote(badge("⬇ " + dl)) + "-" + quote(badge(what)) + f"-{color}"
        return (f'  <a href="{REPO}/releases/latest"><img src="{url}?style=for-the-badge&labelColor=1b2740" '
                f'alt="{dl}: {what}"></a>')  # fmt: skip

    links = " ·\n  ".join(f'<a href="{u}">{n}</a>' if i > 0 else f'<b><a href="{u}">{n}</a></b>'
                          for i, (u, n) in enumerate(zip(LINKS, names)))  # fmt: skip
    body = (I18N / f"{code}.body.md").read_text(encoding="utf-8").strip()
    stamp = source_stamp(code, body)
    body = section_links(REL_HTML.sub(r"\1../../\2", REL_MD.sub(r"\1../../\2", body)))
    if rtl:
        body = f'<div dir="rtl">\n\n{body}\n\n</div>'
    head = "\n\n".join([
        stamp,
        '<p align="center"><img src="../images/icon.svg" width="144" alt="Mario Kart World: Pit Stop icon"></p>',
        '<h1 align="center">Mario Kart World: Pit Stop</h1>',
        f'<p align="center">\n  <b>{tag}</b>\n</p>',
        '<p align="center">\n' + button(ps5, "0070d1") + "\n</p>",
        f'<p align="center">\n  {links}\n</p>',
        bar(code),
    ])  # fmt: skip
    return head + "\n\n" + body + "\n"


def stale() -> list[str]:
    now = readme_hash()
    out = []
    for code, _, _ in LANGS:
        p = I18N / f"README.{code}.md"
        m = SOURCE.search(p.read_text(encoding="utf-8")) if p.is_file() else None
        if not m or m.group(1) != now:
            out.append(f"docs/i18n/README.{code}.md")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        for p in stale():
            print(
                f"note: {p} was translated from an older README.md: update docs/i18n/<lang>.body.md and run "
                "tools/docs/i18n_readme.py"
            )
        return 0
    for code, _, rtl in LANGS:
        (I18N / f"README.{code}.md").write_text(build(code, rtl), encoding="utf-8")
        print(f"docs/i18n/README.{code}.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
