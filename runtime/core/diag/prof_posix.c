// Sampling profiler of Linux and the PS5 (MKW_PROFILE=1): every MKW_PROFILE_US (default 1000) the sampler thread of
// platform/sigsample.c sends SIGPROF to each registered thread, and the handler counts (thread, instruction pointer)
// in a lock-free table. Every 600 frames, and at exit, the log gets each thread name's top functions over the window:
//   prof: frame 600: 41.2 CPU s sampled over 600 frames (0.0 ms dropped), every 1000 us per thread
//   prof:   vk replay (replay): 16.30 CPU ms/frame, 25% outside the executable
//   prof:     #1   9.1%   1.50 ms/f  host prepare_draw_ex
//   prof:     #2   4.0%   0.65 ms/f  main+0x19e5860 (sym+0x40)   <- lifted, named in the guest (tools/analisis/bin/q.py)
//   prof:     #3   3.0%   0.50 ms/f  outside: libc.so.6 __memmove_avx_unaligned_erms
// Each sample weighs the thread's CPU time since its previous one (sigsample.c), so the figures are CPU time, and one
// under MKW_PROFILE_MIN_US is dropped as a wait: the CPU of a thread busy less than a tenth of the time is not
// counted. Percentages are of the thread name's counted CPU time.
// Threads of one name (the guest's "guest" threads) are summed. "outside the executable" is system code: on the PS5
// mostly libkernel's waits (0x8_0000_0000 up), so a thread that mostly waits shows mostly that.
// Names: lifted functions through the dispatch table (rt_linked_modules: host function -> guest start, ldr_describe);
// host functions from the executable's symbols: Linux reads /proc/self/exe's .symtab; the PS5's eboot has none, so
// tools/ps5/build_game.py writes mkw.sym (llvm-nm of llvm-pie.elf, host functions only) next to it, read from
// /app0/mkw.sym or MKW_PROFILE_SYMS. Without it a host address shows as exe+0x... (tools/run/sym.py names it with --ps5).
// Why not ITIMER_PROF: on the PS5 it signals the process, not the running threads (run 16: 12 samples in 600 frames).
#if defined(__linux__) || defined(__PROSPERO__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "prof.h"
#include "platform/platform.h"

#define PROF_SLOTS (1u << 15)
#define REG_SLOTS 512

typedef struct {
    uint64_t key;     // hash of (slot, rip, caller), 0 empty, 1 initializing
    uint64_t rip;     // sampled instruction pointer (256 B bucket when caller != 0)
    uint64_t caller;  // return address in executable when rip is outside it, else 0
    uint32_t slot;    // registry slot
    uint64_t n;       // CPU us of the samples there (each weighted by its thread's CPU time since its previous sample)
    uint64_t c;       // samples there, every one (the running profile: samples x period)
} ProfSlot;

static ProfSlot table[PROF_SLOTS];
static uint64_t prev_n[PROF_SLOTS], prev_c[PROF_SLOTS];   // counts at the last report: each report covers its window
static volatile uint64_t total_samples, dropped_samples;
static uint64_t prev_total, prev_dropped, last_report_frame, prev_frame, cur_frame;
static volatile int active;
static uint32_t slot_tid[REG_SLOTS];   // the tid each registry slot had when sampled (role)
static uint32_t period_us = 1000;
// A sample counts only when its thread used at least min_us of CPU since its previous one (MKW_PROFILE_MIN_US, default a
// tenth of the period). Waking a blocked thread to run the handler costs CPU too, about 12 us a sample under WSL
// (t20-linux4: futex waits still 91% of the guest threads' weight), so without the floor every wait counted.
static uint32_t min_us = 100;

