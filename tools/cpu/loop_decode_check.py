"""Checks recomp/loops.py's instruction decoder against capstone and the lifter, over every distinct word of the game.

Why: recomp/loops.py drops a loop's compiler barrier only when the addresses of its loads and stores move every
trip, which it reads from its own decoder. A wrong base register, offset or writeback there could drop a barrier a
spin wait needs, and no game run would show it until that loop hangs. So each decoded load and store is compared with
capstone's memory operand (base, index, offset, writeback, post-index amount), each `add`/`mov` with capstone's
operands, each decoded write set with what the lifted C writes (regs.written), and no word capstone sees touching
memory may decode as anything but a load, a store or `mem`. Then needs_barrier() runs on a set of small loops
(spin waits, list and array walks, store-to-load forwarding, cancelled increments) with the answer each must give.

Usage: uv run tools/cpu/loop_decode_check.py [--exefs DIR] [--modules main,sdk,subsdk0]   (~1 min on all cores)
Prints the counts per decoded kind and every mismatch (at most 20 per kind of mismatch); exits 1 on any.
"""

import argparse
import os
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

import loops  # noqa: E402
import regs  # noqa: E402
from lifter import Lifter, Unsupported  # noqa: E402
from nso import Nso  # noqa: E402

DEFAULT_EXEFS = Path(os.environ.get("MKW_GAME", "game")) / "exefs"
BASE = 0x8000000  # any aligned base: only PC-relative operands depend on it
M64 = (1 << 64) - 1


