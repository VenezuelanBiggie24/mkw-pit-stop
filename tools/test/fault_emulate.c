// Checks plat_fault_emulate_load (runtime/platform/fault_guard.c) against the CPU. Why: a wrong flag from an emulated
// CMP sends the game down the wrong branch with nothing in the log, and the PS5 run that uses it takes minutes.
// Each case is a random instruction with a memory operand, run twice from the same registers and flags: once on the
// CPU with its page readable, once with the page inaccessible, so the fault handler emulates it. Registers, rflags and
// the rip after it must match. Forms the emulator must decline fall back to the CPU, and are counted.
// Linux x86-64, no game files:
//   clang -O1 -g -Iruntime/platform -Iruntime/core/base tools/test/fault_emulate.c -o build/fault_emulate && build/fault_emulate
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../../runtime/platform/fault_guard.c"

enum { PAGE_SZ = 4096, NREG = 16, RSP = 4 };
typedef struct { uint64_t r[NREG], flags; } Regs;

static uint8_t *code, *data, shadow[PAGE_SZ];
static int code_len, allow_cmp, emulating, emulated, declined, bad_rip;

// Loads every register but rsp from r and the flags, calls code, stores them back.
__attribute__((naked)) static void run(Regs *r, void *fn) {
    __asm__(
        "push %rbx; push %rbp; push %r12; push %r13; push %r14; push %r15\n"
        "push %rsi; push %rdi\n"
        "pushq 128(%rdi); popfq\n"
        "mov 0(%rdi),%rax; mov 8(%rdi),%rcx; mov 16(%rdi),%rdx; mov 24(%rdi),%rbx\n"
        "mov 40(%rdi),%rbp; mov 48(%rdi),%rsi\n"
        "mov 64(%rdi),%r8; mov 72(%rdi),%r9; mov 80(%rdi),%r10; mov 88(%rdi),%r11\n"
        "mov 96(%rdi),%r12; mov 104(%rdi),%r13; mov 112(%rdi),%r14; mov 120(%rdi),%r15\n"
        "mov 56(%rdi),%rdi\n"
        "call *8(%rsp)\n"
        "pushfq; push %rdi\n"
        "mov 16(%rsp),%rdi\n"
        "popq 56(%rdi); popq 128(%rdi)\n"
        "mov %rax,0(%rdi); mov %rcx,8(%rdi); mov %rdx,16(%rdi); mov %rbx,24(%rdi)\n"
        "mov %rbp,40(%rdi); mov %rsi,48(%rdi)\n"
        "mov %r8,64(%rdi); mov %r9,72(%rdi); mov %r10,80(%rdi); mov %r11,88(%rdi)\n"
        "mov %r12,96(%rdi); mov %r13,104(%rdi); mov %r14,112(%rdi); mov %r15,120(%rdi)\n"
        "add $16,%rsp\n"
        "pop %r15; pop %r14; pop %r13; pop %r12; pop %rbp; pop %rbx\n"
        "ret\n");
}

static int rd_shadow(uint64_t ea, size_t n, void *dst) {
    uint64_t off = ea - (uint64_t)data;
    if (off >= PAGE_SZ || off + n > PAGE_SZ) return 0;
    memcpy(dst, shadow + off, n);
    return 1;
}

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig;
    uint64_t a = (uint64_t)si->si_addr;
    if (!emulating || a < (uint64_t)data || a >= (uint64_t)data + PAGE_SZ) {
        static const char m[] = "unexpected fault\n";
        (void)!write(2, m, sizeof m - 1);
        _exit(2);
    }
    plat_fault_uc = uc;
    int ok = plat_fault_emulate_load(a, allow_cmp, rd_shadow);
    plat_fault_uc = NULL;
    if (ok) {
        emulated++;
        if (*ctx_gpr(uc, 16) != (uint64_t)code + (uint64_t)code_len) bad_rip++;
    } else {   // the CPU does it: the retry finds the page open
        declined++;
        mprotect(data, PAGE_SZ, PROT_READ | PROT_WRITE);
    }
}

static uint64_t rnd(void) {
    static uint64_t s = 0x9e3779b97f4a7c15;
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return s;
}

