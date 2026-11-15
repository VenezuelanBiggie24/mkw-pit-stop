"""Which loop back edges need the compiler barrier, for recomp/gen.py.

gen.py lifts guest loads to plain C loads, so clang may treat one another thread changes as loop-invariant: hoist it
out of the loop (LICM), or carry its value around the back edge (GVN-PRE, jump threading), and leave a loop that
never reads memory again: a `jmp` to itself at main+0x27ebb20 (spin_waits.py finds the plain spin waits; this was
not one). So every back edge of a loop without calls got `__asm__ volatile("" ::: "memory")`. The barrier also makes
clang write back and reload every Cpu field (c->x[], c->v[]) and every guest value it held in a register on each
trip, and stops it keeping loop-invariant state in registers.

The hazard needs a cycle that reads the same address on every trip with nothing in between that may write it. A
backward jump with no path from its target back to it (a shared epilogue) has no cycle at all. A cycle through a
call clang cannot see into (blr, a call outside the module: both rt_dispatch; an SVC) is not one: the callee may
write any memory, so clang reloads after it. A bl to a generated function is not such a call, since clang may inline
it from the same chunk. A cycle on which every load and store addresses memory that changes each trip is not one
either: clang can only reuse a value it read at an address it proves equal, and an address that moves each trip is
read anew. Such a loop ends or not by what the guest's own reads find, as on the console. needs_barrier() keeps the
barrier unless it proves one of these for every cycle through the back edge; what it cannot decode or prove keeps it.

A register's value, followed forward over the call-free cycles from the loop head (must: equal on every path):
  (r, off, w)  r's value at the loop head plus off, truncated to w bits (w = 32 after a W-register add or mov);
  'C'          a load's result clang cannot know (fresh each trip), that plus a constant, or plus a register the
               loop does not write;
  'X'          anything else.
An address changes each trip if its base is 'C', or (r, off, w) where r at the back edge is (r, d, _) with the low
32 bits of d not zero (an induction: x9 += 0x140) or 'C' (a list walk: x9 = [x9 + 8]); a register-offset address
needs one of base and index changing and the other not written in the loop. A load's result is 'C' only if no store
in the cycles is as wide as it: otherwise clang could forward a stored value (store x20 to [x9], load it back,
and the next load's address is x20, invariant). The analysis takes every (r, off, w) address as changing while it
labels load results, then checks each address against the registers at the back edge; one failure keeps the barrier.

Atomics, exclusives, fences, DC ZVA, post-indexed SIMD structure loads and stores, patched sites (two alternatives),
calls that may be inlined and anything the decoder does not know keep the barrier. Decoded writes are compared with
what the lifted C writes (regs.written); a mismatch keeps the barrier too. tools/cpu/loop_decode_check.py checks the
decoder against capstone and needs_barrier() on small loops with known answers.
"""
import re

import regs

M64 = (1 << 64) - 1
SP = regs.SP
# C that touches guest memory or orders it, in a lifted instruction the decoder does not classify as a load or store.
MEMORY = re.compile(r'\b(?:LD|ST)(?:8|16|32|64|128)\(|RT_GUEST_|__atomic|\b(?:ld|st)[ax]|\bstl\d|\blda\d|'
                    r'\batomic_|\bmem(?:set|cpy|move)\b|\brt_')


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Op:
    """One decoded instruction. kind: 'load', 'store', 'add' (rd = rn + imm), 'mov' (rd = rm), 'addreg'
    (rd = rn +- f(rm)), 'none' (no register or memory effect), 'other' (writes regs.written() registers, no memory),
    'mem' (memory the analysis does not model)."""
    __slots__ = ('kind', 'rd', 'rn', 'rm', 'imm', 'w', 'sizes', 'dests', 'wb', 'pre', 'lit')

    def __init__(self, kind, rd=None, rn=None, rm=None, imm=0, w=64, sizes=(), dests=(), wb=None, pre=False,
                 lit=None):
        self.kind, self.rd, self.rn, self.rm, self.imm, self.w = kind, rd, rn, rm, imm, w
        self.sizes, self.dests, self.wb, self.pre, self.lit = sizes, dests, wb, pre, lit

    def writes(self):
        out = set(self.dests)
        if self.wb is not None:
            out.add(self.rn)
        if self.kind in ('add', 'mov', 'addreg') and self.rd is not None:
            out.add(self.rd)
        return out


