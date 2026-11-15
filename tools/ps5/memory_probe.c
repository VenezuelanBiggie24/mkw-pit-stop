// Exercise the PS5 backing code on small CPU-only blocks in the guest ranges.
// Query protections before testing a protected page to avoid a fault.
#include "ps5_memory.h"
#include "ps5_mixed.h"
#include "backing_layout.h"

#include <ps5platform/kernel.h>
#include <stdio.h>
#include <string.h>

#include "platform.h" // PS5_VulkanTemplate's title logger

static bool check(const char *step, int status) {
    if (status) say("PS5 memory %s: failed (%#x)", step, (unsigned)status);
    return !status;
}

static bool commit(const char *step, Ps5MemBlock *block, bool compare) {
    Ps5MemCommitReport report;
    int result = compare ? ps5_mem_commit_probe(block, &report) : ps5_mem_commit(block, &report);
    if (report.attempted) {
        say("PS5 memory %s allocate-direct: length=%#x alignment=%#x status=%#x",
            step, (unsigned)PS5_MEM_UNIT, (unsigned)PS5_MEM_UNIT, (unsigned)report.allocation_status);
        if (!report.allocation_status)
            say("PS5 memory %s physical: offset=%#llx alignment remainder=%#llx", step,
                (unsigned long long)report.physical, (unsigned long long)report.physical & (PS5_MEM_UNIT - 1));
    }
    if (report.any_attempted) {
        say("PS5 memory %s map-any: returned=%p length=%#x alignment=%#x status=%#x",
            step, report.any_address, (unsigned)PS5_MEM_UNIT, (unsigned)PS5_MEM_UNIT,
            (unsigned)report.any_status);
        if (!report.any_status && report.any_address)
            say("PS5 memory %s unmap-any: status=%#x", step, (unsigned)report.any_unmap_status);
    }
    if (report.fixed_attempted)
        say("PS5 memory %s map-direct: address=%p returned=%p length=%#x alignment=%#x status=%#x",
            step, (void *)block->address, report.fixed_address, (unsigned)PS5_MEM_UNIT, (unsigned)PS5_MEM_UNIT,
            (unsigned)report.mapping_status);
    return check(step, result);
}

static bool filled(const volatile unsigned char *data, size_t bytes, unsigned char value) {
    for (size_t i = 0; i < bytes; i++)
        if (data[i] != value) return false;
    return true;
}

typedef struct {
    const char *name;
    uintptr_t address;
    size_t size;
    bool sample;
    Ps5MemBlock block;
    void *reservation;
} CoexistSpan;

static bool reserve_span(const char *trial, CoexistSpan *span) {
    if (span->sample) {
        int result = ps5_mem_reserve(&span->block, span->address);
        if (!result) return true;
        say("PS5 memory %s reserve %s: address=%p length=%#llx status=%#x",
            trial, span->name, (void *)span->address, (unsigned long long)span->size, (unsigned)result);
        return false;
    }

    void *placed = (void *)span->address;
    int result = sceKernelReserveVirtualRange(&placed, span->size, 0, PS5_MEM_UNIT);
    if (!result) span->reservation = placed;
    if (!result && placed == (void *)span->address) return true;
    say("PS5 memory %s reserve %s: address=%p returned=%p length=%#llx status=%#x",
        trial, span->name, (void *)span->address, placed, (unsigned long long)span->size, (unsigned)result);
    return false;
}

