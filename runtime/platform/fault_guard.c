// A copy that may fault without running the fault hooks (Linux, PS5): plat_mem_copy_guarded returns 0 when the copy
// faulted, and the platform's fault handler jumps back to it (plat_fault_guarded) before any hook runs.
//
// Why: the PS5's plat_mem_read (plat_mem_peek there) copies guest memory with vm_lock held. A page the early-read watch
// made inaccessible faulted inside that copy, and the fault hook (memory.c thaw_fault) took frz_lock, which a game
// thread held in mem_early_watch while waiting for vm_lock (plat_mem_state): run 25 (14/11/2026) hung with the replay
// "in a submission" for 70 s. A copy that faults now fails instead, with no lock taken in the handler, and the caller
// reads the page the plain way, holding nothing the fault path needs (mem_copy_out).
#if defined(__linux__) || defined(__PROSPERO__)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   // (REG_RIP and the other gregs indexes)
#endif
#include <fcntl.h>
#include <setjmp.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "platform.h"

static _Thread_local sigjmp_buf *guard_jmp;

int plat_mem_copy_guarded(void *dst, const void *src, size_t n) {
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1)) {   // 1: the signal mask comes back as it was (the handler may run with SIGSEGV blocked)
        __atomic_store_n(&guard_jmp, NULL, __ATOMIC_RELAXED);
        return 0;
    }
    __atomic_store_n(&guard_jmp, &jb, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    memcpy(dst, src, n);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&guard_jmp, NULL, __ATOMIC_RELAXED);
    return 1;
}

void plat_fault_guarded(void) {
    sigjmp_buf *j = __atomic_load_n(&guard_jmp, __ATOMIC_RELAXED);
    if (!j) return;
    __atomic_store_n(&guard_jmp, NULL, __ATOMIC_RELAXED);
    siglongjmp(*j, 1);
}

// One instruction let through (plat_fault_step, platform.h): the fault hook asks for it, the platform's fault handler
// sets the trap flag in the context it returns to (plat_fault_step_arm), and the debug trap after that one
// instruction runs what the hook asked (plat_fault_step_trap, from the SIGTRAP handler on the same thread). Several
// requests of one instruction (it faulted again on another page before it completed) run at that one trap.
#if defined(__x86_64__)
#define STEP_MAX 8
#define TF 0x100ull
static _Thread_local struct { void (*fn)(uint64_t); uint64_t arg; } step_q[STEP_MAX];
static _Thread_local int step_n, step_req;
static int step_works = -1;   // the self-test (plat_fault_step_init): 1 a trap came after one instruction
static volatile int step_testing;
static uint64_t step_stray;

int plat_fault_step(void (*fn)(uint64_t arg), uint64_t arg) {
    if (__atomic_load_n(&step_works, __ATOMIC_RELAXED) != 1 || step_n == STEP_MAX) return 0;
    step_q[step_n].fn = fn, step_q[step_n].arg = arg;
    step_n++, step_req = 1;
    return 1;
}

void plat_fault_step_arm(uint64_t *rflags) {
    if (step_req) *rflags |= TF, step_req = 0;
}

int plat_fault_step_trap(uint64_t *rflags) {
    if (step_testing) return *rflags &= ~TF, step_testing = 0, __atomic_store_n(&step_works, 1, __ATOMIC_RELAXED), 1;
    if (!(*rflags & TF)) return 0;   // (not a single step: someone else's SIGTRAP)
    *rflags &= ~TF;
    int n = step_n;
    step_n = 0;
    for (int i = 0; i < n; i++) step_q[i].fn(step_q[i].arg);
    if (!n) __atomic_fetch_add(&step_stray, 1, __ATOMIC_RELAXED);
    return 1;
}

// After the platform installed its SIGTRAP handler: one popf with the trap flag; the trap after the next instruction
// says the kernel single-steps user code (FreeBSD and Linux both let sigreturn and popf set it; untried on a PS5
// before 17/11/2026, hence the test rather than an assumption).
int plat_fault_step_init(void) {
    if (step_works >= 0) return step_works;
    step_testing = 1;
    __asm__ volatile("pushfq\n\torq $0x100, (%%rsp)\n\tpopfq\n\tnop" ::: "memory", "cc");
    if (step_testing) step_testing = 0, __atomic_store_n(&step_works, 0, __ATOMIC_RELAXED);
    return step_works;
}

