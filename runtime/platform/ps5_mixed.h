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
    uint32_t nz_freeze;  // plat_mem_reclaim_zeros: the freeze (mem_frozen's number) in which it found this unit not
                         // all zero; 0 none. Read-only for that whole freeze, so no need to scan it again.
    // MKW_PACK (ps5_guest_memory.c, plat_mem_pack): the unit's bytes compressed while its backing is released (the
    // guest pages keep their states); the next backing decompresses them. NULL when not packed. Under the unit's lock.
    void *pack;
    uint32_t pk_fail_freeze;   // the freeze in which it did not compress well enough; 0 none
    uint32_t pk_unpack_ms;     // plat_now_ms at the last unpack, | 1; 0 never: not packed again until it is cold again
} Ps5MixedBlock;

int ps5_mixed_reserve(Ps5MixedBlock *block, uintptr_t address);
int ps5_mixed_commit(Ps5MixedBlock *block, size_t offset);
// Records the guest page as committed RW without allocating physical DMEM until first access or ps5_mixed_back().
int ps5_mixed_commit_lazy(Ps5MixedBlock *block, size_t offset);
int ps5_mixed_back(Ps5MixedBlock *block);
int ps5_mixed_back_report(Ps5MixedBlock *block, Ps5MemCommitReport *report);
// The same, with fill(ctx, unit) writing the unit's bytes through a private map before the unit is mapped at its guest
// address (ps5_mem_commit_fill), so no thread sees it half written. A nonzero fill releases the backing again and is
// returned.
int ps5_mixed_back_fill(Ps5MixedBlock *block, Ps5MemCommitReport *report, int (*fill)(void *ctx, void *unit),
                        void *ctx);
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