static bool probe_spans(const char *trial, CoexistSpan *spans, size_t count, bool *full_exact) {
    bool reserved = true, backed = true, released = true;
    for (size_t i = 0; i < count && reserved; i++) reserved = reserve_span(trial, &spans[i]);
    if (full_exact) *full_exact = reserved;
    say("PS5 memory %s %s reservation exact: %d", trial, full_exact ? "full" : "sample", reserved);

    unsigned samples = 0;
    if (reserved) {
        // Keep the complementary spans reserved through all commits.
        for (size_t i = 0; i < count; i++) {
            CoexistSpan *span = &spans[i];
            if (!span->sample) continue;
            samples++;
            char step[48];
            snprintf(step, sizeof step, "%s %s", trial, span->name);
            bool mapped = commit(step, &span->block, false);
            backed &= mapped && check(step, !filled((const void *)span->address, PS5_MEM_UNIT, 0));
        }
    }
    say("PS5 memory %s sampled backing: %s (%u 64 KiB units tested)",
        trial, reserved ? (backed ? "passed" : "failed") : "skipped", samples);

    for (size_t i = count; i-- > 0;) {
        CoexistSpan *span = &spans[i];
        int result = span->sample ? ps5_mem_release(&span->block) :
                     span->reservation ? sceKernelMunmap(span->reservation, span->size) : 0;
        if (result) {
            say("PS5 memory %s release %s: failed (%#x)", trial, span->name, (unsigned)result);
            released = false;
        }
    }
    return released;
}

static bool coexist_trial(const char *trial, uintptr_t stack, bool *exact) {
    const uintptr_t last = stack + PS5_BACKING_STACK_SIZE - PS5_MEM_UNIT;
    CoexistSpan spans[] = {
        {.name = "modules", .address = PS5_BACKING_MODULE_BASE, .size = PS5_MEM_UNIT, .sample = true},
        {.name = "modules tail", .address = PS5_BACKING_MODULE_BASE + PS5_MEM_UNIT,
         .size = PS5_BACKING_MODULE_SIZE - PS5_MEM_UNIT},
        {.name = "kernel", .address = PS5_BACKING_KERNEL_BASE, .size = PS5_MEM_UNIT, .sample = true},
        {.name = "kernel tail", .address = PS5_BACKING_KERNEL_BASE + PS5_MEM_UNIT,
         .size = PS5_BACKING_KERNEL_SIZE - PS5_MEM_UNIT},
        {.name = "host functions", .address = PS5_BACKING_HOSTFN_BASE, .size = PS5_BACKING_HOSTFN_SIZE},
        {.name = "alias", .address = PS5_BACKING_ALIAS_BASE, .size = PS5_MEM_UNIT, .sample = true},
        {.name = "alias tail", .address = PS5_BACKING_ALIAS_BASE + PS5_MEM_UNIT,
         .size = PS5_BACKING_ALIAS_SIZE - PS5_MEM_UNIT},
        {.name = "stack first", .address = stack, .size = PS5_MEM_UNIT, .sample = true},
        {.name = "stack middle", .address = stack + PS5_MEM_UNIT, .size = PS5_BACKING_STACK_SIZE - 2 * PS5_MEM_UNIT},
        {.name = "stack last", .address = last, .size = PS5_MEM_UNIT, .sample = true},
    };
    return probe_spans(trial, spans, sizeof spans / sizeof *spans, exact);
}

bool mkw_ps5_coexistence_probe(void) {
    bool low_exact, high_exact;
    if (!coexist_trial("low48 coexist", 48ull << 30, &low_exact) ||
        !coexist_trial("high144 coexist", PS5_BACKING_STACK_BASE, &high_exact)) return false;
    if (low_exact || high_exact) return true;

    // Both full trials cleaned up without reaching any fixed backing checks.
    CoexistSpan modules = {.name = "base", .address = PS5_BACKING_MODULE_BASE, .size = PS5_MEM_UNIT, .sample = true};
    CoexistSpan kernel = {.name = "base", .address = PS5_BACKING_KERNEL_BASE, .size = PS5_MEM_UNIT, .sample = true};
    return probe_spans("isolated modules", &modules, 1, NULL) && probe_spans("isolated kernel", &kernel, 1, NULL);
}

