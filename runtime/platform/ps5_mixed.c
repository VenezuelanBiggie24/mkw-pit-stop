// Logical guest pages inside a single owned PS5 direct-memory unit.
#include "ps5_mixed.h"

#include <string.h>

enum { GUEST_PER_NATIVE = PS5_MEM_PAGE / PS5_GUEST_PAGE };

static bool valid(const Ps5MixedBlock *block, size_t offset) {
    return block->backing.reserved && offset < PS5_MEM_UNIT && !(offset % PS5_GUEST_PAGE);
}

static Ps5MemProt native_mode(const Ps5MixedBlock *block, size_t page, Ps5GuestPage next) {
    size_t first = page / GUEST_PER_NATIVE * GUEST_PER_NATIVE;
    bool any_rw = false, any_read = false;
    for (size_t i = first; i < first + GUEST_PER_NATIVE; ++i) {
        Ps5GuestPage p = i == page ? next : (Ps5GuestPage)block->pages[i];
        if (p == PS5_GUEST_RW) {
            any_rw = true;
            break;
        }
        if (p == PS5_GUEST_READ) any_read = true;
    }
    return any_rw ? PS5_MEM_RW : any_read ? PS5_MEM_READ : PS5_MEM_NONE;
}

static int change_native(Ps5MixedBlock *block, size_t page, Ps5GuestPage next) {
    Ps5MemProt mode = native_mode(block, page, next);
    if (mode == native_mode(block, page, block->pages[page])) return 0;
    size_t offset = page / GUEST_PER_NATIVE * PS5_MEM_PAGE;
    return ps5_mem_protect_native(&block->backing, offset, PS5_MEM_PAGE, mode);
}

static int first_commit(Ps5MixedBlock *block, size_t offset) {
    // A failed rollback may leave backing to release with no live guest pages.
    if (block->backing.committed || block->backing.allocated || block->backing.relocated)
        return PS5_MEM_BAD_REQUEST;
    int result = ps5_mem_commit(&block->backing, NULL);
    if (result) return result;
    // Only the group containing the new page needs to be writable.
    size_t first = offset / PS5_MEM_PAGE * PS5_MEM_PAGE;
    size_t tail = first + PS5_MEM_PAGE;
    if (first) result = ps5_mem_protect_native(&block->backing, 0, first, PS5_MEM_NONE);
    if (!result && tail < PS5_MEM_UNIT)
        result = ps5_mem_protect_native(&block->backing, tail, PS5_MEM_UNIT - tail, PS5_MEM_NONE);
    if (!result) return 0;
    int cleanup = ps5_mem_decommit(&block->backing);
    return cleanup ? cleanup : result;
}

int ps5_mixed_reserve(Ps5MixedBlock *block, uintptr_t address) {
    return ps5_mem_reserve(&block->backing, address);
}

int ps5_mixed_commit(Ps5MixedBlock *block, size_t offset) {
    if (!valid(block, offset)) return PS5_MEM_BAD_REQUEST;
    size_t page = offset / PS5_GUEST_PAGE;
    if (block->pages[page] != PS5_GUEST_RESERVED) return PS5_MEM_BAD_REQUEST;
    int result = block->live_pages ? change_native(block, page, PS5_GUEST_RW) : first_commit(block, offset);
    if (result) return result;
    if (block->live_pages)
        memset((void *)(block->backing.address + offset), 0, PS5_GUEST_PAGE);
    block->pages[page] = PS5_GUEST_RW;
    ++block->live_pages;
    return 0;
}

int ps5_mixed_commit_lazy(Ps5MixedBlock *block, size_t offset) {
    if (!valid(block, offset)) return PS5_MEM_BAD_REQUEST;
    size_t page = offset / PS5_GUEST_PAGE;
    if (block->pages[page] != PS5_GUEST_RESERVED) return PS5_MEM_BAD_REQUEST;
    if (block->backing.committed) {
        int result = change_native(block, page, PS5_GUEST_RW);
        if (result) return result;
        memset((void *)(block->backing.address + offset), 0, PS5_GUEST_PAGE);
    } else if (block->backing.allocated || block->backing.relocated) {
        return PS5_MEM_BAD_REQUEST;
    }
    block->pages[page] = PS5_GUEST_RW;
    ++block->live_pages;
    return 0;
}