// One load emulated (plat_fault_emulate_load, platform.h): the faulting instruction decoded at the context's rip,
// its bytes read by rd, its destination register written in the context and rip moved past it. Only the forms the
// game's sibling-page readers use (the PS5 race's six early-read sites, 17/11/2026: mov r64/r32 from [base+disp8/32],
// movzx r32 from a byte or word) and their near relatives: MOV r/m (8B, and 8A without the high-byte registers),
// MOVZX/MOVSX (0F B6/B7/BE/BF), MOVSXD (REX.W 63); any ModRM/SIB addressing, rip-relative too; prefixes 66 and REX
// only. Anything else (VEX, other opcodes, segment or lock prefixes) returns 0 and the caller does what it did before.
_Thread_local void *plat_fault_uc;

static uint64_t *ctx_gpr(void *uc, int r) {
    ucontext_t *u = uc;
#ifdef __PROSPERO__
    mcontext_t *m = &u->uc_mcontext;
    switch (r) {
    case 0: return (uint64_t *)&m->mc_rax;
    case 1: return (uint64_t *)&m->mc_rcx;
    case 2: return (uint64_t *)&m->mc_rdx;
    case 3: return (uint64_t *)&m->mc_rbx;
    case 4: return (uint64_t *)&m->mc_rsp;
    case 5: return (uint64_t *)&m->mc_rbp;
    case 6: return (uint64_t *)&m->mc_rsi;
    case 7: return (uint64_t *)&m->mc_rdi;
    case 8: return (uint64_t *)&m->mc_r8;
    case 9: return (uint64_t *)&m->mc_r9;
    case 10: return (uint64_t *)&m->mc_r10;
    case 11: return (uint64_t *)&m->mc_r11;
    case 12: return (uint64_t *)&m->mc_r12;
    case 13: return (uint64_t *)&m->mc_r13;
    case 14: return (uint64_t *)&m->mc_r14;
    case 15: return (uint64_t *)&m->mc_r15;
    default: return (uint64_t *)&m->mc_rip;   // 16: rip
    }
#else
    static const int map[17] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI, REG_R8,
                                REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RIP};
    return (uint64_t *)&u->uc_mcontext.gregs[map[r]];
#endif
}

// The instruction bytes the decoder reads. The PS5 maps the eboot's text execute-only: a read of the code at rip
// stopped the title (SYSTEM_XO_VIOLATION, emu-on-20261117-155644, in this decoder). There they come from the file
// the code was loaded from (plat_code_file_init: /app0/eboot.bin, a fake SELF whose segments are stored plain, each
// at its entry's offset; or a plain ELF), read with pread, which is async-signal-safe; elsewhere from memory.
#define CODE_SEGS 16
static int code_fd = -1;
static struct { uint64_t lo, hi, off; } code_seg[CODE_SEGS];
static int ncode_seg;

static int pread_all(int fd, void *dst, size_t n, uint64_t off) {
    for (size_t done = 0; done < n;) {
        ssize_t k = pread(fd, (uint8_t *)dst + done, n - done, (off_t)(off + done));
        if (k <= 0) return 0;
        done += (size_t)k;
    }
    return 1;
}

