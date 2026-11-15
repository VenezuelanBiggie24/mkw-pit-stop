// Scoped host access to two adjacent, independently owned PS5 backing units.
#include "ps5_guest_span.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

enum { SPAN_BYTES = PS5_GUEST_SPAN_BLOCKS * PS5_MEM_UNIT };
static _Atomic uint64_t last_lifetime;

int ps5_guest_span_init(Ps5GuestSpan *span, uintptr_t base) {
    if (span->initialized || !base || base % PS5_MEM_UNIT || base > UINTPTR_MAX - SPAN_BYTES)
        return PS5_MEM_BAD_REQUEST;
    uint64_t previous = atomic_load_explicit(&last_lifetime, memory_order_relaxed);
    do {
        if (previous == UINT64_MAX) return PS5_MEM_BAD_REQUEST;
    } while (!atomic_compare_exchange_weak_explicit(&last_lifetime, &previous, previous + 1,
                                                     memory_order_relaxed, memory_order_relaxed));
    span->base = base;
    span->lifetime = previous + 1;
    for (unsigned i = 0; i < PS5_GUEST_SPAN_BLOCKS; ++i) {
        Ps5GuestSpanBlock *block = &span->blocks[i];
        int result = pthread_mutex_init(&block->lock, NULL);
        if (result) return result;
        result = pthread_cond_init(&block->idle, NULL);
        if (result) {
            pthread_mutex_destroy(&block->lock);
            return result;
        }
        ++span->initialized;
        result = ps5_mixed_reserve(&block->mixed, base + i * PS5_MEM_UNIT);
        if (result) return result;
    }
    span->ready = true;
    return 0;
}

int ps5_guest_span_destroy(Ps5GuestSpan *span) {
    for (unsigned i = 0; i < span->initialized; ++i)
        if (span->blocks[i].pins || span->blocks[i].changing) return PS5_GUEST_SPAN_BUSY;
    span->ready = false;
    while (span->initialized) {
        Ps5GuestSpanBlock *block = &span->blocks[span->initialized - 1];
        int result = ps5_mixed_release(&block->mixed);
        if (result) return result;
        pthread_cond_destroy(&block->idle);
        pthread_mutex_destroy(&block->lock);
        --span->initialized;
    }
    *span = (Ps5GuestSpan){0};
    return 0;
}

typedef enum { CHANGE_COMMIT, CHANGE_PROTECT, CHANGE_DECOMMIT } Change;

static int change(Ps5GuestSpan *span, uintptr_t address, Change action, Ps5GuestPage protection) {
    uintptr_t offset = address - span->base;
    if (!span->ready || offset >= SPAN_BYTES || offset % PS5_GUEST_PAGE) return PS5_MEM_BAD_REQUEST;
    Ps5GuestSpanBlock *block = &span->blocks[offset / PS5_MEM_UNIT];
    pthread_mutex_lock(&block->lock);
    if (block->changing) {
        pthread_mutex_unlock(&block->lock);
        return PS5_GUEST_SPAN_BUSY;
    }
    if (action == CHANGE_DECOMMIT && block->generation == UINT64_MAX) {
        pthread_mutex_unlock(&block->lock);
        return PS5_MEM_BAD_REQUEST;
    }
    block->changing = true;
    while (block->pins) pthread_cond_wait(&block->idle, &block->lock);
    pthread_mutex_unlock(&block->lock);

    // The backing helper requires the whole block to be quiescent. Changing
    // one guest page can also change a native group's protection or the mapping.
    offset %= PS5_MEM_UNIT;
    int result;
    switch (action) {
    case CHANGE_COMMIT: result = ps5_mixed_commit(&block->mixed, offset); break;
    case CHANGE_PROTECT: result = ps5_mixed_protect(&block->mixed, offset, protection); break;
    case CHANGE_DECOMMIT: result = ps5_mixed_decommit(&block->mixed, offset); break;
    default: result = PS5_MEM_BAD_REQUEST; break;
    }

    pthread_mutex_lock(&block->lock);
    // Even an error can have unmapped backing. Invalidate the whole block's
    // tickets conservatively on every decommit attempt.
    if (action == CHANGE_DECOMMIT) ++block->generation;
    block->changing = false;
    pthread_mutex_unlock(&block->lock);
    return result;
}

int ps5_guest_span_commit(Ps5GuestSpan *span, uintptr_t address) {
    return change(span, address, CHANGE_COMMIT, PS5_GUEST_RW);
}

int ps5_guest_span_protect(Ps5GuestSpan *span, uintptr_t address, Ps5GuestPage protection) {
    return change(span, address, CHANGE_PROTECT, protection);
}

