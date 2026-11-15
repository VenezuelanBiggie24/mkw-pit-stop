// Check that a fixed direct map can replace part of an owned alias reservation.
#include "backing_layout.h"
#include "ps5_memory.h"

#include <ps5platform/kernel.h>
#include <string.h>

#include "platform.h" // PS5_VulkanTemplate's title logger

typedef struct {
    const char *name;
    uintptr_t address;
    size_t bytes;
    void *owned;
} Reservation;

static bool reserve(Reservation *range) {
    void *address = (void *)range->address;
    int status = sceKernelReserveVirtualRange(&address, range->bytes, 0, PS5_MEM_UNIT);
    if (!status) range->owned = address;
    say("PS5 alias subrange reserve %s: requested=%p returned=%p length=%#llx status=%#x",
        range->name, (void *)range->address, address, (unsigned long long)range->bytes, (unsigned)status);
    return !status && address == (void *)range->address;
}

static bool unmap(const char *name, void *address, size_t bytes) {
    if (!address) return true;
    int status = sceKernelMunmap(address, bytes);
    say("PS5 alias subrange unmap %s: address=%p length=%#llx status=%#x",
        name, address, (unsigned long long)bytes, (unsigned)status);
    return !status;
}

static bool protection(const char *name, uintptr_t address, uint32_t expected) {
    const uint32_t cpu = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_CPU_EXEC;
    void *start = NULL, *end = NULL;
    uint32_t found = 0;
    int status = sceKernelQueryMemoryProtection((void *)address, &start, &end, &found);
    say("PS5 alias subrange query %s: address=%p range=[%p,%p) protection=%#x expectedCPU=%#x status=%#x",
        name, (void *)address, start, end, (unsigned)found, (unsigned)expected, (unsigned)status);
    return !status && (uintptr_t)start <= address && (uintptr_t)end >= address + PS5_MEM_UNIT &&
           (found & cpu) == expected;
}

static bool neighbors(uintptr_t first) {
    bool left = protection("left reservation", first - PS5_MEM_UNIT, 0);
    bool right = protection("right reservation", first + 2 * PS5_MEM_UNIT, 0);
    return left && right;
}

static bool commit(const char *name, Ps5MemBlock *slot) {
    Ps5MemCommitReport report;
    int result = ps5_mem_commit(slot, &report);
    if (report.attempted)
        say("PS5 alias subrange allocate %s: length=%#x alignment=%#x physical=%#llx status=%#x",
            name, (unsigned)PS5_MEM_UNIT, (unsigned)PS5_MEM_UNIT, (unsigned long long)report.physical,
            (unsigned)report.allocation_status);
    if (report.fixed_attempted)
        say("PS5 alias subrange map %s: requested=%p returned=%p length=%#x status=%#x",
            name, (void *)slot->address, report.fixed_address, (unsigned)PS5_MEM_UNIT, (unsigned)report.mapping_status);
    if (result) say("PS5 alias subrange commit %s: status=%#x", name, (unsigned)result);
    return !result;
}

static bool recycle(Ps5MemBlock *slot) {
    if (!unmap("first slot for recycle", (void *)slot->address, PS5_MEM_UNIT)) return false;
    slot->reserved = false;
    slot->committed = false;

    Reservation replacement = {"first slot for recycle", slot->address, PS5_MEM_UNIT, NULL};
    if (!reserve(&replacement)) {
        // The original address now belongs to neither this slot nor cleanup.
        slot->relocated = replacement.owned;
        return false;
    }
    slot->reserved = true;
    int status = sceKernelReleaseDirectMemory(slot->physical, PS5_MEM_UNIT);
    say("PS5 alias subrange release physical for recycle: offset=%#llx status=%#x",
        (unsigned long long)slot->physical, (unsigned)status);
    if (!status) slot->allocated = false;
    return !status;
}

