// Shared System V x86-64 fiber implementation. The host supplies fiber_map_stack.
// glibc's swapcontext saves and restores the signal mask with a system call
// on every switch, and the game switches fibers often. fiber_swap saves the callee-saved registers, the MXCSR and the
// x87 control word on the current stack, stores the stack pointer in *save and loads the other one. A new fiber's
// stack is laid out as fiber_swap leaves one, returning into fiber_entry, which calls fn(arg) (r12 and r13).
struct PlatFiber {
    void *sp;
    void *stack;   // mapped stack (NULL for a thread's own context)
    size_t size;
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    movq %r13, %rdi\n"
        "    andq $-16, %rsp\n"   // the ret here left it 8 off; SysV wants 16 before the call (NX2navi e9edd6a)
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    void *m = fiber_map_stack(&stack);
    if (m == MAP_FAILED) {
        free(f);
        return NULL;
    }
    f->stack = m, f->size = stack;
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
    uint64_t *sp = top - 10;   // fpu state (2 words), r15, r14, r13, r12, rbx, rbp, return address, padding
    sp[0] = 0x037f;            // x87 control word: the default
    sp[1] = 0x1f80;            // MXCSR: the default
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = 0;
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;   // on this thread; the switch back into from sets it on whichever thread that is
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