// Values that reach every flag: zero, one, the sign bits and their neighbours, or anything.
static uint64_t value(void) {
    static const uint64_t edge[] = {0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
                                    0xffffffff, 0x7fffffffffffffff, 0x8000000000000000, ~0ull, 0x10, 0x0f};
    return rnd() % 2 ? edge[rnd() % (sizeof edge / sizeof *edge)] : rnd();
}

typedef struct { int len, base, index, scale, sib_index, riprel; uint64_t off; int64_t disp; } Insn;

// Writes prefixes, opcode, ModRM, SIB and displacement, with the addressing chosen at random.
static Insn encode(int o66, int w, int op0f, int op, int digit, int imm_size, int force_rex) {
    uint8_t *p = code;
    Insn in = {0};
    int n = 0, field;
    in.off = 64 + rnd() % (PAGE_SZ - 128);
    do field = rnd() % NREG; while (field == RSP);
    do in.base = rnd() % NREG; while (in.base == RSP);
    do in.index = rnd() % NREG; while (in.index == RSP || in.index == in.base);
    if (digit >= 0) field = digit;
    int form = rnd() % 4;   // 0 [base], 1 [base+disp8], 2 [base+index*scale+disp32], 3 [rip+disp32]
    if (form == 0 && (in.base & 7) == 5) form = 1;   // (mod 0 with rbp or r13 means something else)
    in.riprel = form == 3;
    in.sib_index = form == 2;
    int sib = !in.riprel && (form == 2 || (in.base & 7) == 4);   // (rsp's encoding, r12 too, needs a SIB byte)
    int mod = in.riprel ? 0 : form == 2 ? 2 : form;
    in.scale = in.sib_index ? rnd() % 4 : 0;
    in.disp = form == 1 ? (int8_t)rnd() : form == 2 ? (int32_t)(rnd() % 0x10000) - 0x8000 : 0;
    int rex = (w ? 8 : 0) | (field > 7 && digit < 0 ? 4 : 0) | (in.sib_index && in.index > 7 ? 2 : 0) |
              (!in.riprel && in.base > 7 ? 1 : 0);
    if (o66) p[n++] = 0x66;
    if (rex || force_rex) p[n++] = (uint8_t)(0x40 | rex);
    if (op0f) p[n++] = 0x0f;
    p[n++] = (uint8_t)op;
    p[n++] = (uint8_t)(mod << 6 | (field & 7) << 3 | (in.riprel ? 5 : sib ? 4 : in.base & 7));
    if (sib) p[n++] = (uint8_t)(in.scale << 6 | (in.sib_index ? in.index & 7 : 4) << 3 | (in.base & 7));
    int dsize = in.riprel || mod == 2 ? 4 : mod, disp_at = n;
    n += dsize;
    in.len = n + imm_size;
    if (in.riprel) in.disp = (int64_t)(data + in.off) - (int64_t)(code + in.len);
    memcpy(p + disp_at, &in.disp, (size_t)dsize);
    uint64_t imm = value();
    memcpy(p + n, &imm, (size_t)imm_size);
    p[in.len] = 0xc3;   // ret
    return in;
}