static bool protection(const char *step, void *address, uint32_t expected) {
    void *start = NULL, *end = NULL;
    uint32_t found = 0;
    int result = sceKernelQueryMemoryProtection(address, &start, &end, &found);
    const uint32_t cpu = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_CPU_EXEC;
    const uintptr_t at = (uintptr_t)address;
    bool ok = !result && (uintptr_t)start <= at && (uintptr_t)end >= at + PS5_MEM_PAGE && (found & cpu) == expected;
    say("PS5 memory high144 %s query %p: range=[%p,%p) protection=%#x expectedCPU=%#x status=%#x",
        step, address, start, end, (unsigned)found, (unsigned)expected, (unsigned)result);
    return ok;
}

static bool raw_guest_protection(Ps5MemBlock *block) {
    // Sample an interior 4 KiB slice so both neighbors are in the same native page.
    enum { guest_page = 0x1000 };
    const uint32_t read = PS5_KERNEL_PROT_CPU_READ, rw = read | PS5_KERNEL_PROT_CPU_WRITE;
    const uint32_t cpu = rw | PS5_KERNEL_PROT_CPU_EXEC;
    const uintptr_t left = block->address + PS5_MEM_PAGE;
    const uintptr_t slice = left + guest_page;
    int result = sceKernelMprotect((void *)slice, guest_page, read);
    say("PS5 memory high144 raw-4KiB protect: address=%p length=%#x status=%#x",
        (void *)slice, (unsigned)guest_page, (unsigned)result);

    static const char *const labels[] = {"left", "slice", "right"};
    uint32_t flags[3] = {0};
    bool observed = true;
    for (size_t i = 0; i < 3; i++) {
        uintptr_t address = left + i * guest_page;
        void *start = NULL, *end = NULL;
        uint32_t found = 0;
        int queried = sceKernelQueryMemoryProtection((void *)address, &start, &end, &found);
        say("PS5 memory high144 raw-4KiB query %s: address=%p range=[%p,%p) protection=%#x status=%#x",
            labels[i], (void *)address, start, end, (unsigned)found, (unsigned)queried);
        observed &= !queried && (uintptr_t)start <= address && (uintptr_t)end >= address + guest_page;
        flags[i] = found & cpu;
    }
    bool neighbors_rw = flags[0] == rw && flags[2] == rw;
    const char *outcome = "inconclusive";
    if (observed) {
        if (result && neighbors_rw && flags[1] == rw) outcome = "rejected with sampled protections unchanged";
        else if (!result && neighbors_rw && flags[1] == read) outcome = "isolated 4 KiB read-only slice";
        else if (!neighbors_rw) outcome = "neighbor protection changed";
        else outcome = "unexpected slice protection";
    }
    say("PS5 memory high144 raw-4KiB outcome: %s", outcome);

    // At entry the entire owned unit is writable. Restore it at native-page
    // granularity before the lifecycle test reads or writes it again.
    int restored = ps5_mem_protect(block, 0, PS5_MEM_UNIT, true);
    say("PS5 memory high144 raw-4KiB restore: address=%p length=%#x status=%#x",
        (void *)block->address, (unsigned)PS5_MEM_UNIT, (unsigned)restored);
    if (restored) return false;
    for (size_t offset = 0; offset < PS5_MEM_UNIT; offset += PS5_MEM_PAGE)
        if (!protection("raw-4KiB restore", (void *)(block->address + offset), rw)) return false;
    return true;
}

