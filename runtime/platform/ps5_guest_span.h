#ifndef MKW_PS5_GUEST_SPAN_H
#define MKW_PS5_GUEST_SPAN_H

#include "ps5_mixed.h"
#include <pthread.h>

enum { PS5_GUEST_SPAN_BLOCKS = 2, PS5_GUEST_SPAN_BUSY = -3, PS5_GUEST_SPAN_STALE = -4 };

typedef struct {
    Ps5MixedBlock mixed;
    pthread_mutex_t lock;
    pthread_cond_t idle;
    uint64_t generation;
    unsigned pins;
    bool changing;
} Ps5GuestSpanBlock;

typedef struct {
    uintptr_t base;
    uint64_t lifetime;
    Ps5GuestSpanBlock blocks[PS5_GUEST_SPAN_BLOCKS];
    unsigned initialized;
    bool ready;
} Ps5GuestSpan;

// Start zeroed. Init and destroy require external quiescence. Call destroy
// after failed init to release any partial reservations. Failed destroy can
// be retried. Keep the span at the same address for its entire lifetime and
// let mapping calls return before cancelling their threads.
int ps5_guest_span_init(Ps5GuestSpan *span, uintptr_t base);
int ps5_guest_span_destroy(Ps5GuestSpan *span);
int ps5_guest_span_commit(Ps5GuestSpan *span, uintptr_t address);
int ps5_guest_span_protect(Ps5GuestSpan *span, uintptr_t address, Ps5GuestPage protection);
int ps5_guest_span_decommit(Ps5GuestSpan *span, uintptr_t address);

typedef struct {
    Ps5GuestSpan *span;
    unsigned first, count;
    void *ptr;
} Ps5GuestPin;

// Pin an entire nonempty span or leave it untouched. A change draining pins
// returns BUSY to new callers. Use the pointer only with the requested access
// mode and release the pin before waiting for a mapping change. One release
// per successful pin. Callers synchronize concurrent reads and writes.
int ps5_guest_span_pin(Ps5GuestSpan *span, uintptr_t address, size_t bytes, bool write, Ps5GuestPin *pin);
void ps5_guest_span_unpin(Ps5GuestPin *pin);

// The caller owns the separate host buffer. Zero-byte copies succeed.
int ps5_guest_span_copy_in(Ps5GuestSpan *span, void *host, uintptr_t guest, size_t bytes);
int ps5_guest_span_copy_out(Ps5GuestSpan *span, uintptr_t guest, const void *host, size_t bytes);

typedef struct {
    Ps5GuestSpan *span;
    uintptr_t address;
    size_t bytes;
    uint64_t lifetime;
    uint64_t generation[PS5_GUEST_SPAN_BLOCKS];
} Ps5GuestWrite;

// Capture a nonempty destination's backing identity without holding a pin
// during deferred work. The span's storage must remain valid while the ticket
// can be used. Destroying the span expires the ticket, even across reinit.
int ps5_guest_span_prepare_write(Ps5GuestSpan *span, uintptr_t guest, size_t bytes, Ps5GuestWrite *ticket);
int ps5_guest_span_copy_out_current(const Ps5GuestWrite *ticket, const void *host);

#endif
