// Checked guest-span and mapping-race tests on the Linux backing model.
#define _GNU_SOURCE
#include "platform/ps5_guest_span.h"

#include <assert.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
    Ps5GuestSpan *span;
    uintptr_t address;
    bool decommit;
    int result;
} Mutation;

static void *change_mapping(void *opaque) {
    Mutation *mutation = opaque;
    mutation->result = mutation->decommit ? ps5_guest_span_decommit(mutation->span, mutation->address)
                                         : ps5_guest_span_protect(mutation->span, mutation->address, PS5_GUEST_NONE);
    return NULL;
}

static void check_quiescence(Ps5GuestSpan *span, uintptr_t address, uintptr_t unrelated, bool decommit) {
    Ps5GuestPin held;
    assert(!ps5_guest_span_pin(span, address, 1, true, &held));

    Mutation mutation = {.span = span, .address = address, .decommit = decommit};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, change_mapping, &mutation));

    // BUSY is the admission barrier. The mutator has closed this block and
    // cannot alter its mapping until the held pin drains.
    const uint8_t value = 123;
    for (;;) {
        int result = ps5_guest_span_copy_out(span, address, &value, sizeof value);
        if (result == PS5_GUEST_SPAN_BUSY) break;
        assert(!result);
        sched_yield();
    }
    assert(!ps5_guest_span_copy_out(span, unrelated, &value, sizeof value));
    assert(ps5_guest_span_protect(span, address, PS5_GUEST_RW) == PS5_GUEST_SPAN_BUSY);
    *(uint8_t *)held.ptr = 42;
    ps5_guest_span_unpin(&held);
    assert(!pthread_join(thread, NULL) && !mutation.result);
    uint8_t found = 0;
    assert(ps5_guest_span_copy_in(span, &found, address, 1) == PS5_MEM_BAD_REQUEST);
    assert(!ps5_guest_span_copy_in(span, &found, unrelated, 1) && found == value);
}

int main(void) {
    const uintptr_t base = 145ull << 30;
    const uintptr_t page = base + PS5_GUEST_PAGE;
    const uintptr_t boundary = base + PS5_MEM_UNIT;
    Ps5GuestSpan span = {0};
    assert(ps5_guest_span_init(&span, base + 1) == PS5_MEM_BAD_REQUEST);

    // Partial init must release only its own reservation on cleanup.
    void *occupied = mmap((void *)boundary, PS5_MEM_UNIT, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(occupied == (void *)boundary);
    *(uint8_t *)occupied = 7;
    assert(ps5_guest_span_init(&span, base) == PS5_MEM_RELOCATED);
    assert(!ps5_guest_span_destroy(&span));
    assert(*(uint8_t *)occupied == 7);
    assert(!munmap(occupied, PS5_MEM_UNIT));

    assert(!ps5_guest_span_init(&span, base));
    assert(!ps5_guest_span_copy_in(&span, NULL, UINTPTR_MAX, 0));
    assert(!ps5_guest_span_copy_out(&span, UINTPTR_MAX, NULL, 0));
    assert(!ps5_guest_span_commit(&span, base));
    assert(!ps5_guest_span_commit(&span, page));
    assert(!ps5_guest_span_commit(&span, boundary - PS5_GUEST_PAGE));
    assert(!ps5_guest_span_commit(&span, boundary));

    const uint8_t initial[] = {1, 2};
    const uint8_t changed[] = {3, 4};
    uint8_t found[2] = {0};
    assert(ps5_guest_span_copy_in(&span, found, UINTPTR_MAX, 2) == PS5_MEM_BAD_REQUEST);
    assert(ps5_guest_span_copy_in(&span, found, base, SIZE_MAX) == PS5_MEM_BAD_REQUEST);
    assert(!ps5_guest_span_copy_out(&span, page - 1, initial, 2));
    assert(!ps5_guest_span_protect(&span, page, PS5_GUEST_READ));
    assert(!ps5_guest_span_copy_in(&span, found, page - 1, 2) && !memcmp(found, initial, 2));
    assert(ps5_guest_span_copy_out(&span, page - 1, changed, 2) == PS5_MEM_BAD_REQUEST);
    assert(!ps5_guest_span_copy_in(&span, found, page - 1, 2) && !memcmp(found, initial, 2));
    assert(!ps5_guest_span_protect(&span, page, PS5_GUEST_NONE));
    assert(ps5_guest_span_copy_in(&span, found, page - 1, 2) == PS5_MEM_BAD_REQUEST);
    assert(!memcmp(found, initial, 2));
    assert(!ps5_guest_span_protect(&span, page, PS5_GUEST_RW));
    assert(!ps5_guest_span_copy_in(&span, found, page - 1, 2) && !memcmp(found, initial, 2));

    assert(!ps5_guest_span_copy_out(&span, boundary - 1, initial, 2));
    assert(!ps5_guest_span_copy_in(&span, found, boundary - 1, 2) && !memcmp(found, initial, 2));
    assert(!ps5_guest_span_protect(&span, boundary, PS5_GUEST_READ));
    assert(ps5_guest_span_copy_out(&span, boundary - 1, changed, 2) == PS5_MEM_BAD_REQUEST);
    assert(!ps5_guest_span_copy_in(&span, found, boundary - 1, 2) && !memcmp(found, initial, 2));
    assert(!ps5_guest_span_protect(&span, boundary, PS5_GUEST_RW));

    Ps5GuestWrite old;
    assert(!ps5_guest_span_prepare_write(&span, boundary - 1, 2, &old));
    assert(!ps5_guest_span_protect(&span, boundary, PS5_GUEST_READ));
    assert(ps5_guest_span_copy_out_current(&old, changed) == PS5_MEM_BAD_REQUEST);
    assert(!ps5_guest_span_copy_in(&span, found, boundary - 1, 2) && !memcmp(found, initial, 2));
    assert(!ps5_guest_span_protect(&span, boundary, PS5_GUEST_RW));
    assert(!ps5_guest_span_copy_out_current(&old, changed));
    check_quiescence(&span, page, boundary, false);
    check_quiescence(&span, boundary, base, true);
    assert(!ps5_guest_span_commit(&span, boundary));
    assert(ps5_guest_span_copy_out_current(&old, initial) == PS5_GUEST_SPAN_STALE);
    assert(!ps5_guest_span_copy_in(&span, found, boundary - 1, 2));
    assert(found[0] == changed[0] && found[1] == 0);

    Ps5GuestWrite current;
    assert(!ps5_guest_span_prepare_write(&span, boundary - 1, 2, &current));
    assert(!ps5_guest_span_copy_out_current(&current, initial));
    assert(!ps5_guest_span_copy_in(&span, found, boundary - 1, 2) && !memcmp(found, initial, 2));

    // Both block generations start at zero again. The old ticket still
    // expires even though the arena object and guest addresses are reused.
    assert(!ps5_guest_span_destroy(&span));
    assert(ps5_guest_span_copy_out_current(&old, initial) == PS5_GUEST_SPAN_STALE);
    assert(!ps5_guest_span_init(&span, base));
    assert(!ps5_guest_span_commit(&span, boundary - PS5_GUEST_PAGE));
    assert(!ps5_guest_span_commit(&span, boundary));
    assert(ps5_guest_span_copy_out_current(&old, initial) == PS5_GUEST_SPAN_STALE);
    assert(!ps5_guest_span_copy_in(&span, found, boundary - 1, 2) && !found[0] && !found[1]);
    assert(!ps5_guest_span_destroy(&span));
}
