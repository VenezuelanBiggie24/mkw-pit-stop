// A copy that may fault without running the fault hooks (Linux, PS5): plat_mem_copy_guarded returns 0 when the copy
// faulted, and the platform's fault handler jumps back to it (plat_fault_guarded) before any hook runs.
//
// Why: the PS5's plat_mem_read (plat_mem_peek there) copies guest memory with vm_lock held. A page the early-read watch
// made inaccessible faulted inside that copy, and the fault hook (memory.c thaw_fault) took frz_lock, which a game
// thread held in mem_early_watch while waiting for vm_lock (plat_mem_state): run 25 (14/11/2026) hung with the replay
// "in a submission" for 70 s. A copy that faults now fails instead, with no lock taken in the handler, and the caller
// reads the page the plain way, holding nothing the fault path needs (mem_copy_out).
#if defined(__linux__) || defined(__PROSPERO__)
#include <setjmp.h>
#include <string.h>

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
#endif