static uint64_t mix64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static void on_sample(uint32_t tid, int slot, uint64_t rip, uint64_t caller, uint64_t cpu_ns) {
    if (!__atomic_load_n(&active, __ATOMIC_RELAXED) || !rip || slot < 0 || slot >= REG_SLOTS) return;
    uint64_t w = cpu_ns / 1000;   // us
    if (w < min_us) w = 0;   // blocked (or nearly) since the previous sample: counted, but no CPU weight
    slot_tid[slot] = tid;
    if (caller) rip &= ~0xffull;
    uint64_t h = mix64(rip ^ mix64(caller ^ ((uint64_t)(slot + 1) << 48)));
    if (h <= 1) h += 2;
    uint32_t i = (uint32_t)h & (PROF_SLOTS - 1);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        uint64_t cur = __atomic_load_n(&table[i].key, __ATOMIC_ACQUIRE);
        if (cur == 0) {
            uint64_t expected = 0;
            if (__atomic_compare_exchange_n(&table[i].key, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                table[i].rip = rip;
                table[i].caller = caller;
                table[i].slot = (uint32_t)slot;
                __atomic_store_n(&table[i].key, h, __ATOMIC_RELEASE);
                cur = h;
            } else {
                cur = expected;
            }
        }
        while (cur == 1) cur = __atomic_load_n(&table[i].key, __ATOMIC_ACQUIRE);
        if (cur == h && table[i].rip == rip && table[i].caller == caller && table[i].slot == (uint32_t)slot) {
            if (w) __atomic_fetch_add(&table[i].n, w, __ATOMIC_RELAXED);
            __atomic_fetch_add(&table[i].c, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&total_samples, w, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_fetch_add(&dropped_samples, w, __ATOMIC_RELAXED);
}

// ---- names

typedef struct { uint64_t at; const char *name; } Sym;
static Sym *syms;           // host functions, sorted by address
static size_t nsyms;
static struct { uint64_t host, guest; } *lifted;   // lifted functions, sorted by host address
static size_t nlifted;
static uint64_t lifted_lo, lifted_hi;
static int names_ready;

static int by_at(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int is_lifted_name(const char *s) {
    if (s[0] != 'f' || s[1] != '_' || !s[2]) return 0;
    for (s += 2; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

static void sym_add(uint64_t at, const char *name, size_t *cap) {
    if (is_lifted_name(name) || !(name = strdup(name))) return;
    if (nsyms == *cap) {
        *cap = *cap ? *cap * 2 : 4096;
        Sym *g = realloc(syms, *cap * sizeof *syms);
        if (!g) return;
        syms = g;
    }
    syms[nsyms++] = (Sym){at, name};
}

#ifdef __linux__
#include <dlfcn.h>
#include <elf.h>
// The function symbols of /proc/self/exe's .symtab (build/mkw keeps it).
static void load_host_syms(void) {
    size_t len = 0;
    uint8_t *f = NULL;
    FILE *fp = fopen("/proc/self/exe", "rb");
    if (!fp) return;
    if (!fseek(fp, 0, SEEK_END)) {
        long l = ftell(fp);
        if (l > 0 && (f = malloc((size_t)l)) && !fseek(fp, 0, SEEK_SET) && fread(f, 1, (size_t)l, fp) == (size_t)l)
            len = (size_t)l;
    }
    fclose(fp);
    if (!len || len < sizeof(Elf64_Ehdr) || memcmp(f, ELFMAG, SELFMAG)) { free(f); return; }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)f;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > len) { free(f); return; }
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(f + eh->e_shoff);
    uint64_t base = eh->e_type == ET_DYN ? plat_exe_base() : 0;
    size_t cap = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum) continue;
        const Elf64_Shdr *st = &sh[sh[i].sh_link];
        if (sh[i].sh_offset + sh[i].sh_size > len || st->sh_offset + st->sh_size > len) continue;
        const Elf64_Sym *s = (const Elf64_Sym *)(f + sh[i].sh_offset);
        const char *str = (const char *)(f + st->sh_offset);
        for (size_t k = 0; k < sh[i].sh_size / sizeof *s; k++)
            if (ELF64_ST_TYPE(s[k].st_info) == STT_FUNC && s[k].st_value && s[k].st_name < st->sh_size)
                sym_add(base + s[k].st_value, str + s[k].st_name, &cap);
    }
    free(f);   // the names were copied (sym_add)
}
#else
// mkw.sym: "<hex address in llvm-pie.elf> <name>" per line (tools/ps5/build_game.py).
static void load_host_syms(void) {
    const char *path = cfg_get("MKW_PROFILE_SYMS");
    FILE *fp = fopen(path && *path ? path : "/app0/mkw.sym", "r");
    if (!fp) return;
    char line[512];
    size_t cap = 0;
    uint64_t base = plat_exe_base();
    while (fgets(line, sizeof line, fp)) {
        char *end;
        uint64_t at = strtoull(line, &end, 16);
        if (end == line || *end != ' ') continue;
        end[strcspn(end, "\r\n")] = 0;
        sym_add(base + at, end + 1, &cap);
    }
    fclose(fp);
}
#endif

static int by_host(const void *a, const void *b) { return by_at(a, b); }

static void names_build(void *arg) {
    (void)arg;
    load_host_syms();
    if (nsyms) qsort(syms, nsyms, sizeof *syms, by_at);
    RtModule m[16];
    int nm = rt_linked_modules(m, 16);
    size_t n = 0;
    for (int i = 0; i < nm; i++) n += m[i].count;
    if (n && (lifted = malloc(n * sizeof *lifted))) {
        for (int i = 0; i < nm; i++)
            for (uint64_t k = 0; k < m[i].count; k++)
                lifted[nlifted].host = (uint64_t)(uintptr_t)m[i].fns[k], lifted[nlifted++].guest = m[i].addrs[k];
        qsort(lifted, nlifted, sizeof *lifted, by_host);
        lifted_lo = lifted[0].host;
        lifted_hi = lifted[nlifted - 1].host + 0x10000;   // the last one's size is not known
    }
    rt_log("prof: names: %zu host functions (%s), %zu lifted functions", nsyms,
#ifdef __linux__
           "/proc/self/exe",
#else
           "mkw.sym",
#endif
           nlifted);
    __atomic_store_n(&names_ready, 1, __ATOMIC_RELEASE);
}

static PlatOnce names_once;
static void names_init(void) { plat_once(&names_once, names_build, NULL); }
void prof_names_init(void) { names_init(); }


// What a sample's address is counted under: a function's start. *kind: 0 outside the executable, 1 host, 2 lifted.
static uint64_t func_of(uint64_t rip, int *kind) {
    if (nlifted && rip >= lifted_lo && rip < lifted_hi) {
        size_t lo = 0, hi = nlifted;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (lifted[mid].host <= rip) lo = mid + 1;
            else hi = mid;
        }
        // a host symbol between the lifted function and rip means rip is in host code
        if (lo) {
            uint64_t f = lifted[lo - 1].host;
            size_t a = 0, b = nsyms;
            while (a < b) {
                size_t mid = (a + b) / 2;
                if (syms[mid].at <= rip) a = mid + 1;
                else b = mid;
            }
            if (!a || syms[a - 1].at < f) {
                *kind = 2;
                return f;
            }
        }
    }
    size_t a = 0, b = nsyms;
    while (a < b) {
        size_t mid = (a + b) / 2;
        if (syms[mid].at <= rip) a = mid + 1;
        else b = mid;
    }
    // host code: a named function within 1 MiB, else the 4 KiB page (no symbols: the PS5 without mkw.sym)
    uint64_t base = plat_exe_base();
    if (a && rip - syms[a - 1].at < (1u << 20)) {
        *kind = 1;
        return syms[a - 1].at;
    }
    if (base && rip >= base && rip - base < (1ull << 32)) {
        *kind = 1;
        return rip & ~0xfffull;
    }
    // outside: a shared library's function (Linux) or the 256 B bucket around it (the PS5's system modules)
    *kind = 0;
#ifdef __linux__
    Dl_info di;
    if (dladdr((void *)(uintptr_t)rip, &di) && di.dli_saddr) return (uint64_t)(uintptr_t)di.dli_saddr;
#endif
    return rip & ~0xffull;
}

static void func_name(uint64_t f, int kind, char *buf, int n) {
    if (kind == 2) {
        size_t lo = 0, hi = nlifted;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (lifted[mid].host < f) lo = mid + 1;
            else hi = mid;
        }
        if (lo < nlifted && lifted[lo].host == f) {
            ldr_describe(lifted[lo].guest, buf, n);
            return;
        }
    }
    if (kind == 1) {
        size_t a = 0, b = nsyms;
        while (a < b) {
            size_t mid = (a + b) / 2;
            if (syms[mid].at < f) a = mid + 1;
            else b = mid;
        }
        if (a < nsyms && syms[a].at == f) snprintf(buf, n, "host %s", syms[a].name);
        else snprintf(buf, n, "host exe+%#llx (page)", (unsigned long long)(f - plat_exe_base()));
        return;
    }
#ifdef __linux__
    Dl_info di;
    if (dladdr((void *)(uintptr_t)f, &di) && di.dli_sname && di.dli_saddr == (void *)(uintptr_t)f) {
        const char *lib = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
        snprintf(buf, n, "outside: %s %s", lib ? lib + 1 : di.dli_fname ? di.dli_fname : "?", di.dli_sname);
        return;
    }
#endif
    snprintf(buf, n, "outside: %#llx (256 B)", (unsigned long long)f);
}