bool mkw_ps5_memory_probe(uintptr_t stack_base, uintptr_t alias_base) {
    const uint32_t read = PS5_KERNEL_PROT_CPU_READ, rw = read | PS5_KERNEL_PROT_CPU_WRITE;
    Ps5MemBlock stack = {0}, alias = {0};
    unsigned char *page = (void *)stack_base, *neighbor = (void *)(stack_base + PS5_MEM_PAGE);
    unsigned char *alias_page = (void *)alias_base;
    bool passed = false;
    say("PS5 memory high144 lifecycle starts: stack=%p alias=%p", page, alias_page);

    bool stack_reserved = check("reserve stack", ps5_mem_reserve(&stack, stack_base));
    bool alias_reserved = check("reserve alias", ps5_mem_reserve(&alias, alias_base));
    bool stack_ready = stack_reserved && commit("commit stack", &stack, true);
    bool alias_ready = alias_reserved && commit("commit alias", &alias, true);
    if (!stack_ready || !alias_ready) goto done;
    if (!check("initial data", !(filled(page, PS5_MEM_UNIT, 0) && filled(alias_page, PS5_MEM_UNIT, 0)))) goto done;
    memset(page, 0xa5, PS5_MEM_UNIT);
    memset(alias_page, 0x5a, PS5_MEM_UNIT);
    if (!check("RW data", !(filled(page, PS5_MEM_UNIT, 0xa5) && filled(alias_page, PS5_MEM_UNIT, 0x5a)))) goto done;

    if (!raw_guest_protection(&stack)) goto done;
    int protect = ps5_mem_protect(&stack, 0, PS5_MEM_PAGE, false);
    say("PS5 memory high144 lifecycle protect-native: address=%p length=%#x status=%#x",
        page, (unsigned)PS5_MEM_PAGE, (unsigned)protect);
    if (!check("protect 16 KiB", protect) ||
        !protection("lifecycle", page, read) || !protection("lifecycle", neighbor, rw)) goto done;
    int guest_protect = ps5_mem_protect(&stack, PS5_MEM_PAGE, 0x1000, false);
    const bool rejected_4k = guest_protect == PS5_MEM_BAD_REQUEST;
    say("PS5 memory high144 lifecycle protect-guest-4KiB: wrapper status=%#x", (unsigned)guest_protect);
    if (!check("4 KiB rejection", !rejected_4k) ||
        !protection("lifecycle", page, read) || !protection("lifecycle", neighbor, rw) ||
        !check("data after protection", !filled(page, PS5_MEM_UNIT, 0xa5))) goto done;
    *(volatile unsigned char *)neighbor = 0x3c;
    if (!check("neighbor remains writable", !filled(neighbor, 1, 0x3c))) goto done;
    say("PS5 memory protection: 16 KiB read-only, neighbor RW, 4 KiB rejected");

    if (!check("decommit", ps5_mem_decommit(&stack)) || !commit("recommit stack", &stack, false) ||
        !check("recommit zero and alias intact", !(filled(page, PS5_MEM_UNIT, 0) && filled(alias_page, PS5_MEM_UNIT, 0x5a))))
        goto done;
    passed = true;

done:
    passed &= check("release alias", ps5_mem_release(&alias));
    passed &= check("release stack", ps5_mem_release(&stack));
    say("PS5 memory write watch: unavailable (plat_mem_written UINT64_MAX fallback)");
    say("PS5 memory high144 lifecycle backing: %s", passed ? "passed" : "failed");
    return passed;
}

static bool mixed_state(const Ps5MixedBlock *block, uintptr_t address, Ps5GuestPage expected) {
    bool ok = ps5_mixed_state(block, address) == expected &&
              ps5_mixed_ptr(block, address, PS5_GUEST_PAGE, false) ==
                  (expected >= PS5_GUEST_READ ? (void *)address : NULL) &&
              ps5_mixed_ptr(block, address, PS5_GUEST_PAGE, true) ==
                  (expected == PS5_GUEST_RW ? (void *)address : NULL);
    if (!ok) say("PS5 memory mixed-page guest check: address=%p expected=%d", (void *)address, (int)expected);
    return ok;
}

static bool mixed_uniform(Ps5MixedBlock *block, uintptr_t base, Ps5GuestPage mode, uint32_t cpu, const char *step) {
    for (size_t offset = 0; offset < PS5_MEM_PAGE; offset += PS5_GUEST_PAGE)
        if (!check(step, ps5_mixed_protect(block, offset, mode))) return false;
    if (!protection(step, (void *)base, cpu)) return false;
    for (size_t offset = 0; offset < PS5_MEM_PAGE; offset += PS5_GUEST_PAGE)
        if (!mixed_state(block, base + offset, mode)) return false;
    return true;
}

