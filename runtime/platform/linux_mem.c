// Linux implementation of platform.h, virtual memory, the write watch and the fault handler (the parts: linux_int.h).
#ifndef _GNU_SOURCE   // build.py passes it on Linux
#define _GNU_SOURCE
#endif
#if defined(__aarch64__)
#include <asm/sigcontext.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL_clipboard.h>

#include "platform.h"
#include "linux_int.h"
#include "prot_watch.h"

void plat_mem_census_log(const char *tag) { (void)tag; }


// The write-watch ABI of Linux 6.7 (uapi linux/fs.h and linux/userfaultfd.h), for headers older than it: the release
// builds against the Steam Runtime's 5.10 headers and runs on newer kernels. The kernel refuses what it lacks at run time.
#ifndef PAGEMAP_SCAN
struct page_region {
    __u64 start;
    __u64 end;
    __u64 categories;
};
struct pm_scan_arg {
    __u64 size;
    __u64 flags;
    __u64 start;
    __u64 end;
    __u64 walk_end;
    __u64 vec;
    __u64 vec_len;
    __u64 max_pages;
    __u64 category_inverted;
    __u64 category_mask;
    __u64 category_anyof_mask;
    __u64 return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PAGE_IS_PRESENT (1 << 3)
#define PAGE_IS_SWAPPED (1 << 4)
#define PAGE_IS_PFNZERO (1 << 5)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

// ---- virtual memory. Reservations are PROT_NONE MAP_NORESERVE mappings; commit is mprotect read-write, decommit
// MADV_DONTNEED (private anonymous pages read zero afterwards) and PROT_NONE. Linux keeps no committed/reserved
// distinction, so a byte per guest page records it (pstate, mapped lazily: only the pages touched cost memory).
//
// Write watch: a userfaultfd in asynchronous write-protect mode (Linux 6.7) over the watched reservations. The kernel
// clears a page's protection on the first write without a round trip to us, and PAGEMAP_SCAN lists the pages whose
// protection was cleared and protects them again. Without it (an older kernel), plat_mem_written reports the ranges as
// untracked and the back end uploads them in full.

#define PG 4096ull
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };   // PS_NONE.. are committed: PLAT_PROT_* + PS_NONE
static uint8_t *pstate;
static int ww_fd = -1, pm_fd = -1;   // userfaultfd and /proc/self/pagemap; -1 when write watch is unavailable
static PlatMutex vm_lock = PLAT_MUTEX_INIT;

static void ps_set(uint64_t addr, uint64_t n, uint8_t v) {
    if (addr < PS_LIMIT) memset(pstate + addr / PG, v, (size_t)((n + PG - 1) / PG));
}

static int use_prot_watch = -1;

static int linux_pw_protect(uint64_t addr, size_t size, int rw) {
    return mprotect((void *)(uintptr_t)addr, size, rw ? (PROT_READ | PROT_WRITE) : PROT_READ) == 0;
}

static int linux_pw_state(uint64_t addr) {
    if (!pstate || addr >= PS_LIMIT) return PLAT_MEM_FREE;
    uint8_t v = __atomic_load_n(&pstate[addr / PG], __ATOMIC_RELAXED);
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

static int linux_early_16k(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("MKW_EARLY_16K");
        v = (e && e[0] == '1' && !e[1]) ? 1 : 0;
    }
    return v;
}

size_t plat_mem_early_page_size(void) { return linux_early_16k() ? 16384 : PG; }

static int linux_prot_watch_on(void) {
    int v = __atomic_load_n(&use_prot_watch, __ATOMIC_ACQUIRE);
    if (v >= 0) return v;
    const char *e = getenv("MKW_PROT_WATCH");
    v = (e && e[0] == '1' && !e[1]) ? 1 : 0;
    if (v) prot_watch_init(linux_early_16k() ? 16384 : PG, linux_pw_protect, linux_pw_state);
    __atomic_store_n(&use_prot_watch, v, __ATOMIC_RELEASE);
    return v;
}

// The soft-dirty watch is per 4 KiB page; the protection watch uses the page size it was started with above.
size_t plat_mem_watch_page(void) { return linux_prot_watch_on() && linux_early_16k() ? 16384 : PG; }

static void fault_install(void);

