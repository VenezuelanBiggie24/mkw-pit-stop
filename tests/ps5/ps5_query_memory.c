// Exercise the shared SVC 0x06 handler and real mem_query without booting the game.
#include "kernel.h"
#include "platform/platform.h"

#include <assert.h>
#include <stdlib.h>

_Static_assert(sizeof(RtMemoryInfo) == 40 && offsetof(RtMemoryInfo, perm) == 24,
               "QueryMemory LP64 ABI");
_Static_assert(KERN_R_INVALID_POINTER == ((115u << 9) | 1u), "kernel result encoding");

// There are no loaded modules. The memory query's normal shared-lock calls
// remain in use. The test has a single query thread and no runtime map writers.
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int index) {
    (void)index;
    abort();
}
void plat_mutex_lock_shared(PlatMutex *lock) { (void)lock; }
void plat_mutex_unlock_shared(PlatMutex *lock) { (void)lock; }

static const uint64_t main_stack = 146ull << 30;
void kern_main_stack(uint64_t *base, uint64_t *size) {
    *base = main_stack;
    *size = 0x10000;
}

static void query(uint64_t output, uint64_t address, uint32_t result) {
    uint64_t regs[] = {output, UINT64_MAX, address, UINT64_C(0xcafe112233445566)};
    svc_query_memory(regs);
    assert(regs[0] == result && regs[1] == 0);
    assert(regs[2] == address && regs[3] == UINT64_C(0xcafe112233445566));
}

static const RtMemoryInfo stack_info = {
    .base = 146ull << 30, .size = 0x10000, .state = 0xb, .perm = 3,
};
static const RtMemoryInfo inaccessible_info = {
    .base = RT_ADDRESS_LIMIT, .size = 0 - RT_ADDRESS_LIMIT, .state = 0x10,
};

#ifdef MKW_PS5_QUERY_SPAN_FIXTURE
#include "platform/ps5_guest_span.h"
#include <sched.h>

static Ps5GuestSpan span;
Ps5GuestSpan *ps5_query_fixture_span(void) { return &span; }

static uintptr_t pause_copy_at, protect_page;
static pthread_t mutator;
static RtMemoryInfo copied;

static void *protect_output(void *unused) {
    (void)unused;
    assert(!ps5_guest_span_protect(&span, protect_page, PS5_GUEST_NONE));
    return NULL;
}

void *__real_memcpy(void *dst, const void *src, size_t bytes);

void *__wrap_memcpy(void *dst, const void *src, size_t bytes) {
    if (!pause_copy_at || (uintptr_t)dst != pause_copy_at || bytes != sizeof copied)
        return __real_memcpy(dst, src, bytes);

    pause_copy_at = 0;
    assert(!pthread_create(&mutator, NULL, protect_output, NULL));
    // The copy already holds both pins. Wait for the mutator to gate the
    // second block, then finish the actual memcpy before it can protect it.
    for (;;) {
        Ps5GuestPin probe;
        int result = ps5_guest_span_pin(&span, protect_page, 1, true, &probe);
        if (result == PS5_GUEST_SPAN_BUSY) break;
        assert(!result);
        ps5_guest_span_unpin(&probe);
        sched_yield();
    }
    query((uintptr_t)dst, RT_ADDRESS_LIMIT, KERN_R_INVALID_POINTER);
    void *out = __real_memcpy(dst, src, bytes);
    __real_memcpy(&copied, dst, bytes);
    return out;
}

static void expect_info(uintptr_t address, const RtMemoryInfo *expected) {
    RtMemoryInfo info;
    assert(!ps5_guest_span_copy_in(&span, &info, address, sizeof info));
    assert(!memcmp(&info, expected, sizeof info));
}

int main(void) {
    const uintptr_t base = RT_LOW_BASE;
    const uintptr_t page = base + PS5_GUEST_PAGE;
    const uintptr_t block = base + PS5_MEM_UNIT;
    const size_t half = sizeof(RtMemoryInfo) / 2;
    assert(!ps5_guest_span_init(&span, base));
    assert(!ps5_guest_span_commit(&span, base));
    assert(!ps5_guest_span_commit(&span, page));
    assert(!ps5_guest_span_commit(&span, block - PS5_GUEST_PAGE));
    assert(!ps5_guest_span_commit(&span, block));
    // Keep the native first group of block two writable when the guest
    // output page becomes read-only or inaccessible.
    assert(!ps5_guest_span_commit(&span, block + PS5_GUEST_PAGE));

    query(base, RT_ADDRESS_LIMIT, 0);
    expect_info(base, &inaccessible_info);
    query(base, main_stack + 16, 0);
    expect_info(base, &stack_info);
    query(0, main_stack, KERN_R_INVALID_POINTER);
    query(UINTPTR_MAX - half, main_stack, KERN_R_INVALID_POINTER);
    query(base - sizeof(RtMemoryInfo), main_stack, KERN_R_INVALID_POINTER);
    query(page - half, main_stack, 0);
    expect_info(page - half, &stack_info);
    query(block - half, RT_ADDRESS_LIMIT, 0);
    expect_info(block - half, &inaccessible_info);

    assert(!ps5_guest_span_protect(&span, block, PS5_GUEST_READ));
    query(block - half, main_stack, KERN_R_INVALID_POINTER);
    expect_info(block - half, &inaccessible_info);
    assert(!ps5_guest_span_protect(&span, block, PS5_GUEST_NONE));
    query(block - half, main_stack, KERN_R_INVALID_POINTER);
    assert(!ps5_guest_span_protect(&span, block, PS5_GUEST_RW));
    expect_info(block - half, &inaccessible_info);

    protect_page = block;
    pause_copy_at = block - half;
    query(block - half, main_stack, 0);
    assert(!pause_copy_at);
    assert(!pthread_join(mutator, NULL));
    assert(!memcmp(&copied, &stack_info, sizeof copied));
    query(block - half, RT_ADDRESS_LIMIT, KERN_R_INVALID_POINTER);
    assert(!ps5_guest_span_protect(&span, block, PS5_GUEST_RW));
    expect_info(block - half, &stack_info);

    assert(!ps5_guest_span_destroy(&span));
}
#else
int main(void) {
    RtMemoryInfo out;
    query(0, main_stack, 0);
    query((uintptr_t)&out, main_stack + 16, 0);
    assert(!memcmp(&out, &stack_info, sizeof out));
    query((uintptr_t)&out, RT_ADDRESS_LIMIT, 0);
    assert(!memcmp(&out, &inaccessible_info, sizeof out));
}
#endif