bool mkw_ps5_mixed_probe(uintptr_t base) {
    const uint32_t read = PS5_KERNEL_PROT_CPU_READ, rw = read | PS5_KERNEL_PROT_CPU_WRITE;
    const size_t guest = PS5_GUEST_PAGE;
    Ps5MixedBlock block = {0};
    bool passed = false;
    if (!check("mixed-page reserve", ps5_mixed_reserve(&block, base))) goto done;
    for (size_t offset = 0; offset < 3 * guest; offset += guest)
        if (!check("mixed-page commit", ps5_mixed_commit(&block, offset))) goto done;
    if (!protection("mixed-page initial RW", (void *)base, rw)) goto done;
    unsigned char *initial_sibling = ps5_mixed_ptr(&block, base, guest, true);
    unsigned char *sample = ps5_mixed_ptr(&block, base + guest, guest, true);
    if (!check("mixed-page initial pointers", !initial_sibling || !sample)) goto done;
    memset(initial_sibling, 0x5a, guest);
    memset(sample, 0xa5, guest);

    if (!check("mixed-page protect RO", ps5_mixed_protect(&block, guest, PS5_GUEST_READ)) ||
        !check("mixed-page protect NONE", ps5_mixed_protect(&block, 2 * guest, PS5_GUEST_NONE)) ||
        !protection("mixed-page guest RW/RO/NONE/RESERVED", (void *)base, rw) ||
        !mixed_state(&block, base, PS5_GUEST_RW) ||
        !mixed_state(&block, base + guest, PS5_GUEST_READ) ||
        !mixed_state(&block, base + 2 * guest, PS5_GUEST_NONE) ||
        !mixed_state(&block, base + 3 * guest, PS5_GUEST_RESERVED)) goto done;
    const volatile unsigned char *saved = ps5_mixed_ptr(&block, base + guest, guest, false);
    volatile unsigned char *sibling = ps5_mixed_ptr(&block, base, 1, true);
    if (!check("mixed-page mixed data", !saved || !filled(saved, guest, 0xa5) || !sibling || *sibling != 0x5a)) goto done;
    *sibling = 0x3c;
    if (!check("mixed-page sibling write", *sibling != 0x3c)) goto done;

    if (!check("mixed-page partial decommit", ps5_mixed_decommit(&block, guest)) ||
        !mixed_state(&block, base + guest, PS5_GUEST_RESERVED) ||
        !check("mixed-page partial recommit", ps5_mixed_commit(&block, guest)) ||
        !protection("mixed-page recommit RW", (void *)base, rw)) goto done;
    saved = ps5_mixed_ptr(&block, base + guest, guest, false);
    const volatile unsigned char *neighbor = ps5_mixed_ptr(&block, base, guest, false);
    if (!check("mixed-page recommit zero and sibling intact", !saved || !filled(saved, guest, 0) ||
            !neighbor || *neighbor != 0x3c || !filled(neighbor + 1, guest - 1, 0x5a))) goto done;

    if (!check("mixed-page commit fourth slice", ps5_mixed_commit(&block, 3 * guest)) ||
        !mixed_uniform(&block, base, PS5_GUEST_READ, read, "mixed-page uniform RO") ||
        !mixed_uniform(&block, base, PS5_GUEST_NONE, 0, "mixed-page uniform NONE") ||
        !mixed_uniform(&block, base, PS5_GUEST_RW, rw, "mixed-page restore RW")) goto done;
    neighbor = ps5_mixed_ptr(&block, base, 1, false);
    passed = check("mixed-page restored data", !neighbor || *neighbor != 0x3c);

done:
    passed &= check("mixed-page release", ps5_mixed_release(&block));
    say("PS5 memory mixed-page: %s", passed ? "passed" : "failed");
    return passed;
}
