// Linux x86-64 backing model for the game-free PS5 mixed-page test.
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <sys/mman.h>

#include "platform/ps5_mixed.h"

static Ps5MixedBlock block;
static jmp_buf fault;
static bool expect_denial;

static inline void *checked(uintptr_t address, size_t bytes, bool write) {
    void *pointer = ps5_mixed_ptr(&block, address, bytes, write);
    if (pointer) return pointer;
    assert(expect_denial);
    longjmp(fault, 1);
}

#define RT_GUEST_READ_PTR(a, n) checked(a, n, false)
#define RT_GUEST_WRITE_PTR(a, n) checked(a, n, true)
#include "cpu/cpu.h"
#include "lifted.h"

enum { NATIVE_GROUPS = PS5_MEM_UNIT / PS5_MEM_PAGE, ERROR = -42 };
static Ps5MemProt modes[NATIVE_GROUPS];
static unsigned allocations;
static bool fail_protect, fail_unmap, lose_reservation;

int ps5_mem_reserve(Ps5MemBlock *backing, uintptr_t address) {
    void *mapped = mmap((void *)address, PS5_MEM_UNIT, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(mapped != MAP_FAILED);
    if (mapped != (void *)address) {
        assert(!munmap(mapped, PS5_MEM_UNIT));
        return PS5_MEM_RELOCATED;
    }
    *backing = (Ps5MemBlock){.address = address, .reserved = true};
    return 0;
}

int ps5_mem_commit(Ps5MemBlock *backing, Ps5MemCommitReport *report) {
    assert(!report && backing->reserved && !backing->committed && !allocations);
    assert(!mprotect((void *)backing->address, PS5_MEM_UNIT, PROT_READ | PROT_WRITE));
    memset((void *)backing->address, 0, PS5_MEM_UNIT);
    for (size_t i = 0; i < NATIVE_GROUPS; ++i) modes[i] = PS5_MEM_RW;
    backing->committed = true;
    ++allocations;
    return 0;
}

// The real one (ps5_memory.c) writes the unit's first contents through a private mapping before the unit is mapped at
// its address; here the unit is committed as above and fill writes it in place. A fill's nonzero result is returned.
int ps5_mem_commit_fill(Ps5MemBlock *backing, Ps5MemCommitReport *report, int (*fill)(void *ctx, void *unit), void *ctx) {
    int result = ps5_mem_commit(backing, report);
    if (!result && fill) result = fill(ctx, (void *)backing->address);
    return result;
}

int ps5_mem_protect_native(Ps5MemBlock *backing, size_t offset, size_t bytes, Ps5MemProt protection) {
    assert(backing->committed && bytes && !(offset % PS5_MEM_PAGE) && !(bytes % PS5_MEM_PAGE));
    assert(offset < PS5_MEM_UNIT && bytes <= PS5_MEM_UNIT - offset);
    if (fail_protect) return ERROR;
    const int flags[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE};
    assert(!mprotect((void *)(backing->address + offset), bytes, flags[protection]));
    for (size_t i = offset; i < offset + bytes; i += PS5_MEM_PAGE) modes[i / PS5_MEM_PAGE] = protection;
    return 0;
}

int ps5_mem_decommit(Ps5MemBlock *backing) {
    assert(backing->committed && allocations == 1);
    if (fail_unmap) return ERROR;
    backing->committed = false;
    --allocations;
    if (lose_reservation) {
        assert(!munmap((void *)backing->address, PS5_MEM_UNIT));
        backing->reserved = false;
        return ERROR;
    }
    assert(!mprotect((void *)backing->address, PS5_MEM_UNIT, PROT_NONE));
    return 0;
}

int ps5_mem_release(Ps5MemBlock *backing) {
    if (backing->reserved) assert(!munmap((void *)backing->address, PS5_MEM_UNIT));
    allocations -= backing->committed;
    *backing = (Ps5MemBlock){0};
    return 0;
}

static void denied(void (*insn)(Cpu *), Cpu *cpu, uintptr_t address) {
    cpu->x[1] = address;
    expect_denial = true;
    if (!setjmp(fault)) {
        insn(cpu);
        assert(!"access should have been rejected");
    }
    expect_denial = false;
}

int main(void) {
    const size_t page = PS5_GUEST_PAGE;
    const uintptr_t base = 144ull << 30;
    assert(ps5_mixed_state(&block, base) == PS5_GUEST_FREE);
    assert(!ps5_mixed_reserve(&block, base));
    assert(ps5_mixed_state(&block, base + page - 1) == PS5_GUEST_RESERVED);
    assert(ps5_mixed_state(&block, base - 1) == PS5_GUEST_FREE);
    assert(!ps5_mixed_ptr(&block, base, 1, false));

    fail_protect = true;
    assert(ps5_mixed_commit(&block, page) == ERROR);
    assert(!allocations && ps5_mixed_state(&block, base + page) == PS5_GUEST_RESERVED);
    fail_unmap = true;
    assert(ps5_mixed_commit(&block, page) == ERROR);
    assert(allocations == 1 && !block.live_pages && !ps5_mixed_ptr(&block, base + page, 1, false));
    fail_protect = false;
    fail_unmap = false;
    assert(ps5_mixed_commit(&block, page) == PS5_MEM_BAD_REQUEST);
    assert(!ps5_mixed_release(&block) && !allocations);
    assert(!ps5_mixed_reserve(&block, base));
    assert(!ps5_mixed_commit(&block, 0));
    assert(!ps5_mixed_commit(&block, page));
    assert(!ps5_mixed_commit(&block, page * 2));
    assert(!ps5_mixed_protect(&block, page, PS5_GUEST_READ));
    assert(!ps5_mixed_protect(&block, page * 2, PS5_GUEST_NONE));
    assert(modes[0] == PS5_MEM_RW && modes[1] == PS5_MEM_NONE);
    assert(ps5_mixed_state(&block, base) == PS5_GUEST_RW);
    assert(ps5_mixed_state(&block, base + page) == PS5_GUEST_READ);
    assert(ps5_mixed_state(&block, base + page * 2) == PS5_GUEST_NONE);
    assert(ps5_mixed_state(&block, base + page * 3) == PS5_GUEST_RESERVED);
    assert(ps5_mixed_ptr(&block, base + page - 1, 2, false) == (void *)(base + page - 1));
    assert(!ps5_mixed_ptr(&block, base + page - 1, 2, true));
    assert(!ps5_mixed_ptr(&block, base + page * 2 - 1, 2, false));
    assert(!ps5_mixed_ptr(&block, base + PS5_MEM_UNIT - 1, 2, false));
    assert(!ps5_mixed_ptr(&block, UINTPTR_MAX, 2, false));

    Cpu cpu = {0};
    cpu.x[1] = base;
    cpu.x[2] = 19;
    str_x(&cpu);
    ldxr(&cpu);
    assert(cpu.x[2] == 19);
    cpu.x[2] = 20;
    stxr(&cpu);
    assert(cpu.x[0] == 0 && LD64(base) == 20);
    stxr(&cpu);
    assert(cpu.x[0] == 1 && LD64(base) == 20);
    ldadd(&cpu);
    assert(cpu.x[3] == 20 && LD64(base) == 40);
    cpu.x[2] = 40;
    cpu.x[3] = 21;
    cas(&cpu);
    assert(cpu.x[2] == 40 && LD64(base) == 21);
    cas(&cpu);
    assert(cpu.x[2] == 21 && LD64(base) == 21);
    cpu.x[3] = 0;
    cpu.x[4] = 5;
    cpu.x[5] = 6;
    casp(&cpu);
    assert(LD64(base) == 5 && LD64(base + 8) == 6);

    cpu.x[1] = base + 32;
    cpu.v[2] = 17;
    cpu.v[3] = 18;
    st1(&cpu);
    str_q(&cpu);
    cpu.v[2] = cpu.v[3] = 0;
    ldr_q(&cpu);
    assert(cpu.v[2] == 17);
    ld1(&cpu);
    assert(cpu.v[2] == 17 && cpu.v[3] == 18);
    zva(&cpu);
    assert(LD64(base) == 0 && LD128(base + 32) == 0 && LD128(base + 48) == 0);

    denied(str_x, &cpu, base + page - 4);
    assert(!LD32(base + page - 4));
    denied(str_q, &cpu, base + page - 8);
    denied(st1, &cpu, base + page);
    denied(cas, &cpu, base + page);
    denied(casp, &cpu, base + page);
    denied(ldadd, &cpu, base + page);
    denied(zva, &cpu, base + page + 7);
    denied(ldr_x, &cpu, base + page * 2);
    denied(ldr_q, &cpu, base + page * 3);
    cpu.x[1] = base + page;
    ldapr(&cpu);
    assert(!cpu.x[2]);
    ldxr(&cpu);
    denied(stxr, &cpu, base + page);
    assert(!LD64(base + page));

    ST64(base, 99);
    assert(!ps5_mixed_protect(&block, page, PS5_GUEST_RW));
    ST64(base + page, 77);
    assert(!ps5_mixed_decommit(&block, page));
    assert(allocations == 1 && !ps5_mixed_ptr(&block, base + page, 8, false));
    assert(!ps5_mixed_commit(&block, page));
    assert(LD64(base) == 99 && !LD64(base + page));

    const size_t group = PS5_MEM_PAGE;
    assert(!ps5_mixed_commit(&block, group - page));
    for (size_t i = group; i < group * 2; i += page) {
        assert(!ps5_mixed_commit(&block, i));
        assert(!ps5_mixed_protect(&block, i, PS5_GUEST_READ));
    }
    assert(modes[1] == PS5_MEM_READ && modes[0] == PS5_MEM_RW);
    assert(ps5_mixed_ptr(&block, base + group - 1, 2, false));
    assert(!ps5_mixed_ptr(&block, base + group - 1, 2, true));
    fail_protect = true;
    assert(ps5_mixed_protect(&block, group, PS5_GUEST_RW) == ERROR);
    assert(ps5_mixed_state(&block, base + group) == PS5_GUEST_READ && modes[1] == PS5_MEM_READ);
    fail_protect = false;
    for (size_t i = group; i < group * 2; i += page) assert(!ps5_mixed_protect(&block, i, PS5_GUEST_NONE));
    assert(modes[1] == PS5_MEM_NONE && LD64(base) == 99);
    for (size_t i = page; i < group * 2; i += page) assert(!ps5_mixed_decommit(&block, i));

    fail_unmap = true;
    assert(ps5_mixed_decommit(&block, 0) == ERROR);
    assert(allocations == 1 && LD64(base) == 99);
    fail_unmap = false;
    assert(!ps5_mixed_decommit(&block, 0));
    assert(!allocations && ps5_mixed_state(&block, base) == PS5_GUEST_RESERVED);
    assert(!ps5_mixed_commit(&block, 0) && !LD64(base));

    // Lazy commit, protect-before-back, back, and recommit-zeroed.
    assert(!ps5_mixed_decommit(&block, 0) && !allocations);
    assert(!ps5_mixed_commit_lazy(&block, 0) && !ps5_mixed_commit_lazy(&block, group));
    assert(!allocations && ps5_mixed_state(&block, base) == PS5_GUEST_RW);
    assert(!ps5_mixed_protect(&block, group, PS5_GUEST_READ) && !allocations);
    assert(!ps5_mixed_ptr(&block, base, 8, false)); // not backed yet
    assert(!ps5_mixed_back(&block) && allocations == 1);
    assert(modes[0] == PS5_MEM_RW && modes[1] == PS5_MEM_READ);
    assert(ps5_mixed_ptr(&block, base, 8, true) && !ps5_mixed_ptr(&block, base + group, 8, true));
    assert(ps5_mixed_ptr(&block, base + group, 8, false) && !LD64(base) && !LD64(base + group));
    ST64(base, 0x1234);
    assert(!ps5_mixed_decommit(&block, 0) && !ps5_mixed_decommit(&block, group));
    assert(!allocations);
    assert(!ps5_mixed_commit_lazy(&block, 0) && !ps5_mixed_back(&block) && !LD64(base));

    lose_reservation = true;
    assert(ps5_mixed_decommit(&block, 0) == ERROR);
    assert(!allocations && ps5_mixed_state(&block, base) == PS5_GUEST_FREE);
    assert(!ps5_mixed_release(&block));
}