static void ww_open(void) {
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API,
                             .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    if (fd < 0 || ioctl(fd, UFFDIO_API, &api)) {
        fprintf(stderr, "platform: no asynchronous userfaultfd write protection (%s): written memory is not tracked\n",
                strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm_fd < 0) {
        close(fd);
        return;
    }
    ww_fd = fd;
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    plat_mutex_lock(&vm_lock);
    if (!pstate) {
        void *p = mmap(NULL, PS_LIMIT / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        pstate = p == MAP_FAILED ? NULL : p;
    }
    int pw = watch && linux_prot_watch_on();
    if (pw) fault_install();
    if (watch && !pw && ww_fd < 0 && pm_fd < 0) ww_open();
    void *p = pstate && addr + n <= PS_LIMIT
                  ? mmap((void *)(uintptr_t)addr, n, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0)
                  : MAP_FAILED;
    int ok = p == (void *)(uintptr_t)addr;
    if (p != MAP_FAILED && !ok) munmap(p, n);   // a kernel without MAP_FIXED_NOREPLACE put it elsewhere
    if (ok) {
        ps_set(addr, n, PS_RESERVED);
        // Transparent huge pages would populate (and count as written) 2 MiB at the first write of one page; writes are
        // tracked per 4 KiB page, as on Windows.
        if (watch) madvise(p, n, MADV_NOHUGEPAGE);
        if (pw) {
            prot_watch_reserve(addr, n);
        } else {
            struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
            if (watch && ww_fd >= 0 && ioctl(ww_fd, UFFDIO_REGISTER, &reg))
                fprintf(stderr, "platform: write watch over %#llx+%#llx failed (%s)\n", (unsigned long long)addr,
                        (unsigned long long)n, strerror(errno));
        }
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

void plat_mem_name(const void *p, size_t n, const char *name) {
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif
    // The kernel copies the name; a kernel without CONFIG_ANON_VMA_NAME refuses it and the range stays "anon".
    if (p && n) prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)(uintptr_t)p, (unsigned long)n, name);
}

int64_t plat_mem_resident(const void *p, size_t n) {
    unsigned char v[4096];   // 16 MiB of pages per call
    int64_t in = 0;
    for (size_t off = 0; off < n; off += sizeof v * 4096) {
        size_t len = n - off < sizeof v * 4096 ? n - off : sizeof v * 4096;
        if (mincore((char *)p + off, len, v)) return -1;
        for (size_t i = 0; i < (len + 4095) / 4096; i++) in += (v[i] & 1) * 4096;
    }
    return in;
}

void *plat_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_mem_commit(uint64_t addr, size_t n) {
    if (mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE)) return 0;
    ps_set(addr, n, PS_RW);
    if (linux_prot_watch_on()) prot_watch_touch(addr, n);
    return 1;
}

int plat_mem_decommit(uint64_t addr, size_t n) {
    if (madvise((void *)(uintptr_t)addr, n, MADV_DONTNEED) || mprotect((void *)(uintptr_t)addr, n, PROT_NONE)) return 0;
    ps_set(addr, n, PS_RESERVED);
    if (linux_prot_watch_on()) prot_watch_mark_written(addr, n);
    return 1;
}

// The guest backing of [addr, addr + size) given back, the pages kept committed: they read zero from then on, as the
// PS5's ps5_mem_decommit leaves them (ps5_guest_memory.c). Linux had none (common.c's weak version), so
// MKW_GPU_ONLY_RELEASE did nothing here and run 28's magenta could not be seen on Linux (14/11/2026). Whole pages
// inside the range, committed read-write ones only.
uint64_t plat_mem_release_backing(uint64_t addr, uint64_t size, PlatBackingDiscarded discarded) {
    if (!size || size > UINT64_MAX - addr || addr > UINT64_MAX - (PG - 1)) return 0;
    uint64_t lo = (addr + PG - 1) & ~(uint64_t)(PG - 1), hi = (addr + size) & ~(uint64_t)(PG - 1);
    uint64_t freed = 0;
    for (uint64_t a = lo, b; a < hi; a = b) {
        int st = plat_mem_state(a, hi, &b);
        if (b <= a) break;
        if (st != PLAT_PROT_RW) continue;
        uint64_t backed = plat_mem_backed_bytes(a, b - a);
        if (madvise((void *)(uintptr_t)a, b - a, MADV_DONTNEED)) continue;
        freed += backed;
        if (linux_prot_watch_on()) prot_watch_mark_written(a, b - a);
        if (discarded && !discarded(a, b)) break;
    }
    return freed;
}

// MKW_POOL_RELEASE's version: only whole pages that hold zeros, committed read-write (an anonymous page given back reads
// zero again, so nothing changes for the game; task 32).
uint64_t plat_mem_release_zero_backing(uint64_t addr, uint64_t size) {
    if (!size || size > UINT64_MAX - addr || addr > UINT64_MAX - (PG - 1)) return 0;
    uint64_t lo = (addr + PG - 1) & ~(uint64_t)(PG - 1), hi = (addr + size) & ~(uint64_t)(PG - 1), freed = 0;
    for (uint64_t a = lo, b; a < hi; a = b) {
        int st = plat_mem_state(a, hi, &b);
        if (b <= a) break;
        if (st != PLAT_PROT_RW) continue;
        for (uint64_t p = a; p < b; p += PG) {
            const uint64_t *q = (const uint64_t *)(uintptr_t)p;
            int zero = 1;
            for (uint64_t k = 0; zero && k < PG / sizeof(uint64_t); k++) zero = !q[k];
            if (!zero) continue;
            uint64_t backed = plat_mem_backed_bytes(p, PG);
            if (madvise((void *)(uintptr_t)p, PG, MADV_DONTNEED)) continue;
            freed += backed;
            if (linux_prot_watch_on()) prot_watch_mark_written(p, PG);
        }
    }
    return freed;
}

// Resident pages of [addr, addr + size) (mincore), in bytes: what a release gave back.
uint64_t plat_mem_backed_bytes(uint64_t addr, uint64_t size) {
    uint64_t lo = addr & ~(uint64_t)(PG - 1), hi = (addr + size + PG - 1) & ~(uint64_t)(PG - 1), n = 0;
    unsigned char v[256];
    for (uint64_t a = lo; a < hi; a += sizeof v * PG) {
        uint64_t len = hi - a < sizeof v * PG ? hi - a : sizeof v * PG;
        if (mincore((void *)(uintptr_t)a, len, v)) continue;
        for (uint64_t i = 0; i < len / PG; i++) n += (v[i] & 1) ? PG : 0;
    }
    return n;
}

PlatEarlyResult plat_mem_early_protect(uint64_t addr, size_t n, int on, int restore_prot) {
    int pw = linux_prot_watch_on();
    size_t ep = plat_mem_early_page_size();
    if (on) {
        if (pw)
            for (uint64_t p = addr; p < addr + n; p += ep) prot_watch_set_early(p, 1);
        if (!mprotect((void *)(uintptr_t)addr, n, PROT_NONE)) return PLAT_EARLY_OK;
        // A failed mprotect over several VMAs may have changed a prefix. Restore each guest page before retrying.
        if (!pstate) return PLAT_EARLY_UNRESTORED;
    }
    if (!on && !(ep > PG && pstate) && !(restore_prot == PLAT_PROT_RW && pw)) {
        int prot = restore_prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE
                 : restore_prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
        if (mprotect((void *)(uintptr_t)addr, n, prot)) return PLAT_EARLY_UNRESTORED;
        if (pw)
            for (uint64_t p = addr; p < addr + n; p += ep) prot_watch_set_early(p, 0);
        return PLAT_EARLY_OK;
    }
    for (uint64_t p = addr; p < addr + n; p += ep) {
        for (uint64_t q = p; q < p + ep; q += PG) {
            int st = restore_prot;
            if (on || (ep > PG && pstate)) {
                uint8_t v = q < PS_LIMIT ? pstate[q / PG] : PS_FREE;
                if (on && v == PS_FREE) continue;
                st = v == PS_RW ? PLAT_PROT_RW : v == PS_READ ? PLAT_PROT_READ : PLAT_PROT_NONE;
            }
            int prot = st == PLAT_PROT_RW && !(pw && prot_watch_is_armed(q)) ? PROT_READ | PROT_WRITE
                     : st == PLAT_PROT_READ || st == PLAT_PROT_RW ? PROT_READ : PROT_NONE;
            if (mprotect((void *)(uintptr_t)q, PG, prot)) return PLAT_EARLY_UNRESTORED;
        }
        if (pw) prot_watch_set_early(p, 0);
    }
    return on ? PLAT_EARLY_FAILED : PLAT_EARLY_OK;
}

int plat_mem_drop(uint64_t addr, uint64_t n) { return madvise((void *)(uintptr_t)addr, n, MADV_DONTNEED) == 0; }

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    if (prot == PLAT_PROT_RW && linux_prot_watch_on()) {
        for (uint64_t p = addr, end = addr + n; p < end; p += PG) {
            int want = prot_watch_is_armed(p) ? PROT_READ : (PROT_READ | PROT_WRITE);
            if (mprotect((void *)(uintptr_t)p, PG, want)) return 0;
        }
        ps_set(addr, n, (uint8_t)(PS_NONE + prot));
        return 1;
    }
    int p = prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE : prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
    if (mprotect((void *)(uintptr_t)addr, n, p)) return 0;
    ps_set(addr, n, (uint8_t)(PS_NONE + prot));
    return 1;
}

