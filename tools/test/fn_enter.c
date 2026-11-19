// Checks the PS5's FN_ENTER (runtime/cpu/cpu.h), the out-of-line hook at the start of every lifted function, on any
// x86-64 Linux: why here, because a slip in it corrupts the game's registers only while a diagnostic hook is on.
// fnprof_hit must get the function's address, x30 and x0, on an aligned stack; rdi, the callee-saved registers and the
// 128 bytes below the stack pointer must survive; and with the hook off nothing is called.
//   clang -O1 -g -DRT_FN_ENTER_ASM -Iruntime/cpu tools/test/fn_enter.c -o build/fn_enter && build/fn_enter
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { uint64_t x[32]; } Cpu;
#include "fn_enter.h"
volatile int g_fnprof;

static uint64_t seen_addr, seen_lr, seen_x0, seen_sp;
static int hits;
__attribute__((naked, used)) void fnprof_thunk(void) { __asm__(RT_FN_THUNK_BODY); }
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    seen_addr = addr, seen_lr = lr, seen_x0 = x0, seen_sp = (uint64_t)__builtin_frame_address(0) + 16, hits++;
    // Everything the ABI lets a callee destroy, destroyed.
    __asm__ volatile("xorl %%eax, %%eax; xorl %%ecx, %%ecx; xorl %%edx, %%edx; xorl %%esi, %%esi; xorl %%edi, %%edi;"
                     "xorl %%r8d, %%r8d; xorl %%r9d, %%r9d; xorl %%r10d, %%r10d; xorl %%r11d, %%r11d; pxor %%xmm0, %%xmm0"
                     : : : "cc", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0");
}

__attribute__((noinline)) static uint64_t lifted(Cpu *c) {
    FN_ENTER(0x2801234c50ull);
    return c->x[1] + 1;
}

// The stub's path with known callee-saved registers and a marked red zone. 0: rdi and all of them came back.
__attribute__((naked, noinline)) static uint64_t stress(__attribute__((unused)) Cpu *c) {
    __asm__("pushq %rbx; pushq %rbp; pushq %r12; pushq %r13; pushq %r14; pushq %r15; pushq %rdi\n"
            "movabsq $0x1111111111111111, %rbx; movabsq $0x2222222222222222, %rbp\n"
            "movabsq $0x3333333333333333, %r12; movabsq $0x4444444444444444, %r13\n"
            "movabsq $0x5555555555555555, %r14; movabsq $0x6666666666666666, %r15\n"
            "movabsq $0x7777777777777777, %rax; movq %rax, -8(%rsp); movq %rax, -64(%rsp); movq %rax, -128(%rsp)\n"
            RT_FN_ENTER_CODE("%", "$0xabc")
            "movl $1, %eax; cmpq (%rsp), %rdi; jne 9f\n"
            "movl $2, %eax; movabsq $0x7777777777777777, %rcx\n"
            "cmpq %rcx, -8(%rsp); jne 9f; cmpq %rcx, -64(%rsp); jne 9f; cmpq %rcx, -128(%rsp); jne 9f\n"
            "movl $3, %eax\n"
            "movabsq $0x1111111111111111, %rcx; cmpq %rcx, %rbx; jne 9f\n"
            "movabsq $0x2222222222222222, %rcx; cmpq %rcx, %rbp; jne 9f\n"
            "movabsq $0x3333333333333333, %rcx; cmpq %rcx, %r12; jne 9f\n"
            "movabsq $0x4444444444444444, %rcx; cmpq %rcx, %r13; jne 9f\n"
            "movabsq $0x5555555555555555, %rcx; cmpq %rcx, %r14; jne 9f\n"
            "movabsq $0x6666666666666666, %rcx; cmpq %rcx, %r15; jne 9f\n"
            "xorl %eax, %eax\n"
            "9: addq $8, %rsp; popq %r15; popq %r14; popq %r13; popq %r12; popq %rbp; popq %rbx; ret\n");
}

int main(void) {
    Cpu c = {0};
    c.x[0] = 0x1000, c.x[1] = 41, c.x[30] = 0x2800000040ull;
    assert(lifted(&c) == 42 && !hits);   // off: not called
    g_fnprof = 1;
    assert(lifted(&c) == 42 && hits == 1);
    assert(seen_addr == 0x2801234c50ull && seen_lr == 0x2800000040ull && seen_x0 == 0x1000 && !(seen_sp & 15));
    assert(stress(&c) == 0 && hits == 2 && seen_addr == 0xabc);
    puts("fn_enter: passed");
    return 0;
}
