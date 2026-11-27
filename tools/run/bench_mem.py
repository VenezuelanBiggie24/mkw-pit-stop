"""Memory over a benchmark run from its log: the "bench: mem t=..." lines as a table, with each measure's trend.

Why: MKW_BENCH (runtime/core/diag/bench_mem.c) samples memory every second and logs a line every 30 s, and reports at
the end. A run that dies of memory (the case MKW_BENCH=ko is there to find) never reaches the report: its periodic
lines are all there is. This reads them from any log, finished or not, and prints the table, the start, end, worst and
least-squares trend (MiB per minute) of each measure, and the rally openings and governor passes between them, so a
leak (a steady trend) tells apart from a squeeze (a low minimum) without the report.

Usage:
  uv run tools/run/bench_mem.py build/run/ps5/NAME-mkw.log [--tsv OUT.tsv] [--every N]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LINE = re.compile(r"bench: mem t=(\d+)((?: \w+=-?[\d.]+)+)")
RALLY = re.compile(r"bench: rally (\d+) opens at ([\d.]+) s")
LOW_IS_BAD = {"free", "largest", "flex_free", "pt_cpu_free", "pt_gpu_free"}
COUNTERS = {"gov_hard", "gov_soft", "gov_freed", "relieves", "stg_deferred"}


def trend(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    if n < 2:
        return 0.0
    sx, sy = sum(xs), sum(ys)
    sxx, sxy = sum(x * x for x in xs), sum(x * y for x, y in zip(xs, ys))
    d = n * sxx - sx * sx
    return (n * sxy - sx * sy) / d if d else 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", type=Path)
    ap.add_argument("--tsv", type=Path, help="write every sample line as a table here")
    ap.add_argument("--every", type=int, default=1, help="print every Nth line of the table (default all)")
    a = ap.parse_args()
    rows: list[dict[str, float]] = []
    rallies: list[tuple[int, float]] = []
    for line in a.log.read_text(errors="replace").splitlines():
        if m := LINE.search(line):
            row = {"t": float(m.group(1))}
            row.update((k, float(v)) for k, v in (kv.split("=") for kv in m.group(2).split()))
            rows.append(row)
        elif m := RALLY.search(line):
            rallies.append((int(m.group(1)), float(m.group(2))))
    if not rows:
        print(f"{a.log}: no 'bench: mem' lines (not a MKW_BENCH run, or it ended before its first race)")
        return 1
    keys = [k for k in rows[0] if k != "t"]
    for r in rows[1:]:
        keys += [k for k in r if k != "t" and k not in keys]
    if a.tsv:
        a.tsv.write_text("\n".join(["\t".join(["t", *keys])]
                                   + ["\t".join(str(r.get(k, "")) for k in ["t", *keys]) for r in rows]) + "\n")
        print(f"{len(rows)} samples -> {a.tsv}")
    shown = [k for k in keys if k not in COUNTERS] + ["gov_hard", "relieves"]
    print("     t " + " ".join(f"{k[:11]:>11}" for k in shown))
    for i, r in enumerate(rows):
        if i % a.every == 0 or i == len(rows) - 1:
            print(f"{r['t']:6.0f} " + " ".join(f"{r.get(k, float('nan')):11.0f}" for k in shown))
    print(f"\n{len(rows)} lines over {rows[-1]['t'] - rows[0]['t']:.0f} s (MiB; pt_*: page-table units)")
    print(f"{'measure':<13} {'start':>9} {'end':>9} {'worst':>9} {'at s':>7} {'trend/min':>10}")
    for k in keys:
        if k in COUNTERS:
            continue
        pts = [(r["t"], r[k]) for r in rows if k in r and r[k] >= 0]
        if not pts:
            continue
        worst = min(pts, key=lambda p: p[1]) if k in LOW_IS_BAD else max(pts, key=lambda p: p[1])
        tr = trend([t / 60 for t, _ in pts], [v for _, v in pts])
        print(f"{k:<13} {pts[0][1]:9.0f} {pts[-1][1]:9.0f} {worst[1]:9.0f} {worst[0]:7.0f} {tr:+10.1f}")
    last, first = rows[-1], rows[0]
    print("governor: " + ", ".join(f"{k} +{last.get(k, 0) - first.get(k, 0):.0f}" for k in sorted(COUNTERS)))
    if rallies:
        print("rally openings: " + ", ".join(f"{n} at {t:.0f} s" for n, t in rallies))
    return 0


if __name__ == "__main__":
    sys.exit(main())