// /proc/self/mem writes with FOLL_FORCE, so inaccessible pages of a private mapping take the bytes. A kernel built
// without that (proc_mem.force_override=never) refuses: checked once on a page of our own.
static int self_mem(void);

int plat_mem_poke(uint64_t addr, const void *src, size_t n) {
    int fd = self_mem();
    return fd >= 0 && pwrite(fd, src, n, (off_t)addr) == (ssize_t)n;
}

// Reads through the page protection, as plat_mem_poke writes: the runtime copying a page MKW_EARLY_READ watches.
// MKW_PEEK_PLAIN (a test of the PS5's way on Linux): plat_mem_peek reads as the PS5's plat_mem_read does, a plain copy
// with a lock held that plat_mem_state also takes (the PS5's vm_lock), instead of through /proc/self/mem. 1: the copy
// guarded as on the PS5 now (fault_guard.c); 2: unguarded, as in run 25, whose hang it reproduces.
static int peek_plain = -1;
static pthread_mutex_t peek_lock = PTHREAD_MUTEX_INITIALIZER;
static int peek_mode(void) {
    if (peek_plain < 0) {
        char v[8] = "";
        plat_getenv("MKW_PEEK_PLAIN", v, sizeof v);
        peek_plain = atoi(v);
    }
    return peek_plain;
}

