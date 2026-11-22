"""The one table of runtime/core/base/cfg.c (`rows[]`) holds each key once, in the shapes the runtime reads.

Why: the key list, the ini sections and the menu's settings were three tables in one file, and two PRs that each added
a key met in all three (60 rebases analysed 21/11/2026: 56% stopped at a conflict). HDR once sat in the menu's table
but not in the list of known keys, so `HDR = off` written by the menu was dropped at the next start with "unknown key
'HDR'" (#168, 13/11/2026); one table makes that impossible, and this test keeps it one table.

Usage: uv run pytest tests/runtime/test_cfg.py
"""

import re
from pathlib import Path

SRC = (Path(__file__).resolve().parents[2] / "runtime/core/base/cfg.c").read_text()
BODY = SRC[SRC.index("rows[] = {") : SRC.index("\n};", SRC.index("rows[] = {"))]
ROW = re.compile(r'^\s*(S|KS|K)\("([A-Z0-9_]+)"', re.M)


def test_each_key_has_one_row() -> None:
    keys = [m.group(2) for m in ROW.finditer(BODY)]
    assert len(keys) > 200, "the table was not found"
    dup = sorted({k for k in keys if keys.count(k) > 1})
    assert not dup, f"keys with more than one row in cfg.c: {dup}"


def test_rows_are_one_per_line() -> None:
    """A second row on a line would bring back the conflicts one-per-line avoids."""
    for line in BODY.splitlines():
        code = line.split("//")[0]
        assert len(re.findall(r"\b(?:S|KS|K)\(\"", code)) <= 1, f"two rows on one line: {line.strip()}"


def test_key_only_rows_stay_in_order() -> None:
    """K rows are alphabetical so two additions land in different places."""
    ks = [m.group(2) for m in ROW.finditer(BODY) if m.group(1) == "K"]
    assert ks == sorted(ks), "K rows out of alphabetical order: " + str([a for a, b in zip(ks, sorted(ks)) if a != b][:3])
