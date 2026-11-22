"""Builds docs/project/roadmap.md from docs/project/roadmap.toml, and checks that the roadmap is not stale.

Why: the roadmap was written by hand with numbers and experiments, and fell behind the code within days (Adrià
21/11/2026). Now the page is a cog block generated from a short TOML, and check.py fails when the TOML's `updated`
date is more than STALE_DAYS older than the last commit that changed code.

Usage: uv run cog -r -I tools/docs docs/project/roadmap.md     (rewrites the block)
       uv run tools/docs/roadmap.py --check                    (exit 1 if stale)
"""

import datetime
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "docs/project/roadmap.toml"
CODE = ("runtime", "recomp", "sm86", "launcher", "tools/build")
STALE_DAYS = 7


def load() -> dict:
    return tomllib.loads(DATA.read_text(encoding="utf-8"))


def emit(cog) -> None:
    """The numbered sections, in priority order (the cog block of roadmap.md)."""
    for n, sec in enumerate(load()["section"], 1):
        cog.outl(f"## {n}. {sec['title']}\n")
        for item in sec["items"]:
            cog.outl(f"- {item}")
        cog.outl("")


def stale() -> str | None:
    updated = datetime.date.fromisoformat(load()["updated"])
    r = subprocess.run(["git", "log", "-1", "--format=%cs", "--", *CODE], cwd=ROOT, capture_output=True, text=True)
    if not r.stdout.strip():
        return None
    last = datetime.date.fromisoformat(r.stdout.strip())
    if (last - updated).days > STALE_DAYS:
        return (f"docs/project/roadmap.toml: updated {updated}, code changed on {last} ({(last - updated).days} days): "
                "review it and set `updated`")
    return None


if __name__ == "__main__":
    msg = stale() if "--check" in sys.argv else None
    if msg:
        print(f"docs: {msg}", file=sys.stderr)
    sys.exit(1 if msg else 0)
