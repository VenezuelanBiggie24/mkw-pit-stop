// The hook at the first line of every lifted function (recomp/gen.py), apart from cpu.h so tools/test/fn_enter.c can test
// this very code without the rest of the runtime.
#pragma once
#include <stdint.h>

// Function-entry profiler (core/diag/seqwatch.c, impcensus.c): one predictable branch per call when off. g_fnprof is
// defined beside the generated code (build.py modules.c): hidden, so the game code
// library reads them directly rather than through an import.
extern volatile int g_fnprof __attribute__((visibility("hidden")));
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);
// On the PS5 the call is out of line, behind a thunk. A C call at the first line of a function makes every leaf a
// non-leaf: `c` is saved across it and the registers pushed on the hot path too (10 of the 17 instructions of a small
// function, 3.4 % of the generated code, 19/11/2026). At a function's entry nothing but `c` is live, so the asm names the
// caller-saved registers as clobbered, and the compiler has nothing to save. The stub steps over the red zone first.
#if defined(__PROSPERO__) || defined(RT_FN_ENTER_ASM)
#define RT_FN_ENTER_THUNK 1
// P is the register prefix: "%%" in an asm with operands, "%" in a plain one (the test).
#define RT_FN_ENTER_CODE(P, ADDR) \
    "cmpl $0, g_fnprof(" P "rip)\n\tjne 2f\n1:\n" \
    "\t.pushsection .text.unlikely.fnenter,\"ax\",@progbits\n" \
    "2:\tleaq -128(" P "rsp), " P "rsp\n\tmovabsq " ADDR ", " P "rax\n\tcall fnprof_thunk\n" \
    "\tleaq 128(" P "rsp), " P "rsp\n\tjmp 1b\n\t.popsection\n"
#define FN_ENTER(a) __asm__ volatile(RT_FN_ENTER_CODE("%%", "%[addr]") \
    : : [addr] "i" ((uint64_t)(a)), "D" (c) \
    : "cc", "memory", "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", \
      "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15")
// rax: the function's address; rdi: the Cpu, kept. fnprof_hit(rax, c->x[30], c->x[0]) on an aligned stack.
#define RT_FN_THUNK_BODY \
    "pushq %rdi\n\tpushq %rbp\n\tmovq %rsp, %rbp\n\tandq $-16, %rsp\n" \
    "movq 240(%rdi), %rsi\n\tmovq 0(%rdi), %rdx\n\tmovq %rax, %rdi\n\tcall fnprof_hit\n" \
    "movq %rbp, %rsp\n\tpopq %rbp\n\tpopq %rdi\n\tret\n"
#else
#define FN_ENTER(a) do { if (__builtin_expect(g_fnprof, 0)) fnprof_hit(a, c->x[30], c->x[0]); } while (0)
#endif
