// The register and copy-out contract for QueryMemory (SVC 0x06).
// Split from svc.c, adapted from MacNativeGames games/totk/runtime/services.c @ b75be8c
// (MIT, Copyright (c) 2026 MacNativeGames contributors).
#include "kernel.h"

#ifdef MKW_PS5_QUERY_SPAN_FIXTURE
#include "platform/ps5_guest_span.h"

// The game-free fixture owns the only guest memory this build can access.
extern Ps5GuestSpan *ps5_query_fixture_span(void);
#endif

#define ST_FREE 0u
#define ST_STACK 0xBu
#define PERM_RW 3u

// QueryMemory sees the guest main thread's stack, which memory.c does not know: Stack inside it, free ranges
// around it clipped (the way memory.c clips moved stacks).
static void main_stack_query(uint64_t a, RtMemoryInfo *r) {
    uint64_t mb, ms;
    kern_main_stack(&mb, &ms);
    if (!ms) return;
    uint64_t me = mb + ms;
    if (a >= mb && a < me) {
        *r = (RtMemoryInfo){.base = mb, .size = ms, .state = ST_STACK, .perm = PERM_RW};
        return;
    }
    if (r->state != ST_FREE) return;
    uint64_t lo = r->base, hi = r->base + r->size;
    if (me <= a && me > lo) lo = me;
    if (mb > a && mb < hi) hi = mb;
    r->base = lo;
    r->size = hi - lo;
}

void svc_query_memory(uint64_t *x) {
    RtMemoryInfo mi;
    uint32_t result = mem_query(x[2], &mi);
    if (!result) {
        main_stack_query(x[2], &mi);
#ifdef MKW_PS5_QUERY_SPAN_FIXTURE
        // Atmosphère 6b0c077's QueryMemory uses InvalidPointer for inaccessible
        // user output. The fixture also rejects access while a mapping drains.
        if (ps5_guest_span_copy_out(ps5_query_fixture_span(), x[0], &mi, sizeof mi))
            result = KERN_R_INVALID_POINTER;
#else
        if (x[0]) memcpy((void *)(uintptr_t)x[0], &mi, sizeof mi);
#endif
    }
    x[0] = result;
    x[1] = 0;
}