// sized: operand size from REX.W and 66. must_w: REX.W always. kind 0 emulated always, 1 only with cmp, 2 never.
typedef struct { const char *name; int op0f, op, digit, imm, sized, must_w, kind; } Form;
static const Form forms[] = {
    {"cmp [m],r8", 0, 0x38, -1, 0, 0, 0, 1},     {"cmp [m],r", 0, 0x39, -1, 0, 1, 0, 1},
    {"cmp r8,[m]", 0, 0x3a, -1, 0, 0, 0, 1},     {"cmp r,[m]", 0, 0x3b, -1, 0, 1, 0, 1},
    {"cmp [m8],imm8", 0, 0x80, 7, 1, 0, 0, 1},   {"cmp [m],imm", 0, 0x81, 7, 4, 1, 0, 1},
    {"cmp [m],imm8", 0, 0x83, 7, 1, 1, 0, 1},
    {"mov r,[m]", 0, 0x8b, -1, 0, 1, 0, 0},      {"mov r8,[m]", 0, 0x8a, -1, 0, 0, 0, 0},
    {"movzx r,[m8]", 1, 0xb6, -1, 0, 0, 0, 0},   {"movzx r,[m16]", 1, 0xb7, -1, 0, 0, 0, 0},
    {"movsx r,[m8]", 1, 0xbe, -1, 0, 0, 0, 0},   {"movsx r,[m16]", 1, 0xbf, -1, 0, 0, 0, 0},
    {"movsxd r,[m32]", 0, 0x63, -1, 0, 0, 1, 0},
    {"or [m],imm8", 0, 0x83, 1, 1, 1, 0, 2},     {"sub [m8],imm8", 0, 0x80, 5, 1, 0, 0, 2},
    {"add [m],r", 0, 0x01, -1, 0, 1, 0, 2},      {"test [m],r", 0, 0x85, -1, 0, 1, 0, 2},
};

static int fails;

static void one(const Form *f) {
    int w = f->must_w || (f->sized && rnd() % 2), o66 = f->sized && !w && rnd() % 3 == 0;
    int imm = f->imm == 4 && o66 ? 2 : f->imm;
    Insn in = encode(o66, w, f->op0f, f->op, f->digit, imm, !f->must_w && rnd() % 2);
    code_len = in.len;
    Regs start;
    for (int i = 0; i < NREG; i++) start.r[i] = value();
    start.flags = 0x202 | (rnd() & 0x8d5);
    if (in.sib_index) start.r[in.index] = rnd() >> 40;
    if (!in.riprel)
        start.r[in.base] = (uint64_t)data + in.off - (uint64_t)in.disp - (in.sib_index ? start.r[in.index] << in.scale : 0);
    for (int i = 0; i < PAGE_SZ; i++) shadow[i] = (uint8_t)value();

    static uint8_t after[PAGE_SZ];
    Regs cpu = start, emu = start;
    memcpy(data, shadow, PAGE_SZ);
    run(&cpu, code);
    memcpy(after, data, PAGE_SZ);
    memcpy(data, shadow, PAGE_SZ);
    mprotect(data, PAGE_SZ, PROT_NONE);
    memset(code_cache, 0, sizeof code_cache);   // (kept by rip: the game's code never changes, this test's does)
    emulating = 1;
    run(&emu, code);
    emulating = 0;
    mprotect(data, PAGE_SZ, PROT_READ | PROT_WRITE);

    cpu.flags &= ~0x10000ull, emu.flags &= ~0x10000ull;   // (RF is the CPU's own)
    if (memcmp(&cpu, &emu, sizeof cpu) || memcmp(after, data, PAGE_SZ)) {
        if (fails++ < 10) {
            printf("MISMATCH %s:", f->name);
            for (int i = 0; i <= in.len; i++) printf(" %02x", code[i]);
            printf("  flags cpu %llx emulated %llx\n", (unsigned long long)cpu.flags, (unsigned long long)emu.flags);
        }
    }
}

int main(void) {
    uint8_t *m = mmap(NULL, 2 * PAGE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return perror("mmap"), 2;
    code = m, data = m + PAGE_SZ;
    mprotect(data, PAGE_SZ, PROT_READ | PROT_WRITE);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv, sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL), sigaction(SIGBUS, &sa, NULL);
    enum { RUNS = 4000 };
    for (allow_cmp = 1; allow_cmp >= 0; allow_cmp--)
        for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
            const Form *f = &forms[i];
            emulated = declined = 0;
            for (int n = 0; n < RUNS; n++) one(f);
            int want = f->kind == 0 || (f->kind == 1 && allow_cmp);
            if (want ? !emulated : emulated) fails++, printf("WRONG COVERAGE %s cmp=%d: emulated %d\n", f->name, allow_cmp, emulated);
            printf("%-16s cmp=%d emulated %5d, left to the CPU %5d\n", f->name, allow_cmp, emulated, declined);
        }
    if (bad_rip) fails++, printf("rip wrong after %d emulations\n", bad_rip);
    printf("fault_emulate: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