def _gpr(r):
    """A data register operand: XZR (31) is no register."""
    return None if r == 31 else r


def decode(w, pc):
    """Op for the instruction word w at pc. Only what the barrier decision needs is decoded exactly."""
    # ADD/SUB (immediate), with or without S: rd = rn +- imm. rn is SP for 31; rd is SP without S, XZR with it.
    if (w & 0x1F800000) == 0x11000000:
        sf, op, S = bits(w, 31, 1), bits(w, 30, 1), bits(w, 29, 1)
        imm = bits(w, 10, 12) << (12 if bits(w, 22, 1) else 0)
        rd = bits(w, 0, 5)
        rd = (None if rd == 31 else rd) if S else rd
        return Op('add', rd=rd, rn=bits(w, 5, 5), imm=(-imm if op else imm) & M64, w=64 if sf else 32)
    # ORR (shifted register) from XZR with no shift: MOV rd, rm.
    if (w & 0x7F20FC00) == 0x2A000000 and bits(w, 5, 5) == 31:
        return Op('mov', rd=_gpr(bits(w, 0, 5)), rm=_gpr(bits(w, 16, 5)), w=64 if bits(w, 31, 1) else 32)
    # ADD/SUB (shifted register), 64-bit: rd = rn +- (rm shifted).
    if (w & 0x9F200000) == 0x8B000000 and bits(w, 22, 2) != 3:
        return Op('addreg', rd=_gpr(bits(w, 0, 5)), rn=_gpr(bits(w, 5, 5)), rm=_gpr(bits(w, 16, 5)))
    if bits(w, 25, 1) != 0 or bits(w, 27, 1) != 1:
        return Op('other')
    return _ldst(w, pc)