// Any thread, a signal handler too (memory_early.c's fault): reads the tables only, once built.
const char *prof_pc_describe(uint64_t pc, char *buf, int n) {
    if (!__atomic_load_n(&names_ready, __ATOMIC_ACQUIRE)) {
        snprintf(buf, (size_t)n, "exe+%#llx", (unsigned long long)(pc - plat_exe_base()));
        return buf;
    }
    int kind;
    uint64_t f = func_of(pc, &kind);
    if (kind == 0) {
        snprintf(buf, (size_t)n, "%#llx, outside the executable", (unsigned long long)pc);
        return buf;
    }
    char nm[160];
    func_name(f, kind, nm, sizeof nm);
    if (kind == 2) snprintf(buf, (size_t)n, "%s, exe+%#llx", nm, (unsigned long long)(pc - plat_exe_base()));
    else snprintf(buf, (size_t)n, "%s+%#llx", nm, (unsigned long long)(pc - f));
    return buf;
}

// ---- report

typedef struct { int group, kind, ckind; uint64_t f, cf, n, c; } Hit;

static int by_hit(const void *a, const void *b) {
    const Hit *x = a, *y = b;
    if (x->group != y->group) return x->group - y->group;
    if (x->kind != y->kind) return x->kind - y->kind;
    if (x->f != y->f) return x->f < y->f ? -1 : 1;
    if (x->ckind != y->ckind) return x->ckind - y->ckind;
    return x->cf < y->cf ? -1 : x->cf > y->cf;
}