int ps5_guest_span_decommit(Ps5GuestSpan *span, uintptr_t address) {
    return change(span, address, CHANGE_DECOMMIT, PS5_GUEST_RESERVED);
}

void ps5_guest_span_unpin(Ps5GuestPin *pin) {
    while (pin->count) {
        Ps5GuestSpanBlock *block = &pin->span->blocks[pin->first + --pin->count];
        pthread_mutex_lock(&block->lock);
        if (!--block->pins && block->changing) pthread_cond_signal(&block->idle);
        pthread_mutex_unlock(&block->lock);
    }
    *pin = (Ps5GuestPin){0};
}

static int pin_range(Ps5GuestSpan *span, uintptr_t address, size_t bytes, bool write,
                     const Ps5GuestWrite *expected, Ps5GuestPin *pin) {
    uintptr_t offset = address - span->base;
    if (!span->ready || !bytes || offset >= SPAN_BYTES || bytes > SPAN_BYTES - offset)
        return PS5_MEM_BAD_REQUEST;
    unsigned first = offset / PS5_MEM_UNIT;
    *pin = (Ps5GuestPin){.span = span, .first = first, .ptr = (void *)address};
    while (bytes) {
        unsigned i = offset / PS5_MEM_UNIT;
        Ps5GuestSpanBlock *block = &span->blocks[i];
        size_t chunk = PS5_MEM_UNIT - offset % PS5_MEM_UNIT;
        if (chunk > bytes) chunk = bytes;

        pthread_mutex_lock(&block->lock);
        int result = 0;
        if (block->changing)
            result = PS5_GUEST_SPAN_BUSY;
        else if (expected && expected->generation[i] != block->generation)
            result = PS5_GUEST_SPAN_STALE;
        else if (block->pins == UINT_MAX ||
                 !ps5_mixed_ptr(&block->mixed, span->base + offset, chunk, write))
            result = PS5_MEM_BAD_REQUEST;
        else
            ++block->pins;
        pthread_mutex_unlock(&block->lock);
        if (result) {
            ps5_guest_span_unpin(pin);
            return result;
        }
        ++pin->count;
        offset += chunk;
        bytes -= chunk;
    }
    return 0;
}

int ps5_guest_span_pin(Ps5GuestSpan *span, uintptr_t address, size_t bytes, bool write, Ps5GuestPin *pin) {
    return pin_range(span, address, bytes, write, NULL, pin);
}

int ps5_guest_span_copy_in(Ps5GuestSpan *span, void *host, uintptr_t guest, size_t bytes) {
    if (!bytes) return 0;
    Ps5GuestPin pin;
    int result = ps5_guest_span_pin(span, guest, bytes, false, &pin);
    if (result) return result;
    memcpy(host, pin.ptr, bytes);
    ps5_guest_span_unpin(&pin);
    return 0;
}

static int copy_out(Ps5GuestSpan *span, uintptr_t guest, const void *host, size_t bytes,
                    const Ps5GuestWrite *expected) {
    if (!bytes) return 0;
    Ps5GuestPin pin;
    int result = pin_range(span, guest, bytes, true, expected, &pin);
    if (result) return result;
    memcpy(pin.ptr, host, bytes);
    ps5_guest_span_unpin(&pin);
    return 0;
}

int ps5_guest_span_copy_out(Ps5GuestSpan *span, uintptr_t guest, const void *host, size_t bytes) {
    return copy_out(span, guest, host, bytes, NULL);
}

int ps5_guest_span_prepare_write(Ps5GuestSpan *span, uintptr_t guest, size_t bytes, Ps5GuestWrite *ticket) {
    Ps5GuestPin pin;
    int result = ps5_guest_span_pin(span, guest, bytes, true, &pin);
    if (result) return result;
    *ticket = (Ps5GuestWrite){.span = span, .address = guest, .bytes = bytes, .lifetime = span->lifetime};
    // A mutation cannot advance a generation until these pins drain.
    for (unsigned i = pin.first; i < pin.first + pin.count; ++i)
        ticket->generation[i] = span->blocks[i].generation;
    ps5_guest_span_unpin(&pin);
    return 0;
}

int ps5_guest_span_copy_out_current(const Ps5GuestWrite *ticket, const void *host) {
    if (!ticket->span->ready || ticket->lifetime != ticket->span->lifetime) return PS5_GUEST_SPAN_STALE;
    return copy_out(ticket->span, ticket->address, host, ticket->bytes, ticket);
}
