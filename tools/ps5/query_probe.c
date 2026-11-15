// Game-free smoke adapter for the real SVC 0x06 output handler.
#include "kernel.h"
#include "platform/platform.h"
#include "platform/ps5_guest_span.h"
#include "platform.h" // PS5_VulkanTemplate's title logger

#include <stdlib.h>

_Static_assert(sizeof(RtMemoryInfo) == 40 && offsetof(RtMemoryInfo, perm) == 24, "QueryMemory LP64 ABI");

static Ps5GuestSpan span;
Ps5GuestSpan *ps5_query_fixture_span(void) { return &span; }

// The title has no loaded guest modules or guest threads. The extracted
// query uses one metadata lock. A real pthread mutex protects that lock.
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int index) {
    (void)index;
    abort();
}
void kern_main_stack(uint64_t *base, uint64_t *size) { *base = *size = 0; }

static pthread_mutex_t query_lock = PTHREAD_MUTEX_INITIALIZER;
void plat_mutex_lock_shared(PlatMutex *lock) {
    (void)lock;
    if (pthread_mutex_lock(&query_lock)) abort();
}
void plat_mutex_unlock_shared(PlatMutex *lock) {
    (void)lock;
    if (pthread_mutex_unlock(&query_lock)) abort();
}

static bool check(bool ok, const char *step) {
    if (!ok) say("PS5 QueryMemory: %s failed", step);
    return ok;
}

static bool query(uintptr_t output, uint32_t expected_result, const char *step) {
    uint64_t regs[] = {output, UINT64_MAX, RT_ADDRESS_LIMIT, 0xcafe112233445566ull};
    svc_query_memory(regs);
    bool ok = regs[0] == expected_result && !regs[1] && regs[2] == RT_ADDRESS_LIMIT &&
              regs[3] == 0xcafe112233445566ull;
    if (!ok) say("PS5 QueryMemory: %s x0=%#llx expected=%#x x1=%#llx", step,
                 (unsigned long long)regs[0], expected_result, (unsigned long long)regs[1]);
    return ok;
}

static bool output_matches(uintptr_t output, const void *expected, const char *step) {
    RtMemoryInfo info;
    return check(!ps5_guest_span_copy_in(&span, &info, output, sizeof info) &&
                 !memcmp(&info, expected, sizeof info), step);
}

static bool test_output(void) {
    const uintptr_t base = RT_LOW_BASE;
    const uintptr_t unit = base + PS5_MEM_UNIT;
    const uintptr_t output = unit - sizeof(RtMemoryInfo) / 2;
    const RtMemoryInfo expected = {.base = RT_ADDRESS_LIMIT, .size = 0 - RT_ADDRESS_LIMIT, .state = 0x10};
    uint8_t marker[sizeof(RtMemoryInfo)];
    memset(marker, 0x5a, sizeof marker);

    return check(!ps5_guest_span_commit(&span, unit - PS5_GUEST_PAGE), "commit first unit") &&
           check(!ps5_guest_span_commit(&span, unit), "commit second unit") &&
           check(!ps5_guest_span_commit(&span, unit + PS5_GUEST_PAGE), "commit writable sibling") &&
           query(output, 0, "cross-page output") &&
           output_matches(output, &expected, "40-byte output") &&
           query(0, KERN_R_INVALID_POINTER, "null output") &&
           query(UINTPTR_MAX - 19, KERN_R_INVALID_POINTER, "overflowing output") &&
           check(!ps5_guest_span_copy_out(&span, output, marker, sizeof marker), "set output marker") &&
           check(!ps5_guest_span_protect(&span, unit, PS5_GUEST_READ), "set read-only page") &&
           query(output, KERN_R_INVALID_POINTER, "cross-page read-only output") &&
           output_matches(output, marker, "read-only denial preserved marker") &&
           check(!ps5_guest_span_protect(&span, unit, PS5_GUEST_NONE), "set no-access page") &&
           query(output, KERN_R_INVALID_POINTER, "cross-page no-access output") &&
           check(!ps5_guest_span_protect(&span, unit, PS5_GUEST_RW), "restore writable page") &&
           output_matches(output, marker, "no-access denial preserved marker");
}

bool mkw_query_memory_probe(void) {
    bool ok = check(!ps5_guest_span_init(&span, RT_LOW_BASE), "reserve test span") && test_output();
    if (!check(!ps5_guest_span_destroy(&span), "release test span")) ok = false;
    say("PS5 QueryMemory: %s", ok ? "passed" : "failed");
    return ok;
}