int ps5_mixed_back_fill(Ps5MixedBlock *block, Ps5MemCommitReport *report, int (*fill)(void *ctx, void *unit),
                        void *ctx) {
    if (!block->backing.reserved || !block->live_pages || block->backing.committed ||
        block->backing.allocated || block->backing.relocated)
        return PS5_MEM_BAD_REQUEST;
    int result = ps5_mem_commit(&block->backing, report);
    if (result) return result;
    if (fill && (result = fill(ctx, (void *)block->backing.address)) != 0) {
        int cleanup = ps5_mem_decommit(&block->backing);
        return cleanup ? cleanup : result;
    }
    for (size_t g = 0; g < PS5_MEM_UNIT / PS5_MEM_PAGE; ++g) {
        size_t first = g * GUEST_PER_NATIVE;
        bool any_rw = false, any_read = false;
        for (size_t i = first; i < first + GUEST_PER_NATIVE; ++i) {
            if (block->pages[i] == PS5_GUEST_RW) {
                any_rw = true;
                break;
            }
            if (block->pages[i] == PS5_GUEST_READ) any_read = true;
        }
        Ps5MemProt mode = any_rw ? PS5_MEM_RW : any_read ? PS5_MEM_READ : PS5_MEM_NONE;
        if (mode != PS5_MEM_RW) {
            result = ps5_mem_protect_native(&block->backing, g * PS5_MEM_PAGE, PS5_MEM_PAGE, mode);
            if (result) {
                int cleanup = ps5_mem_decommit(&block->backing);
                return cleanup ? cleanup : result;
            }
        }
    }
    return 0;
}

int ps5_mixed_back_report(Ps5MixedBlock *block, Ps5MemCommitReport *report) {
    return ps5_mixed_back_fill(block, report, NULL, NULL);
}

int ps5_mixed_back(Ps5MixedBlock *block) { return ps5_mixed_back_report(block, NULL); }

int ps5_mixed_protect(Ps5MixedBlock *block, size_t offset, Ps5GuestPage protection) {
    if (!valid(block, offset) || protection < PS5_GUEST_NONE || protection > PS5_GUEST_RW)
        return PS5_MEM_BAD_REQUEST;
    size_t page = offset / PS5_GUEST_PAGE;
    if (block->pages[page] == PS5_GUEST_RESERVED) return PS5_MEM_BAD_REQUEST;
    int result = block->backing.committed ? change_native(block, page, protection) : 0;
    if (!result) block->pages[page] = protection;
    return result;
}

int ps5_mixed_decommit(Ps5MixedBlock *block, size_t offset) {
    if (!valid(block, offset)) return PS5_MEM_BAD_REQUEST;
    size_t page = offset / PS5_GUEST_PAGE;
    if (block->pages[page] == PS5_GUEST_RESERVED) return PS5_MEM_BAD_REQUEST;
    int result = 0;
    if (block->backing.committed)
        result = block->live_pages == 1 ? ps5_mem_decommit(&block->backing)
                                        : change_native(block, page, PS5_GUEST_RESERVED);
    // The last unmap can succeed even if reacquiring the reservation fails.
    if (!result || !block->backing.committed) {
        block->pages[page] = PS5_GUEST_RESERVED;
        --block->live_pages;
    }
    return result;
}

int ps5_mixed_release(Ps5MixedBlock *block) {
    int result = ps5_mem_release(&block->backing);
    if (!result) *block = (Ps5MixedBlock){0};
    return result;
}

Ps5GuestPage ps5_mixed_state(const Ps5MixedBlock *block, uintptr_t address) {
    uintptr_t offset = address - block->backing.address;
    return block->backing.reserved && offset < PS5_MEM_UNIT ? block->pages[offset / PS5_GUEST_PAGE] : PS5_GUEST_FREE;
}
