#ifndef MKW_PS5_MIXED_H
#define MKW_PS5_MIXED_H

#include "ps5_memory.h"

enum { PS5_GUEST_PAGE = 0x1000 };
typedef enum { PS5_GUEST_FREE = -1, PS5_GUEST_RESERVED, PS5_GUEST_NONE, PS5_GUEST_READ, PS5_GUEST_RW } Ps5GuestPage;

// Game-free prototype. Start zeroed. Map, change permissions and release only
// when no thread can access the block. Every guest access must use the check.
typedef struct {
    Ps5MemBlock backing;
    uint8_t pages[PS5_MEM_UNIT / PS5_GUEST_PAGE];
    unsigned live_pages;
    volatile int lock;   // per-unit spinlock; never take vm_lock while holding this
} Ps5MixedBlock;

int ps5_mixed_reserve(Ps5MixedBlock *block, uintptr_t address);
int ps5_mixed_commit(Ps5MixedBlock *block, size_t offset);
// Records the guest page as committed RW without allocating physical DMEM until first access or ps5_mixed_back().
int ps5_mixed_commit_lazy(Ps5MixedBlock *block, size_t offset);
int ps5_mixed_back(Ps5MixedBlock *block);
int ps5_mixed_back_report(Ps5MixedBlock *block, Ps5MemCommitReport *report);
int ps5_mixed_protect(Ps5MixedBlock *block, size_t offset, Ps5GuestPage protection);
int ps5_mixed_decommit(Ps5MixedBlock *block, size_t offset);
int ps5_mixed_release(Ps5MixedBlock *block);
Ps5GuestPage ps5_mixed_state(const Ps5MixedBlock *block, uintptr_t address);

// Returns the original address only when the entire span is accessible.
static inline void *ps5_mixed_ptr(const Ps5MixedBlock *block, uintptr_t address, size_t bytes, bool write) {
    uintptr_t offset = address - block->backing.address;
    if (!block->backing.committed || !bytes || offset >= PS5_MEM_UNIT || bytes > PS5_MEM_UNIT - offset)
        return NULL;
    size_t last = (offset + bytes - 1) / PS5_GUEST_PAGE;
    for (size_t page = offset / PS5_GUEST_PAGE; page <= last; ++page)
        if (block->pages[page] < (write ? PS5_GUEST_RW : PS5_GUEST_READ)) return NULL;
    return (void *)address;
}

#endif
