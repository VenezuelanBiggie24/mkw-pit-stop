"""Count the jumps to themselves in build/mkw against the guest's own `b .` loops in the generated C, per function.

Why: a host `jmp` to itself hangs the thread that reaches it. Most come straight from guest code that loops on
itself (`b .` after a call that never returns, or on a path that must not happen); the ones a function has beyond its
guest loops are clang's (a spin wait whose load it hoisted, undefined behaviour) and need a look. A hoisted load can
also leave a conditional jump to itself (`je .`) or a short loop that reads no memory, so both are counted too.

With --base (the same build before a generator change, e.g. recomp/loops.py dropping loop barriers) it also lists the
functions with more loops that read no memory, call nothing and are at most --short instructions long than in the
base: the ones a dropped barrier could have let clang empty. Guest loops on registers alone (a countdown) give such
loops too, so each needs a look at its C, not a verdict.

Usage: uv run tools/cpu/self_loops.py [--bin build/mkw ...] [--gen build/generated] [--base OLD ...]  (~2 min)
--bin and --base take binaries or directories of objects (build/ps5-game/game-objects/lift).
Prints both totals and every function with more host self-jumps than guest self-loops.
"""

import argparse
import re
import subprocess
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUNC = re.compile(r"^[0-9a-f]+ <(.+)>:$")
INSN = re.compile(r"^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$")
JUMP = re.compile(r"^([0-9a-f]+)\b")
C_FUNC = re.compile(r"^void (f_[0-9a-f]+)\(Cpu \*c\) \{")
C_LABEL = re.compile(r"^L_([0-9a-f]+):;")


def files(paths: list[Path]) -> list[Path]:
    out = []
    for p in paths:
        out += sorted(p.rglob("*.o")) if p.is_dir() else [p]
    return out


def scan(binary: Path, short: int) -> tuple[Counter, Counter]:
    """(self-jumps, short memory-free loops) per function of one binary or object."""
    selfj, quiet, fn, insns = Counter(), Counter(), None, []

    def close():
        # A backward jump whose loop body (target..jump, falling through) has no memory operand and no call.
        addrs = {a: k for k, (a, _, _) in enumerate(insns)}
        for k, (a, mn, op) in enumerate(insns):
            m = JUMP.match(op)
            if not mn.startswith("j") or not m:
                continue
            t = int(m.group(1), 16)
            if t == a:
                selfj[fn] += 1
            elif t < a and t in addrs and k - addrs[t] < short:
                body = insns[addrs[t] : k + 1]
                if any(n in ("jmp", "ret") for _, n, _ in body[:-1]):
                    continue  # the range is not one cycle: an earlier unconditional jump leaves it
                if not any("(" in o and not n.startswith(("lea", "nop")) or n.startswith("call") for _, n, o in body):
                    quiet[fn] += 1

    dis = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", str(binary)], stdout=subprocess.PIPE, text=True)
    for line in dis.stdout:
        if m := FUNC.match(line):
            if fn:
                close()
            fn, insns = re.sub(r"[+.].*", "", m.group(1)), []
        elif fn and (m := INSN.match(line)):
            insns.append((int(m.group(1), 16), m.group(2), m.group(3)))
    if fn:
        close()
    dis.wait()
    return selfj, quiet


def host_counts(paths: list[Path], short: int) -> tuple[Counter, Counter]:
    selfj, quiet = Counter(), Counter()
    with ThreadPoolExecutor() as pool:
        for s, q in pool.map(lambda b: scan(b, short), files(paths)):
            selfj += s
            quiet += q
    return selfj, quiet


def guest_counts(gen: Path) -> Counter:
    """A label followed by an unconditional goto to itself: the lifted `b .`, or a body-less loop of the guest."""
    out, fn = Counter(), None
    for src in sorted(gen.glob("*/*.c")):
        prev = None
        for line in src.read_text().splitlines():
            if m := C_FUNC.match(line):
                fn = m.group(1)
            if prev and "if (" not in line and re.search(rf"goto L_{prev};\s*$", line):
                out[fn] += 1
            m = C_LABEL.match(line)
            prev = m.group(1) if m else None
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bin", type=Path, nargs="+", default=[ROOT / "build/mkw"])
    ap.add_argument("--gen", type=Path, default=ROOT / "build/generated")
    ap.add_argument("--base", type=Path, nargs="+", help="the same build before the change, to compare loops with")
    ap.add_argument("--short", type=int, default=8, help="longest memory-free loop counted, in instructions")
    args = ap.parse_args()
    (host, quiet), guest = host_counts(args.bin, args.short), guest_counts(args.gen)
    print(f"host jumps to themselves {sum(host.values())}, guest self-loops in the C {sum(guest.values())}")
    extra = {f: host[f] - guest[f] for f in host if host[f] > guest[f]}
    print(f"functions with more host self-jumps than guest self-loops: {len(extra)} ({sum(extra.values())} jumps)")
    for f, n in sorted(extra.items()):
        print(f"  {f} +{n}")
    if args.base:
        base_self, base_quiet = host_counts(args.base, args.short)
        print(
            f"memory-free loops of at most {args.short} instructions: {sum(quiet.values())} "
            f"(base {sum(base_quiet.values())}); self-jumps {sum(host.values())} (base {sum(base_self.values())})"
        )
        more = {f: (quiet[f], base_quiet[f]) for f in quiet if quiet[f] > base_quiet[f]}
        more_self = {f: (host[f], base_self[f]) for f in host if host[f] > base_self[f]}
        print(f"functions with more self-jumps than the base: {len(more_self)}")
        for f, (n, b) in sorted(more_self.items()):
            print(f"  {f} {b} -> {n}")
        print(f"functions with more memory-free loops than the base: {len(more)}")
        for f, (n, b) in sorted(more.items()):
            print(f"  {f} {b} -> {n}")


if __name__ == "__main__":
    main()