int plat_mem_peek(void *dst, uint64_t addr, size_t n) {
    int m = peek_mode();
    if (m) {
        pthread_mutex_lock(&peek_lock);
        int ok = 1;
        if (m == 1) ok = plat_mem_copy_guarded(dst, (const void *)(uintptr_t)addr, n);
        else memcpy(dst, (const void *)(uintptr_t)addr, n);
        pthread_mutex_unlock(&peek_lock);
        return ok;
    }
    int fd = self_mem();
    return fd >= 0 && pread(fd, dst, n, (off_t)addr) == (ssize_t)n;
}

static int self_mem(void) {   // /proc/self/mem, once: -1 when it cannot write through a no-access page
    static int fd = -2;
    if (fd == -2) {
        int f = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
        char *t = mmap(NULL, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = f >= 0 && t != MAP_FAILED && pwrite(f, "x", 1, (off_t)(uintptr_t)t) == 1;
        if (t != MAP_FAILED) munmap(t, PG);
        if (!ok && f >= 0) close(f);
        __atomic_store_n(&fd, ok ? f : -1, __ATOMIC_RELEASE);
    }
    return fd;
}

static int mem_state_unlocked(uint64_t addr, uint64_t limit, uint64_t *end);
int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (peek_mode()) {   // MKW_PEEK_PLAIN: the PS5's vm_lock order
        pthread_mutex_lock(&peek_lock);
        int st = mem_state_unlocked(addr, limit, end);
        pthread_mutex_unlock(&peek_lock);
        return st;
    }
    return mem_state_unlocked(addr, limit, end);
}

