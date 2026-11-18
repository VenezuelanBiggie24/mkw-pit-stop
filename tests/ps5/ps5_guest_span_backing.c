// Linux mmap model of two adjacent PS5 direct-memory units.
#define _GNU_SOURCE
#include "platform/ps5_memory.h"

#include <assert.h>
#include <string.h>
#include <sys/mman.h>

int ps5_mem_reserve(Ps5MemBlock *block, uintptr_t address) {
    void *mapped = mmap((void *)address, PS5_MEM_UNIT, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return PS5_MEM_BAD_REQUEST;
    if (mapped != (void *)address) {
        assert(!munmap(mapped, PS5_MEM_UNIT));
        return PS5_MEM_RELOCATED;
    }
    *block = (Ps5MemBlock){.address = address, .reserved = true};
    return 0;
}

int ps5_mem_commit(Ps5MemBlock *block, Ps5MemCommitReport *report) {
    assert(!report && block->reserved && !block->committed);
    assert(!mprotect((void *)block->address, PS5_MEM_UNIT, PROT_READ | PROT_WRITE));
    memset((void *)block->address, 0, PS5_MEM_UNIT);
    block->committed = true;
    return 0;
}

int ps5_mem_protect_native(Ps5MemBlock *block, size_t offset, size_t bytes, Ps5MemProt protection) {
    const int flags[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE};
    assert(block->committed && bytes && !(offset % PS5_MEM_PAGE) && !(bytes % PS5_MEM_PAGE));
    assert(offset < PS5_MEM_UNIT && bytes <= PS5_MEM_UNIT - offset);
    assert(!mprotect((void *)(block->address + offset), bytes, flags[protection]));
    return 0;
}

int ps5_mem_decommit(Ps5MemBlock *block) {
    assert(block->reserved && block->committed);
    assert(!mprotect((void *)block->address, PS5_MEM_UNIT, PROT_NONE));
    block->committed = false;
    return 0;
}

int ps5_mem_release(Ps5MemBlock *block) {
    if (block->reserved) assert(!munmap((void *)block->address, PS5_MEM_UNIT));
    *block = (Ps5MemBlock){0};
    return 0;
}