// The running profile (15/11/2026): the CPU-weighted list puts a thread's work before a wait under the wait, and the
// game's job threads block ~75 times a frame each, so most of their CPU went to wait_cond. A sample that finds a thread
// in code (not blocked) lands there in proportion to the time spent there, so samples x period is that time; the
// samples that find the thread waiting are left out by their caller's name.
static int is_wait(const char *caller) {
    static const char *const w[] = {"wait_cond", "lock_at", "plat_mutex_lock", "plat_cond_wait", "plat_sleep",
                                    "plat_event_wait", "sem_wait", "pthread_cond", "pthread_mutex_lock", "nanosleep"};
    for (size_t i = 0; i < sizeof w / sizeof *w; i++)
        if (strstr(caller, w[i])) return 1;
    return 0;
}

static void running_top(int g, const char *gname, const Hit *hits, size_t m, uint64_t frames) {
    Hit top[16] = {{0}};
    uint64_t run = 0, all = 0;
    for (size_t i = 0; i < m; i++) {
        if (hits[i].group != g || !hits[i].c) continue;
        all += hits[i].c;
        if (!hits[i].kind) {   // outside the executable: a wait when its caller is one
            char cdesc[128] = "";
            if (hits[i].cf) func_name(hits[i].cf, hits[i].ckind, cdesc, sizeof cdesc);
            if (is_wait(cdesc)) continue;
        }
        run += hits[i].c;
        for (int j = 0; j < 16; j++)
            if (hits[i].c > top[j].c) {
                memmove(top + j + 1, top + j, (size_t)(15 - j) * sizeof *top);
                top[j] = hits[i];
                break;
            }
    }
    if (!run || !frames) return;
    rt_log("prof:   %s running: %.2f ms/frame (%llu of %llu samples not in a wait, x %u us)", gname,
           run * (double)period_us / 1e3 / frames, (unsigned long long)run, (unsigned long long)all, period_us);
    for (int j = 0; j < 16 && top[j].c; j++) {
        char desc[192], cdesc[128];
        func_name(top[j].f, top[j].kind, desc, sizeof desc);
        if (!top[j].kind && top[j].cf) {
            func_name(top[j].cf, top[j].ckind, cdesc, sizeof cdesc);
            rt_log("prof:     r%-2d %5.1f%% %6.2f ms/f  %s <- %s", j + 1, 100.0 * top[j].c / run,
                   top[j].c * (double)period_us / 1e3 / frames, desc, cdesc);
        } else {
            rt_log("prof:     r%-2d %5.1f%% %6.2f ms/f  %s", j + 1, 100.0 * top[j].c / run,
                   top[j].c * (double)period_us / 1e3 / frames, desc);
        }
    }
}