static int mem_state_unlocked(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (!pstate || addr >= PS_LIMIT) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < PS_LIMIT ? limit : PS_LIMIT, i = addr / PG;
    uint8_t v = pstate[i];
    while ((i + 1) * PG < lim && pstate[i + 1] == v) i++;
    *end = (i + 1) * PG < lim ? (i + 1) * PG : lim;
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

// MKW_SNAP_SCOPED (memory.c mem_written_any): no peek here, so always "may be written" (the caller queries).
int plat_mem_written_any(uint64_t addr, uint64_t n) { (void)addr, (void)n; return 1; }
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    if (linux_prot_watch_on()) return prot_watch_written(addr, n, pages, max, reset, NULL);
    if (ww_fd < 0) return UINT64_MAX;
    struct page_region r[256];
    uint64_t got = 0;
    for (uint64_t at = addr, end = addr + n; at < end && got < max;) {
        // Written means: write-protection cleared, on a populated page (present or swapped out) that is not the shared
        // zero page. A page never touched since its commit has no protection marker either and counts as written to
        // the kernel; a read maps the zero page there. Neither is a write, as on Windows.
        struct pm_scan_arg arg = {.size = sizeof arg,
                                  .flags = reset ? PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC : 0,
                                  .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 256,
                                  .max_pages = max - got,
                                  .category_inverted = PAGE_IS_PFNZERO,
                                  .category_mask = PAGE_IS_WRITTEN | PAGE_IS_PFNZERO,
                                  .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                                  .return_mask = PAGE_IS_WRITTEN};
        long k = ioctl(pm_fd, PAGEMAP_SCAN, &arg);
        if (k < 0) return UINT64_MAX;
        for (long i = 0; i < k; i++)
            for (uint64_t p = r[i].start; p < r[i].end && got < max; p += PG) pages[got++] = p;
        if (arg.walk_end <= at) break;
        at = arg.walk_end;
    }
    return got;
}

uint64_t plat_mem_populated(uint64_t addr, uint64_t n, uint64_t (*runs)[2], uint64_t max) {
    if (!max || !n) return 0;
    static int own = -1;   // not pm_fd: that one being open means write watch is on (plat_mem_reserve)
    if (pm_fd < 0 && own < 0) own = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    int fd = pm_fd >= 0 ? pm_fd : own;
    if (fd < 0) return UINT64_MAX;
    struct page_region r[256];
    uint64_t got = 0;
    for (uint64_t at = addr, end = addr + n; at < end && got < max;) {
        struct pm_scan_arg arg = {.size = sizeof arg, .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r,
                                  .vec_len = 256, .category_inverted = PAGE_IS_PFNZERO,
                                  .category_mask = PAGE_IS_PFNZERO,
                                  .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED};
        long k = ioctl(fd, PAGEMAP_SCAN, &arg);
        if (k < 0) return UINT64_MAX;
        for (long i = 0; i < k && got < max; i++) {
            if (got && runs[got - 1][1] == r[i].start) runs[got - 1][1] = r[i].end;
            else runs[got][0] = r[i].start, runs[got][1] = r[i].end, got++;
        }
        if (arg.walk_end <= at) break;
        at = arg.walk_end;
    }
    return got;
}

// One handler for the synchronous faults. A SIGSEGV goes to the fault hook first (memory.c thaws a frozen region on a
// write, fills a read-trapped page on any access); a fault in this executable's code then goes to the crash
// hook (main.c); anything else gets the default action. Runs on the thread's signal stack.
static int (*fault_hook)(uint64_t addr);
static int (*watch_fn)(uint64_t addr, uint64_t rip);   // plat_mem_watch_hook: write watches set for diagnostics
static void (*crash_fn)(const PlatCrash *c);

void plat_mem_watch_hook(int (*fn)(uint64_t addr, uint64_t rip)) { watch_fn = fn; }
void plat_mem_lazy_hook(int (*fn)(uint64_t addr)) { (void)fn; }   // Linux commits on touch

_Thread_local int plat_fault_write;        // platform.h
_Thread_local uint64_t plat_fault_pc, plat_fault_ret;