static bool filled(const volatile unsigned char *data, unsigned char value) {
    for (size_t i = 0; i < PS5_MEM_UNIT; ++i)
        if (data[i] != value) return false;
    return true;
}

bool mkw_ps5_alias_subrange_probe(void) {
    const uint32_t rw = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    const uintptr_t first = PS5_BACKING_ALIAS_BASE + (1ull << 30);
    unsigned char *first_data = (void *)first, *second_data = (void *)(first + PS5_MEM_UNIT);
    enum { ALIAS = 3 };
    Reservation windows[] = {
        {"modules", PS5_BACKING_MODULE_BASE, PS5_BACKING_MODULE_SIZE, NULL},
        {"kernel", PS5_BACKING_KERNEL_BASE, PS5_BACKING_KERNEL_SIZE, NULL},
        {"host functions", PS5_BACKING_HOSTFN_BASE, PS5_BACKING_HOSTFN_SIZE, NULL},
        {"alias", PS5_BACKING_ALIAS_BASE, PS5_BACKING_ALIAS_SIZE, NULL},
        {"stack and heap", PS5_BACKING_STACK_BASE, PS5_BACKING_STACK_SIZE, NULL},
    };
    Reservation tail = {"alias suffix", first + 2 * PS5_MEM_UNIT,
                        PS5_BACKING_ALIAS_BASE + PS5_BACKING_ALIAS_SIZE - first - 2 * PS5_MEM_UNIT, NULL};
    Ps5MemBlock slots[2] = {0};
    bool passed = false;

    for (size_t i = 0; i < sizeof windows / sizeof *windows; ++i)
        if (!reserve(&windows[i])) goto done;
    // Split ownership only in the cleanup ledger. The SDK still holds one
    // alias reservation when the first direct map replaces part of it.
    windows[ALIAS].name = "alias prefix";
    windows[ALIAS].bytes = first - PS5_BACKING_ALIAS_BASE;
    tail.owned = (void *)tail.address;
    slots[0] = (Ps5MemBlock){.address = first, .reserved = true};
    slots[1] = (Ps5MemBlock){.address = first + PS5_MEM_UNIT, .reserved = true};

    if (!commit("first slot", &slots[0]) ||
        !commit("second slot", &slots[1]) || !neighbors(first) ||
        !protection("first slot", slots[0].address, rw) ||
        !protection("second slot", slots[1].address, rw)) goto done;
    memset(first_data, 0xa5, PS5_MEM_UNIT);
    memset(second_data, 0x5a, PS5_MEM_UNIT);
    if (!filled(first_data, 0xa5) || !filled(second_data, 0x5a)) {
        say("PS5 alias subrange initial writes did not read back");
        goto done;
    }

    if (!recycle(&slots[0]) || !protection("re-reserved first slot", first, 0) || !neighbors(first) ||
        !protection("second slot after recycle", slots[1].address, rw)) goto done;
    if (!filled(second_data, 0x5a)) {
        say("PS5 alias subrange second slot changed during recycle");
        goto done;
    }
    if (!commit("first slot again", &slots[0]) || !neighbors(first) ||
        !protection("recommitted first slot", first, rw) ||
        !protection("second slot after recommit", slots[1].address, rw)) goto done;
    passed = filled(first_data, 0) && filled(second_data, 0x5a);
    if (!passed) say("PS5 alias subrange recommit zeroing or sibling data check failed");

done:
    for (size_t i = sizeof slots / sizeof *slots; i-- > 0;) {
        int status = ps5_mem_release(&slots[i]);
        say("PS5 alias subrange cleanup slot %zu: status=%#x", i + 1, (unsigned)status);
        passed &= !status;
    }
    passed &= unmap(tail.name, tail.owned, tail.bytes);
    for (size_t i = sizeof windows / sizeof *windows; i-- > 0;)
        passed &= unmap(windows[i].name, windows[i].owned, windows[i].bytes);
    say("PS5 alias subrange: %s", passed ? "passed" : "failed");
    return passed;
}