static void report(const char *tag, uint64_t frames) {
    uint64_t tot = __atomic_load_n(&total_samples, __ATOMIC_RELAXED), drp = __atomic_load_n(&dropped_samples, __ATOMIC_RELAXED);
    uint64_t wtot = tot - prev_total, wdrp = drp - prev_dropped;
    prev_total = tot, prev_dropped = drp;
    if (!wtot) return;
    names_init();
    enum { GROUPS = 64 };
    char gname[GROUPS][48];
    uint64_t gtot[GROUPS] = {0}, gout[GROUPS] = {0};
    int ng = 0, slot_group[REG_SLOTS];
    for (int i = 0; i < REG_SLOTS; i++) slot_group[i] = -1;
    Hit *hits = malloc(PROF_SLOTS * sizeof *hits);
    if (!hits) return;
    size_t nh = 0;
    for (uint32_t i = 0; i < PROF_SLOTS; i++) {
        uint64_t key = __atomic_load_n(&table[i].key, __ATOMIC_ACQUIRE);
        if (key <= 1) continue;
        uint64_t n = __atomic_load_n(&table[i].n, __ATOMIC_RELAXED), d = n - prev_n[i];
        uint64_t cn = __atomic_load_n(&table[i].c, __ATOMIC_RELAXED), dc = cn - prev_c[i];
        prev_n[i] = n, prev_c[i] = cn;
        if (!d && !dc) continue;
        int slot = (int)table[i].slot;
        if (slot < 0 || slot >= REG_SLOTS) continue;
        if (slot_group[slot] < 0) {   // the group of the slot's thread name (role in brackets)
            char name[48] = "?";
            plat_sample_slot_name(slot, name, sizeof name);
            uint32_t tid = slot_tid[slot];
            const char *role = tid == prof_main_tid ? " (guest main)" : tid == prof_render_tid ? " (render)"
                             : tid == prof_replay_tid ? " (replay)" : "";
            char full[48];
            snprintf(full, sizeof full, "%s%s", name, role);
            int g = 0;
            while (g < ng && strcmp(gname[g], full)) g++;
            if (g == ng && ng < GROUPS) snprintf(gname[ng++], sizeof gname[0], "%s", full);
            slot_group[slot] = g < GROUPS ? g : GROUPS - 1;
        }
        int g = slot_group[slot], kind, ckind = 0;
        uint64_t f = func_of(table[i].rip, &kind);
        uint64_t cf = (!kind && table[i].caller) ? func_of(table[i].caller, &ckind) : 0;
        gtot[g] += d;
        if (!kind) gout[g] += d;
        hits[nh++] = (Hit){g, kind, ckind, f, cf, d, dc};
    }
    qsort(hits, nh, sizeof *hits, by_hit);
    size_t m = 0;   // merge the same function (and caller when outside) of a group
    for (size_t i = 0; i < nh; i++) {
        if (m && hits[m - 1].group == hits[i].group && hits[m - 1].kind == hits[i].kind && hits[m - 1].f == hits[i].f &&
            hits[m - 1].ckind == hits[i].ckind && hits[m - 1].cf == hits[i].cf)
            hits[m - 1].n += hits[i].n, hits[m - 1].c += hits[i].c;
        else
            hits[m++] = hits[i];
    }
    rt_log("prof: %s: %.1f CPU s sampled over %llu frames (%.1f ms dropped), every %u us per thread", tag, wtot / 1e6,
           (unsigned long long)frames, wdrp / 1e3, period_us);
    for (int pass = 0; pass < ng; pass++) {   // groups by samples, most first
        int g = -1;
        for (int k = 0; k < ng; k++)
            if (gtot[k] && (g < 0 || gtot[k] > gtot[g])) g = k;
        if (g < 0) break;
        uint64_t in = gtot[g] - gout[g];
        rt_log("prof:   %s: %.2f CPU ms/frame, %.0f%% outside the executable", gname[g],
               frames ? gtot[g] / 1e3 / frames : 0.0, 100.0 * gout[g] / gtot[g]);
        (void)in;
        Hit top[12] = {{0}};
        for (size_t i = 0; i < m; i++) {
            if (hits[i].group != g) continue;
            for (int j = 0; j < 12; j++)
                if (hits[i].n > top[j].n) {
                    memmove(top + j + 1, top + j, (size_t)(11 - j) * sizeof *top);
                    top[j] = hits[i];
                    break;
                }
        }
        for (int j = 0; j < 12 && top[j].n; j++) {
            char desc[192], cdesc[128];
            func_name(top[j].f, top[j].kind, desc, sizeof desc);
            if (!top[j].kind && top[j].cf) {
                func_name(top[j].cf, top[j].ckind, cdesc, sizeof cdesc);
                rt_log("prof:     #%-2d %5.1f%% %6.2f ms/f  %s <- %s", j + 1, 100.0 * top[j].n / gtot[g],
                       frames ? top[j].n / 1e3 / frames : 0.0, desc, cdesc);
            } else {
                rt_log("prof:     #%-2d %5.1f%% %6.2f ms/f  %s", j + 1, 100.0 * top[j].n / gtot[g],
                       frames ? top[j].n / 1e3 / frames : 0.0, desc);
            }
        }
        running_top(g, gname[g], hits, m, frames);
        gtot[g] = 0;
    }
    free(hits);
}