def _ldst(w, pc):
    V, rn, rt = bits(w, 26, 1), bits(w, 5, 5), bits(w, 0, 5)
    if bits(w, 24, 6) == 0b001000:   # exclusive, acquire, release, CAS
        return Op('mem')
    if bits(w, 27, 3) == 0b011 and bits(w, 24, 2) == 0:   # LDR (literal)
        opc = bits(w, 30, 2)
        if not V and opc == 3:
            return Op('none')   # PRFM
        size = (4, 8, 16)[opc] if V else (4, 8, 4)[opc]
        return Op('load', sizes=(size,), dests=() if V else tuple(r for r in [_gpr(rt)] if r is not None),
                  lit=(pc + sext(bits(w, 5, 19), 19) * 4) & M64)
    if bits(w, 27, 3) == 0b101:   # pairs
        opc, mode, L = bits(w, 30, 2), bits(w, 23, 2), bits(w, 22, 1)
        if V:
            if opc == 3:
                return Op('mem')
            size = 4 << opc
        else:
            if opc == 3 or (opc == 1 and not L):   # STGP and reserved
                return Op('mem')
            size = 8 if opc == 2 else 4
        off = (sext(bits(w, 15, 7), 7) * size) & M64
        dests = () if V or not L else tuple(r for r in (_gpr(rt), _gpr(bits(w, 10, 5))) if r is not None)
        return Op('load' if L else 'store', rn=rn, imm=off, sizes=(size, size), dests=dests,
                  wb=off if mode in (1, 3) else None, pre=mode != 1)
    if bits(w, 27, 3) == 0b111:   # single register
        size, opc = bits(w, 30, 2), bits(w, 22, 2)
        if V:
            nbytes = 16 if (opc & 2) and size == 0 else 1 << size
            load = bool(opc & 1)
            if (opc & 2) and size != 0:
                return Op('mem')
        else:
            if size == 3 and opc >= 2:
                return Op('none')   # PRFM (the lifter lifts opc 3 as one too)
            if size == 2 and opc == 3:
                return Op('mem')
            nbytes, load = 1 << size, opc != 0
        dests = () if V or not load else tuple(r for r in [_gpr(rt)] if r is not None)
        kind = 'load' if load else 'store'
        if bits(w, 24, 2) == 1:   # unsigned offset
            return Op(kind, rn=rn, imm=bits(w, 10, 12) << (4 if nbytes == 16 else size), sizes=(nbytes,),
                      dests=dests, pre=True)
        if bits(w, 24, 2) != 0:
            return Op('mem')
        mode = bits(w, 10, 2)
        if bits(w, 21, 1):
            if mode != 2:
                return Op('mem')   # LSE atomics, LDRAA/LDRAB
            if bits(w, 13, 3) not in (2, 3, 6, 7):
                return Op('mem')
            return Op(kind, rn=rn, rm=bits(w, 16, 5), sizes=(nbytes,), dests=dests, pre=True)
        imm9 = sext(bits(w, 12, 9), 9) & M64
        if mode in (0, 2):   # unscaled, unprivileged
            return Op(kind, rn=rn, imm=imm9, sizes=(nbytes,), dests=dests, pre=True)
        return Op(kind, rn=rn, imm=imm9, sizes=(nbytes,), dests=dests, wb=imm9, pre=mode == 3)
    if bits(w, 31, 1) == 0 and bits(w, 24, 6) in (0b001100, 0b001101) and V:   # SIMD structures
        if bits(w, 23, 1):
            return Op('mem')   # the post-index amount depends on the structure; not worth decoding
        # At most four 16-byte registers.
        return Op('load' if bits(w, 22, 1) else 'store', rn=rn, sizes=(64,), pre=True)
    return Op('mem')


def predecessors(succ):
    p = [[] for _ in succ]
    for i, ss in enumerate(succ):
        for t in ss:
            p[t].append(i)
    return p


def _form_add(v, imm, width):
    """Value v plus imm, as a width-bit operation (zero-extended)."""
    if v == 'C':
        return 'C'
    if isinstance(v, tuple):
        r, off, vw = v
        if width == 64:
            return (r, (off + imm) & M64, 64) if vw == 64 else 'X'
        return (r, (off + imm) & 0xFFFFFFFF, 32)
    return 'X'


def _join(a, b):
    return a if a == b else 'X'


