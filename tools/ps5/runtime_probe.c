// Exercise MKW's dispatcher with synthetic callbacks in the smoke title.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "dispatch.h"
#include "host.h"
#include "platform.h" // Template klog adapter
#include "platform/platform.h"

#ifndef PS5_HOST_REFERENCE
#include <ps5platform/kernel.h>
#endif

#ifdef PS5_HOST_REFERENCE
static const char probe_name[] = "Host runtime dispatch";
#else
static const char probe_name[] = "PS5 runtime dispatch";
#endif

// The smoke title owns one anonymous table. Guest mappings use a separate
// implementation and must preserve their 4 KiB permission boundaries.
static void *table;
static size_t table_size;
static bool table_zeroed;
static bool table_sealed;

void *plat_mem_alloc(size_t size) {
    if (table || !size) return NULL;
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return NULL;
    table = mapping;
    table_size = size;
    const uint8_t *bytes = mapping;
    table_zeroed = true;
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i]) {
            table_zeroed = false;
            break;
        }
    }
    return table;
}

int plat_mem_protect(uint64_t address, uint64_t size, int protection) {
    if (!table || address != (uintptr_t)table || size != table_size || protection != PLAT_PROT_READ) return 0;
#ifdef PS5_HOST_REFERENCE
    table_sealed = mprotect(table, table_size, PROT_READ) == 0;
#else
    if (address % PS5_KERNEL_PAGE_SIZE || size % PS5_KERNEL_PAGE_SIZE) return 0;
    table_sealed = sceKernelMprotect(table, table_size, PS5_KERNEL_PROT_CPU_READ) == 0;
#endif
    return table_sealed;
}

static bool table_read_only(void) {
    if (!table_sealed) return false;
#ifndef PS5_HOST_REFERENCE
    const uint32_t cpu = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_CPU_EXEC;
    uintptr_t current = (uintptr_t)table;
    const uintptr_t limit = current + table_size;
    while (current < limit) {
        void *start = NULL, *end = NULL;
        uint32_t protection = 0;
        const int status = sceKernelQueryMemoryProtection((void *)current, &start, &end, &protection);
        say("%s: table protection [%p, %p) flags=%#x status=%#x", probe_name, start, end, protection,
            (unsigned)status);
        if (status || (uintptr_t)start > current || (uintptr_t)end <= current ||
            (protection & cpu) != PS5_KERNEL_PROT_CPU_READ)
            return false;
        current = (uintptr_t)end;
    }
#endif
    return true;
}

void rt_log_raw(const char *text, size_t length) {
    if (length == 7 && !memcmp(text, "FATAL: ", 7)) say("%s: failed", probe_name);
    fwrite(text, 1, length, stderr);
}

void rt_vlogf(const char *format, va_list args) { vfprintf(stderr, format, args); }

// The fixture has no guest thread. The link still needs the dispatcher's
// preemption target, and reaching it is an error in this title.
void kern_checkpoint(void) { rt_fatal("synthetic dispatch entered the scheduler"); }

static void add(Cpu *cpu) { cpu->x[0] += 3; }
static void double_value(Cpu *cpu) { cpu->x[0] *= 2; }
static void host_xor(Cpu *cpu) { cpu->x[0] ^= 0x14; }

static bool expect(bool ok, const char *test) {
    if (!ok) say("%s: %s failed", probe_name, test);
    return ok;
}

bool mkw_runtime_dispatch_probe(void) {
    static int verdict;
    if (verdict) return verdict > 0;

    // The dispatcher holds this module by pointer until the process exits.
    static const uint64_t addrs[] = {RT_MODULE_BASE + 0x100, RT_MODULE_BASE + 0x10c};
    static const GuestFn functions[] = {add, double_value};
    static const RtModule module = {"synthetic", RT_MODULE_BASE, 0x4000, addrs, functions, 2};
    const uint64_t host_pc = RT_HOSTFN_BASE + 16;
    rt_register_host(host_pc, host_xor);
    rt_dispatch_init(&module, 1);

    const bool module_entries = expect(rt_dispatch_lookup(addrs[0]) == add &&
                                       rt_dispatch_lookup(addrs[1]) == double_value, "module lookup");
    const bool host_entry = expect(rt_dispatch_lookup(host_pc) == host_xor, "host lookup");
    bool ok = module_entries && host_entry;
    ok &= expect(!rt_dispatch_lookup(addrs[0] + 4) && !rt_dispatch_lookup(addrs[0] + 1) &&
                 !rt_dispatch_lookup(RT_KERNEL_BASE), "gap, misaligned and out-of-range lookups");
    const RtDispatchStats stats = rt_dispatch_stats();
    ok &= expect(stats.lo == addrs[0] && stats.hi == addrs[1] + 4 && stats.slots == 4 &&
                 stats.table_bytes == 0x10000 && stats.table_bytes == table_size &&
                 stats.functions == 2 && stats.host_fns == 1, "table statistics");
    ok &= expect(table_zeroed, "zeroed table allocation");
    ok &= expect(!plat_mem_protect(RT_MODULE_BASE, table_size, PLAT_PROT_READ), "unowned protection rejection");
    ok &= expect(table_read_only(), "read-only table protection");

    if (module_entries && host_entry) {
        Cpu cpu = {.x = {7}, .pc = addrs[0]};
        rt_dispatch(&cpu);
        ok &= expect(cpu.x[0] == 10, "first module callback");
        cpu.pc = addrs[1];
        rt_dispatch(&cpu);
        ok &= expect(cpu.x[0] == 20, "second module callback");
        cpu.pc = host_pc;
        rt_dispatch(&cpu);
        ok &= expect(cpu.x[0] == 0 && cpu.thread == NULL, "host callback");
    }
    say("%s: %s", probe_name, ok ? "passed" : "failed");
    verdict = ok ? 1 : -1;
    return ok;
}