static uint64_t from_frame;   // MKW_PROFILE_FROM
static int from_started;
static int sampler_go(void);

int prof_sampler_start(void) {
    const char *e = cfg_get("MKW_PROFILE");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    const char *us = cfg_get("MKW_PROFILE_US");
    if (us && atoi(us) >= 100) period_us = (uint32_t)atoi(us);
    const char *mu = cfg_get("MKW_PROFILE_MIN_US");
    min_us = mu ? (uint32_t)atoi(mu) : period_us / 10;
    // MKW_PROFILE_FROM=<frame>: no signal before that present. With the sampler on from the start, the PS5 title hung in
    // the race's intro movie twice (15/11/2026, at 1000 and 10000 us: every thread waiting, the movie never ending);
    // the same build without it raced. Starting after the movie profiles the race.
    const char *fr = cfg_get("MKW_PROFILE_FROM");
    from_frame = fr ? strtoull(fr, NULL, 10) : 0;
    if (from_frame) {
        rt_log("prof: MKW_PROFILE: sampling from frame %llu", (unsigned long long)from_frame);
        return 1;
    }
    return sampler_go();
}

static int sampler_go(void) {
    const char *only = cfg_get("MKW_PROFILE_ONLY");   // e.g. guest: the game's threads only (plat_sample_filter)
    if (only && *only) plat_sample_filter(only), rt_log("prof: MKW_PROFILE_ONLY: only threads named %s", only);
    __atomic_store_n(&active, 1, __ATOMIC_RELEASE);
    if (!plat_sample_start(period_us, on_sample)) {
        __atomic_store_n(&active, 0, __ATOMIC_RELEASE);
        rt_log("prof: MKW_PROFILE: the sampler did not start");
        return 0;
    }
    rt_log("prof: MKW_PROFILE: every registered thread sampled each %u us (SIGPROF from a sampler thread), a sample "
           "counted when its thread ran %u us or more since its previous one", period_us, min_us);
    return 1;
}

void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d) {
    (void)now, (void)d;
    if (from_frame && frame >= from_frame && !__atomic_exchange_n(&from_started, 1, __ATOMIC_ACQ_REL)) {
        prev_frame = last_report_frame = frame;   // the first window starts here
        sampler_go();
    }
    if (!__atomic_load_n(&active, __ATOMIC_RELAXED)) return;
    cur_frame = frame;
    if (frame && frame % 600 == 0 && frame != last_report_frame) {
        last_report_frame = frame;
        char tag[32];
        snprintf(tag, sizeof tag, "frame %llu", (unsigned long long)frame);
        report(tag, frame - prev_frame);
        prev_frame = frame;
    }
}

void prof_sampler_write(void) {
    if (!__atomic_exchange_n(&active, 0, __ATOMIC_ACQ_REL)) return;
    plat_sample_stop();
    report("exit", cur_frame > prev_frame ? cur_frame - prev_frame : 0);
}
#endif