def needs_barrier(ops, texts, succ, preds, head, edge, blocked, opaque, text_range):
    """True if the back edge edge -> head (instruction indices) keeps the compiler barrier.

    ops[i]: decode() of instruction i; texts[i]: its lifted C; succ: recomp/gen.py Emitter.flow(), preds: its
    inverse (predecessors()); blocked: instructions that end a cycle (calls clang cannot see into, SVCs); opaque:
    instructions that keep the barrier if a cycle without those reaches them (patched sites, inlinable calls);
    text_range: (lo, hi) of the read-only code whose literal loads another thread cannot change."""
    if head in blocked or edge in blocked:
        return False   # every cycle through this edge passes a call
    # Instructions that reach edge without passing head or a call (walking predecessors back from edge).
    back, work = {edge}, [edge]
    while work:
        i = work.pop()
        if i == head:
            continue
        for p in preds[i]:
            if p not in back and p not in blocked:
                back.add(p)
                work.append(p)
    if head not in back:
        return False   # no call-free path from head to edge
    # ... that head reaches without a call: the call-free cycles through the edge.
    body, work = {head}, [head]
    while work:
        i = work.pop()
        for s in succ[i]:
            if s in back and s not in body and s != head:
                body.add(s)
                work.append(s)
    if edge not in body:
        return False
    written = set()
    widest_store = 0
    for i in body:
        op = ops[i]
        if i in opaque or op.kind == 'mem':
            return True
        if op.kind == 'other':
            if MEMORY.search(texts[i]):
                return True
            written |= regs.written(texts[i])
            continue
        if regs.written(texts[i]) != op.writes():
            return True   # the decoder and the lifter disagree: do not trust either
        written |= op.writes()
        if op.kind == 'store':
            widest_store = max(widest_store, *op.sizes)

    # Must-forward analysis over the body from the head; the head's state is every register's own value.
    start = [(r, 0, 64) for r in range(32)]
    state = {head: start}
    order = sorted(body)
    checks = []   # (instruction, address values) to verify at the end
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        if rounds > 50:
            return True
        checks = []
        for i in order:
            s = state.get(i)
            if s is None:
                continue
            s = list(s)
            op = ops[i]
            k = op.kind
            if k == 'add':
                if op.rd is not None:
                    s[op.rd] = _form_add(s[op.rn], op.imm, op.w)
            elif k == 'mov':
                if op.rd is not None:
                    s[op.rd] = 'X' if op.rm is None else _form_add(s[op.rm], 0, op.w)
            elif k == 'addreg':
                if op.rd is not None:
                    a = s[op.rn] if op.rn is not None else 'X'
                    b = s[op.rm] if op.rm is not None else 'X'
                    fixed_a = op.rn is None or op.rn not in written
                    fixed_b = op.rm is None or op.rm not in written
                    s[op.rd] = 'C' if (a == 'C' and fixed_b) or (b == 'C' and fixed_a) else 'X'
            elif k in ('load', 'store'):
                if op.lit is not None:
                    if not text_range[0] <= op.lit < text_range[1]:
                        return True   # a literal outside the code: a fixed address that may change
                else:
                    checks.append((i, s[op.rn], op.rn, op.rm, s[op.rm] if op.rm is not None else None))
                    if op.wb is not None:
                        s[op.rn] = _form_add(s[op.rn], op.wb, 64)
                for r in op.dests:
                    s[r] = 'C' if min(op.sizes) > widest_store else 'X'
            elif k == 'other':
                for r in regs.written(texts[i]):
                    s[r] = 'X'
            for t in succ[i]:
                if t not in body or t == head:
                    continue
                old = state.get(t)
                new = s if old is None else [_join(x, y) for x, y in zip(old, s)]
                if new != old:
                    state[t] = new
                    changed = True
    at_edge = state.get(edge)
    if at_edge is None:
        return True

    def moves(v):
        """v changes every trip."""
        if v == 'C':
            return True
        if isinstance(v, tuple):
            e = at_edge[v[0]]
            if e == 'C':
                return True
            return isinstance(e, tuple) and e[0] == v[0] and (e[1] & 0xFFFFFFFF) != 0
        return False

    for i, base, rn, rm, index in checks:
        if rm is None or rm == 31:
            ok = moves(base)
        elif rm not in written:
            ok = moves(base)
        elif rn not in written:
            ok = moves(index)
        else:
            ok = False
        if not ok:
            return True
    return False


def depth_weights(succ, insns, entry, factor, cap=2):
    """Per instruction, factor ** (how many loops hold it, at most cap): recomp/regs.py's weight of a register use
    (gen.py --reg-loop-weight). A loop is the natural loop of each backward branch inside the function."""
    preds = predecessors(succ)
    bodies = {}   # head -> body: back edges to one head make one loop
    for i, ins in enumerate(insns):
        if ins.kind not in ('b', 'bcond') or not entry <= ins.target <= entry + 4 * i:
            continue
        head = (ins.target - entry) // 4
        body = bodies.setdefault(head, {head})
        work = [i] if i not in body else []
        body.add(i)
        while work:   # back from the branch to the head, which is in body already and so stops the walk
            j = work.pop()
            for p in preds[j]:
                if p not in body:
                    body.add(p)
                    work.append(p)
    depth = [0] * len(succ)
    for body in bodies.values():
        for j in body:
            depth[j] += 1
    return [factor ** min(d, cap) for d in depth]