static uint64_t rd_u(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

int plat_code_file_init(const char *path, uint64_t base) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint8_t h[64], e[32], ph[56];
    uint64_t elf = 0, ne = 0;
    int self = 0;
    if (!pread_all(fd, h, sizeof h, 0)) goto fail;
    if (h[0] == 0x4f && h[1] == 0x15 && h[2] == 0x3d && h[3] == 0x1d) {   // SELF: 32-byte header, 32-byte entries
        self = 1, ne = rd_u(h + 24, 2), elf = 32 + 32 * ne;
        if (!pread_all(fd, h, sizeof h, elf)) goto fail;
    }
    if (memcmp(h, "\x7f" "ELF", 4) || h[4] != 2) goto fail;
    // (loaded at base unless ET_EXEC or the PS5's ET_SCE_EXEC: the eboot is ET_SCE_DYNEXEC 0xfe10, at VA 0)
    uint64_t type = rd_u(h + 16, 2), add = type == 2 || type == 0xfe00 ? 0 : base, phoff = rd_u(h + 32, 8);
    unsigned phnum = (unsigned)rd_u(h + 56, 2);
    ncode_seg = 0;
    if (self) {
        for (uint64_t i = 0; i < ne && ncode_seg < CODE_SEGS; i++) {
            if (!pread_all(fd, e, sizeof e, 32 + 32 * i)) goto fail;
            uint64_t props = rd_u(e, 8), seg = props >> 20 & 0xffff;
            if (!(props >> 11 & 1) || (props >> 1 & 1) || (props >> 3 & 1) || seg >= phnum) continue;   // data, plain
            if (!pread_all(fd, ph, sizeof ph, elf + phoff + 56 * seg) || rd_u(ph, 4) != 1) continue;
            uint64_t va = rd_u(ph + 16, 8) + add;
            code_seg[ncode_seg].lo = va, code_seg[ncode_seg].hi = va + rd_u(ph + 32, 8);
            code_seg[ncode_seg++].off = rd_u(e + 8, 8);
        }
    } else {
        for (unsigned i = 0; i < phnum && ncode_seg < CODE_SEGS; i++) {
            if (!pread_all(fd, ph, sizeof ph, phoff + 56 * i) || rd_u(ph, 4) != 1) continue;
            uint64_t va = rd_u(ph + 16, 8) + add;
            code_seg[ncode_seg].lo = va, code_seg[ncode_seg].hi = va + rd_u(ph + 32, 8);
            code_seg[ncode_seg++].off = rd_u(ph + 8, 8);
        }
    }
    if (!ncode_seg) goto fail;
    if (code_fd >= 0) close(code_fd);
    __atomic_store_n(&code_fd, fd, __ATOMIC_RELEASE);
    return 1;
fail:
    close(fd);
    return 0;
}

static int code_read(uint64_t a, uint8_t *dst, int n) {   // the bytes read, at most n (0 none)
    int fd = __atomic_load_n(&code_fd, __ATOMIC_ACQUIRE);
    if (fd < 0) return memcpy(dst, (const void *)(uintptr_t)a, (size_t)n), n;
    for (int i = 0; i < ncode_seg; i++)
        if (a >= code_seg[i].lo && a < code_seg[i].hi) {
            if ((uint64_t)n > code_seg[i].hi - a) n = (int)(code_seg[i].hi - a);
            return pread_all(fd, dst, (size_t)n, code_seg[i].off + (a - code_seg[i].lo)) ? n : 0;
        }
    return 0;
}

// Compares bytes of this file's read-only data read through code_read with memory: the segment table and load base
// are right (the text itself cannot be read in memory on the PS5 to compare). 1 good.
static const char code_probe[32] = "mkw-recomp code read check 0123";
int plat_code_check(void) {
    uint8_t b[sizeof code_probe];
    return code_read((uintptr_t)code_probe, b, (int)sizeof b) == (int)sizeof b && !memcmp(b, code_probe, sizeof b);
}

int plat_code_init(void) {
#ifdef __PROSPERO__
    char path[256];
    if (!plat_exe_path(path, sizeof path) || !plat_code_file_init(path, plat_exe_base())) return 0;
#endif
    return plat_code_check();
}

static _Thread_local struct { uint64_t rip; uint8_t n, b[15]; } code_cache[16];   // per thread: no lock in the handler