#if defined(__aarch64__)
// x86's page fault error code from the ESR the kernel puts in the signal frame (an esr_context record): 2 a write
// (data abort with WnR), 16 an instruction fetch (instruction abort). A leaf's caller and a wild call's return address
// are in x30 there, not on the stack.
static long fault_err(const ucontext_t *u) {
    const unsigned char *p = u->uc_mcontext.__reserved, *end = p + sizeof u->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const struct _aarch64_ctx *)p;
        if (!h->magic || !h->size) break;
        if (h->magic == ESR_MAGIC) {
            uint64_t esr = ((const struct esr_context *)h)->esr, ec = esr >> 26;
            if (ec == 0x20 || ec == 0x21) return 16;
            if ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6))) return 2;
            return 0;
        }
        p += h->size;
    }
    return 0;
}
#define UC_PC(u) ((u)->uc_mcontext.pc)
#define UC_SP(u) ((u)->uc_mcontext.sp)
#define UC_RET(u) ((u)->uc_mcontext.regs[30])
#else
static long fault_err(const ucontext_t *u) { return u->uc_mcontext.gregs[REG_ERR]; }
#define UC_PC(u) ((u)->uc_mcontext.gregs[REG_RIP])
#define UC_SP(u) ((u)->uc_mcontext.gregs[REG_RSP])
#define UC_RET(u) (*(const uint64_t *)(uintptr_t)(u)->uc_mcontext.gregs[REG_RSP])
#endif

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
    long err = fault_err(u);
    if (sig == SIGSEGV && (err & 2) && linux_prot_watch_on() &&
        prot_watch_fault((uint64_t)(uintptr_t)si->si_addr))
        return;
    if (sig == SIGSEGV && watch_fn && (err & 2) &&
        watch_fn((uint64_t)(uintptr_t)si->si_addr, (uint64_t)UC_PC(u)))
        return;
    if (sig == SIGSEGV || sig == SIGBUS) plat_fault_guarded();   // a guarded copy (fault_guard.c): no hook, back to it
    plat_fault_write = (err & 2) != 0, plat_fault_pc = (uint64_t)UC_PC(u);   // for the hook
    plat_fault_ret = (uint64_t)UC_RET(u);   // a leaf (libc memcpy): its caller
    plat_fault_uc = uc;   // (plat_fault_emulate_load)
    int retry = sig == SIGSEGV && fault_hook && fault_hook((uint64_t)(uintptr_t)si->si_addr);
    plat_fault_uc = NULL;
    if (retry) {   // reads too: traps
#if !defined(__aarch64__)
        plat_fault_step_arm((uint64_t *)&u->uc_mcontext.gregs[REG_EFL]);   // (the hook asked for one instruction)
#endif
        return;
    }
    uintptr_t rip = (uintptr_t)UC_PC(u);
    Dl_info at = {0}, self = {0};
    // Ours: this executable, the game code library (mkwgame.so), or no module at all (a jump to a wild address)
    int in_module = dladdr((void *)rip, &at) != 0, ours_exe = 0, ours_game = 0;
    if (in_module && dladdr((void *)plat_crash_hook, &self)) {
        ours_exe = at.dli_fbase == self.dli_fbase;
        const char *base = at.dli_fname ? strrchr(at.dli_fname, '/') : NULL;
        ours_game = base && !strcmp(base + 1, "mkwgame.so");
    }
    if (crash_fn && (!in_module || ours_exe || ours_game)) {
        PlatCrash c = {sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : "arithmetic exception",
                       (uint32_t)sig, rip, in_module ? rip - (uintptr_t)at.dli_fbase : rip, -1, 0,
                       !in_module ? "no module" : ours_exe ? "mkw" : "mkwgame", 0};
#if defined(__aarch64__)
        if (!in_module) c.ret = (uintptr_t)UC_RET(u);   // a wild call left its return address in x30
#else
        uintptr_t sp = (uintptr_t)UC_SP(u);   // a wild call left its return address there
        if (!in_module && sp && !(sp & 7) && plat_mem_readable(sp, 8)) c.ret = *(const uintptr_t *)sp;
#endif
        if (sig == SIGSEGV || sig == SIGBUS)
            c.access = (err & 16) ? 8 : (err & 2) ? 1 : 0, c.addr = (uint64_t)(uintptr_t)si->si_addr;
        crash_fn(&c);
    } else {
        // Outside this executable (a driver or system library): no crash report, but say where before dying.
        Dl_info lib = {0};
        dladdr((void *)rip, &lib);
        char line[512];
        int n = snprintf(line, sizeof line, "[host] fatal signal %d at %#lx (%s+%#lx, %s) addr %p err %#lx\n", sig,
                         (unsigned long)rip, lib.dli_fname ? lib.dli_fname : "?",
                         (unsigned long)(rip - (uintptr_t)lib.dli_fbase), lib.dli_sname ? lib.dli_sname : "?",
                         si->si_addr, (unsigned long)err);
        if (n > 0) (void)!write(2, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        void *frames[32];
        backtrace_symbols_fd(frames, backtrace(frames, 32), 2);   // the host call into the library
    }
    signal(sig, SIG_DFL);   // returns into the faulting instruction, which faults again with the default action
}