def text_words(path: Path) -> np.ndarray:
    nso = Nso(str(path))
    off, size = nso.text
    return np.frombuffer(bytes(nso.image), dtype="<u4", count=size // 4, offset=off)


def reg_index(name: str) -> int | None:
    """Capstone register name -> 0-30, 31 for SP, None for XZR/WZR."""
    if name in ("sp", "wsp"):
        return 31
    if name in ("xzr", "wzr"):
        return None
    if name in ("fp", "x29", "w29"):
        return 29
    if name in ("lr", "x30", "w30"):
        return 30
    if name[0] in "xw" and name[1:].isdigit():
        return int(name[1:])
    raise ValueError(name)


# Loops for needs_barrier() itself, assembled with keystone at 0x8000000 (text: what each case is, and whether its back
# edge must keep the barrier). Each ends in `ret`; its back edge is its only backward branch.
CASES = [
    ("spin on a fixed flag (keep)", [0xB9400268, 0x34FFFFE8, 0xD65F03C0]),
    ("list walk x9 = [x9+8] (drop)", [0xF9400529, 0x3940412A, 0xB5FFFFC9, 0xD65F03C0]),
    ("array walk post-index (drop)", [0xF8408408, 0xF1000421, 0x54FFFFC1, 0xD65F03C0]),
    ("counted index [x0, x2, lsl 3] (drop)", [0xF8627808, 0x91000442, 0xEB01005F, 0x54FFFFA1, 0xD65F03C0]),
    ("index and base both move (keep)", [0xF8627808, 0x91000442, 0x91002000, 0xEB01005F, 0x54FFFF81, 0xD65F03C0]),
    ("increment cancelled (keep)", [0xF9400008, 0x91002000, 0xD1002000, 0xB4FFFFA8, 0xD65F03C0]),
    ("base reset from a fixed register (keep)", [0xF9400008, 0x91002280, 0xB4FFFFC8, 0xD65F03C0]),
    ("store-to-load forwarding (keep)", [0xF9000134, 0xF9400128, 0xF940010A, 0x91004129, 0xB4FFFF8A, 0xD65F03C0]),
    ("narrow store, wide chase (drop)", [0xF9400529, 0x39006121, 0xB5FFFFC9, 0xD65F03C0]),
    ("store to a fixed address (keep)", [0xF9400529, 0xF9000289, 0xB5FFFFC9, 0xD65F03C0]),
    ("side path with a fixed load (keep)", [0xF9400529, 0x36000049, 0xB940026A, 0xB5FFFFA9, 0xD65F03C0]),
    ("every cycle through an opaque call (drop)", [0xB9400268, 0xD63F0200, 0x34FFFFC8, 0xD65F03C0]),
    ("every cycle through an inlinable bl (keep)", [0xB9400268, 0x97FFFFFF, 0x34FFFFC8, 0xD65F03C0]),
    ("acquire load (keep)", [0x88DFFD28, 0x91001129, 0x34FFFFC8, 0xD65F03C0]),
    ("ldr literal in code (drop)", [0x58000008, 0xF1000421, 0x54FFFFC1, 0xD65F03C0]),
    ("32-bit counter into uxtw index (drop)", [0xB8625808, 0x11000442, 0x6B01005F, 0x54FFFFA1, 0xD65F03C0]),
    ("swap of two registers (keep)", [0xF9400128, 0xAA0903EA, 0xAA0B03E9, 0xAA0A03EB, 0xB4FFFF88, 0xD65F03C0]),
]


def check_cases() -> list[str]:
    """needs_barrier() on CASES, with the flow graph gen.py builds; returns the cases it gets wrong."""
    from gen import Emitter

    lifter = Lifter(BASE, "c")
    wrong = []
    for what, words in CASES:
        end = BASE + 4 * len(words)
        insns = [lifter.lift(w, BASE + 4 * i) for i, w in enumerate(words)]
        succ = Emitter.flow(insns, BASE, end, {})
        ops = [
            loops.decode(w, BASE + 4 * i) if ins.kind == "op" else loops.Op("other")
            for i, (w, ins) in enumerate(zip(words, insns, strict=True))
        ]
        texts = [ins.code if ins.kind in ("op", "trap") else ins.cond or "" for ins in insns]
        # As gen.py: blr ends a cycle; a bl to a function of the same module may be inlined, so it does not.
        blocked = {i for i, ins in enumerate(insns) if ins.kind in ("blr", "trap")}
        opaque = {i for i, ins in enumerate(insns) if ins.kind == "bl"}
        edge = next(i for i, ins in enumerate(insns) if ins.kind in ("b", "bcond") and ins.target <= BASE + 4 * i)
        head = (insns[edge].target - BASE) // 4
        keep = loops.needs_barrier(ops, texts, succ, loops.predecessors(succ), head, edge, blocked, opaque, (BASE, end))
        if keep != what.endswith("(keep)"):
            wrong.append(f"{what}: {'kept' if keep else 'dropped'}")
    return wrong


def check_chunk(words: list[int]) -> tuple[Counter, list[tuple[str, int, str]]]:
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
    from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True
    lifter = Lifter(BASE, "c")
    kinds, bad = Counter(), []
    for w in words:
        try:
            ins = lifter.lift(w, BASE)
        except Unsupported:
            continue
        if ins.kind != "op":
            continue  # gen.py decodes only 'op' words; branches and calls are classified by the lifter's kind
        op = loops.decode(w, BASE)
        kinds[op.kind] += 1
        d = next(cs.disasm(w.to_bytes(4, "little"), BASE), None)
        text = f"{d.mnemonic} {d.op_str}" if d else "(none)"
        mem = [o for o in d.operands if o.type == ARM64_OP_MEM] if d else []
        if op.kind in ("load", "store", "add", "mov", "addreg") and regs.written(ins.code) != op.writes():
            bad.append(
                (
                    "writes differ from the lifted C",
                    w,
                    f"{text}: {sorted(op.writes())} vs {sorted(regs.written(ins.code))}",
                )
            )
        if op.kind in ("other", "add", "mov", "addreg") and mem:
            bad.append(("memory instruction not decoded as one", w, text))
        if op.kind not in ("load", "store"):
            if op.kind == "add" and d:
                # add/sub/adds/subs/cmp/cmn/mov (to or from SP): Rd (or none), Rn, imm.
                regs_ = [reg_index(d.reg_name(o.reg)) for o in d.operands if o.type == ARM64_OP_REG]
                imms = [o.imm for o in d.operands if o.type == ARM64_OP_IMM]
                shift = [o.shift.value for o in d.operands if o.type == ARM64_OP_IMM and o.shift.type]
                imm = (imms[0] << (shift[0] if shift else 0)) if imms else 0
                if d.mnemonic in ("sub", "subs", "cmp"):
                    imm = -imm
                if d.mnemonic in ("cmp", "cmn"):
                    regs_ = [None, *regs_]
                want = (regs_[0], regs_[1], imm & M64)
                if (op.rd, op.rn, op.imm) != want:
                    bad.append(
                        (
                            "add operands",
                            w,
                            f"{text}: {(op.rd, op.rn, hex(op.imm))} vs {(want[0], want[1], hex(want[2]))}",
                        )
                    )
            if op.kind == "addreg" and d:
                regs_ = [reg_index(d.reg_name(o.reg)) for o in d.operands if o.type == ARM64_OP_REG]
                if d.mnemonic in ("cmp", "cmn", "tst"):
                    regs_ = [None, *regs_]
                elif d.mnemonic in ("neg", "negs"):
                    regs_ = [regs_[0], None, regs_[1]]
                if (op.rd, op.rn, op.rm) != tuple(regs_) or d.mnemonic not in (
                    "add",
                    "adds",
                    "sub",
                    "subs",
                    "cmp",
                    "cmn",
                    "neg",
                    "negs",
                ):
                    bad.append(("addreg operands", w, f"{text}: {(op.rd, op.rn, op.rm)}"))
            if op.kind == "mov" and d:
                regs_ = [reg_index(d.reg_name(o.reg)) for o in d.operands if o.type == ARM64_OP_REG]
                if (op.rd, op.rm) != (regs_[0], regs_[-1]) or d.mnemonic not in ("mov", "orr"):
                    bad.append(("mov operands", w, f"{text}: {(op.rd, op.rm)}"))
            continue
        if op.lit is not None:
            imms = [o.imm for o in d.operands if o.type == ARM64_OP_IMM]
            if not imms or (imms[-1] & M64) != op.lit:
                bad.append(("literal address", w, f"{text}: {op.lit:#x}"))
            continue
        if len(mem) != 1:
            bad.append(("no single memory operand", w, text))
            continue
        m = mem[0].mem
        base = reg_index(d.reg_name(m.base))
        index = reg_index(d.reg_name(m.index)) if m.index else None
        post = [o.imm for o in d.operands if o.type == ARM64_OP_IMM]
        rm = None if op.rm is None or op.rm == 31 else op.rm
        if base != op.rn or index != rm:
            bad.append(("address registers", w, f"{text}: base {op.rn} index {op.rm}"))
        if op.rm is None:
            # Offset of the access: imm for offset and pre-index forms, 0 for post-index.
            want = 0 if op.wb is not None and not op.pre else op.imm
            if (m.disp & M64) != want:
                bad.append(("offset", w, f"{text}: {want:#x}"))
        if bool(d.writeback) != (op.wb is not None):
            bad.append(("writeback", w, f"{text}: {op.wb}"))
        elif op.wb is not None:
            amount = post[-1] if (not op.pre and post) else m.disp
            if (amount & M64) != op.wb:
                bad.append(("writeback amount", w, f"{text}: {op.wb:#x}"))
    return kinds, bad


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exefs", type=Path, default=DEFAULT_EXEFS)
    ap.add_argument("--modules", default="main,sdk,subsdk0")
    ap.add_argument("--jobs", type=int, default=0, help="worker processes (default: all cores)")
    args = ap.parse_args()
    jobs = args.jobs or os.cpu_count() or 4
    words = np.unique(np.concatenate([text_words(args.exefs / m) for m in args.modules.split(",")])).tolist()
    kinds, bad = Counter(), []
    with ProcessPoolExecutor(jobs) as pool:
        for k, b in pool.map(check_chunk, [words[i::jobs] for i in range(jobs)]):
            kinds += k
            bad += b
    print(f"{len(words)} distinct words; lifted 'op' words by decoded kind: {dict(sorted(kinds.items()))}")
    by = Counter(kind for kind, _, _ in bad)
    for kind, n in sorted(by.items()):
        print(f"{kind}: {n}")
        for _, w, text in [b for b in bad if b[0] == kind][:20]:
            print(f"  {w:08x} {text}")
    print("mismatches:", len(bad))
    wrong = check_cases()
    print(f"loop cases: {len(CASES) - len(wrong)} of {len(CASES)} right")
    for w in wrong:
        print(f"  {w}")
    sys.exit(1 if bad or wrong else 0)


if __name__ == "__main__":
    main()