int plat_fault_emulate_load(uint64_t fault_addr, int (*rd)(uint64_t ea, size_t n, void *dst)) {
    void *uc = plat_fault_uc;
    if (!uc) return 0;
    uint64_t *rip = ctx_gpr(uc, 16);
    int slot = (int)((*rip >> 1) % 16);
    if (code_cache[slot].rip != *rip || !code_cache[slot].n) {
        int got = code_read(*rip, code_cache[slot].b, 15);
        if (got <= 0) return 0;
        code_cache[slot].rip = *rip, code_cache[slot].n = (uint8_t)got;
    }
    const uint8_t *ib = code_cache[slot].b;
    int len = code_cache[slot].n, i = 0, o16 = 0, rex = 0;
#define NEXT() (i < len ? ib[i++] : -1)
    for (;;) {   // prefixes: 66 and one REX last
        if (i > 4 || i >= len) return 0;
        if (ib[i] == 0x66) o16 = 1, i++;
        else if ((ib[i] & 0xf0) == 0x40) { rex = ib[i++]; break; }
        else break;
    }
    int w = rex & 8, size, sx = 0, op = NEXT();
    if (op == 0x8b) size = w ? 8 : o16 ? 2 : 4;
    else if (op == 0x8a) size = 1;
    else if (op == 0x63 && w) size = 4, sx = 1;
    else if (op == 0x0f) {
        int op2 = NEXT();
        if (op2 != 0xb6 && op2 != 0xb7 && op2 != 0xbe && op2 != 0xbf) return 0;
        size = (op2 & 1) ? 2 : 1, sx = op2 >= 0xbe, op = 0x0f00 | op2;
    } else return 0;
    int modrm = NEXT();
    if (modrm < 0) return 0;
    int mod = modrm >> 6, reg = (modrm >> 3 & 7) | (rex & 4) << 1, rm = modrm & 7;
    if (mod == 3) return 0;
    if (op == 0x8a && !rex && reg >= 4) return 0;   // (ah, ch, dh, bh)
    uint64_t ea = 0;
    int riprel = 0, nodisp32base = 0;
    if (rm == 4) {
        int sib = NEXT();
        if (sib < 0) return 0;
        int scale = sib >> 6, index = (sib >> 3 & 7) | (rex & 2) << 2, base = (sib & 7) | (rex & 1) << 3;
        if (index != 4) ea += *ctx_gpr(uc, index) << scale;
        if ((sib & 7) == 5 && mod == 0) nodisp32base = 1;
        else ea += *ctx_gpr(uc, base);
    } else if (rm == 5 && mod == 0) {
        riprel = 1;
    } else {
        ea += *ctx_gpr(uc, rm | (rex & 1) << 3);
    }
    if (mod == 1) {
        int d = NEXT();
        if (d < 0) return 0;
        ea += (uint64_t)(int64_t)(int8_t)d;
    } else if (mod == 2 || riprel || nodisp32base) {
        if (i + 4 > len) return 0;
        ea += (uint64_t)(int64_t)(int32_t)rd_u(ib + i, 4);
        i += 4;
    }
#undef NEXT
    if (riprel) ea += *rip + (uint64_t)i;   // (no immediate in these forms: the next instruction)
    if (ea > fault_addr || fault_addr - ea >= (uint64_t)size) return 0;   // (not the access that faulted)
    uint64_t v = 0;
    if (!rd(ea, (size_t)size, &v)) return 0;
    if (sx) v = size == 1 ? (uint64_t)(int64_t)(int8_t)v : size == 2 ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(int64_t)(int32_t)v;
    uint64_t *d = ctx_gpr(uc, reg);
    int dsize = op == 0x8a ? 1 : op == 0x8b ? size : w ? 8 : o16 ? 2 : 4;   // (movzx/movsx/movsxd: the operand size)
    if (dsize == 8) *d = v;
    else if (dsize == 4) *d = (uint32_t)v;   // (a 32-bit write clears the upper half)
    else if (dsize == 2) *d = (*d & ~0xffffull) | (v & 0xffff);
    else *d = (*d & ~0xffull) | (v & 0xff);
    *rip += (uint64_t)i;
    return 1;
}
#else
_Thread_local void *plat_fault_uc;
int plat_code_file_init(const char *path, uint64_t base) { return (void)path, (void)base, 0; }
int plat_code_check(void) { return 0; }
int plat_code_init(void) { return 0; }
int plat_fault_emulate_load(uint64_t fault_addr, int (*rd)(uint64_t ea, size_t n, void *dst)) {
    return (void)fault_addr, (void)rd, 0;
}
int plat_fault_step(void (*fn)(uint64_t arg), uint64_t arg) { return (void)fn, (void)arg, 0; }
void plat_fault_step_arm(uint64_t *rflags) { (void)rflags; }
int plat_fault_step_trap(uint64_t *rflags) { return (void)rflags, 0; }
int plat_fault_step_init(void) { return 0; }
#endif
#endif