#if !defined(__aarch64__)
// The debug trap after an instruction the fault hook let through (plat_fault_step); any other SIGTRAP as by default.
static void trap_handler(int sig, siginfo_t *si, void *uc) {
    (void)si;
    ucontext_t *u = uc;
    if (plat_fault_step_trap((uint64_t *)&u->uc_mcontext.gregs[REG_EFL])) return;
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
#if !defined(__aarch64__)
    struct sigaction st = {.sa_sigaction = trap_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&st.sa_mask);
    if (!sigaction(SIGTRAP, &st, NULL)) plat_fault_step_init();
#endif
}

int plat_mem_back(uint64_t addr, size_t n) {
    if (linux_prot_watch_on()) prot_watch_unprotect_and_mark(addr, n);
    return 1;   // Linux backs anonymous pages inside kernel writes automatically.
}

static void (*early_hold_fn)(uint64_t addr, size_t n);
void plat_mem_early_hold_hook(void (*fn)(uint64_t addr, size_t n)) { __atomic_store_n(&early_hold_fn, fn, __ATOMIC_RELEASE); }

static void (*thaw_fn)(uint64_t addr, size_t n);
void plat_mem_thaw_hook(void (*fn)(uint64_t addr, size_t n)) { __atomic_store_n(&thaw_fn, fn, __ATOMIC_RELEASE); }

void plat_mem_early_resolve(uint64_t addr, size_t n) {
    if (!n) return;
    void (*eh)(uint64_t, size_t) = __atomic_load_n(&early_hold_fn, __ATOMIC_ACQUIRE);
    if (eh) eh(addr, n);
    void (*th)(uint64_t, size_t) = __atomic_load_n(&thaw_fn, __ATOMIC_ACQUIRE);
    if (th) th(addr, n);
}

int plat_mem_back_hold(uint64_t addr, size_t n) {
    void (*eh)(uint64_t, size_t) = __atomic_load_n(&early_hold_fn, __ATOMIC_ACQUIRE);
    if (eh) eh(addr, n);
    if (linux_prot_watch_on()) prot_watch_hold(addr, n);
    return 1;
}

void plat_mem_back_release(uint64_t addr, size_t n) {
    if (linux_prot_watch_on()) prot_watch_release(addr, n);
}

int plat_mem_watch_info(uint64_t addr) {
    return linux_prot_watch_on() ? prot_watch_page_info(addr) : 0;
}
void plat_mem_gpu_import(uint64_t addr, size_t n, int on) { (void)addr; (void)n; (void)on; }

uint64_t plat_mem_dmem_avail(void) { return UINT64_MAX; }
uint64_t plat_mem_dmem_free_total(void) { return UINT64_MAX; }

uint64_t plat_mem_write_watch_faults(int reset) {
    return linux_prot_watch_on() ? prot_watch_faults(reset) : 0;
}

uint64_t plat_mem_write_watch_repeats(int reset) {
    return linux_prot_watch_on() ? prot_watch_repeat_faults(reset) : 0;
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

int plat_shadow_stack(void) { return -1; }

void plat_reexec(void) {
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    fflush(stdout);
    fflush(stderr);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "FATAL: re-running the process failed (%s)\n", strerror(errno));
    _exit(70);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    uint8_t b;
    uint64_t end = addr + (n ? n : 1);
    for (uint64_t a = addr; a < end; a = (a | 4095) + 1)
        if (!plat_mem_read(a, &b, 1)) return 0;
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// Fresh read-write anonymous pages over [addr, addr + n) of a reservation (a placed map undone, vk_memory.c), back
// under the write watch like the pages they replace.
int plat_mem_replace_anon(uint64_t addr, uint64_t n) {
    void *p = mmap((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                   -1, 0);
    if (p != (void *)(uintptr_t)addr) return 0;
    madvise(p, n, MADV_NOHUGEPAGE);
    struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
    if (ww_fd >= 0) ioctl(ww_fd, UFFDIO_REGISTER, &reg);
    ps_set(addr, n, PS_RW);
    return 1;
}
