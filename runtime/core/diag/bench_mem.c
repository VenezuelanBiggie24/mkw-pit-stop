// Memory report of the benchmark: what a long run does to memory, so a leak or a squeeze shows as numbers instead of
// a crash an hour in. MKW_BENCH=ko (a Knockout Tour: one race after another over the open world with no menu between,
// rally after rally) is made for it; tt and gp report it too.
//
// - A thread of its own samples once a second from the window's start (bench.c): nothing is added to the game,
//   replay or present threads (AGENTS.md). It reads only what is lock-free: plat_mem_pressure (PS5: free direct memory
//   over all its areas, on a scan buffer of its own so the governor's scan never finds it busy, and the largest area;
//   the guest's backed memory; free flexible memory; the page-table pools), plat_process_memory/plat_mem_info (Linux
//   and Windows: the process's resident and private memory, the machine's available memory), vk_mem_snap (the census
//   snapshot vk_mem_census refreshes every 15 frames: driver heap usage, VMA blocks, live and released textures,
//   sparse, shadows) and vk_mem_pressure (governor passes by level, bytes they freed, out-of-memory reliefs, deferred
//   staging uploads).
// - "bench: mem t=..." every BENCH_MEM_LOG_S: a run that dies of memory leaves its trend in the log
//   (tools/run/bench_mem.py turns those lines into a table, with or without the final report).
// - The report, per window and segment (bench.c: the rallies of ko, the save-split segments of tt and gp): each
//   measure's value at the start and end, its minimum or maximum, and its trend in MiB/min by least squares over the
//   samples (a leak is a steady rise of resident memory or fall of free memory across segments, where a squeeze is a
//   low minimum); the governor's hard passes, reliefs and deferred uploads within it. The whole window's numbers
//   again as "bench: memresult k=v ..." for tools/ps5/bench.py.
#include <stdio.h>
#include <string.h>

#include "bench_mem.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define BENCH_MEM_SAMPLES 14400   // 4 h at one a second (MKW_BENCH_MAX's ceiling)
#define BENCH_MEM_LOG_S 30
#define MIB (1.0 / 1048576.0)

// Every measure in MiB, -1 when the platform has none. free: PS5 free direct memory (all areas), elsewhere the
// machine's available memory; resident: PS5 the guest's backed memory, elsewhere the process's resident memory.
enum { M_FREE, M_LARGEST, M_RESIDENT, M_PRIVATE, M_FLEX, M_CHEAP, M_VRAM, M_VMA, M_TEX, M_TEXREL, M_SPARSE, M_SHADOW, M_PTCPU,
       M_PTGPU, M_N };
static const struct { const char *name; int low_is_bad; } measure[M_N] = {
    {"free", 1},  {"largest", 1}, {"resident", 0}, {"private", 0}, {"flex_free", 1}, {"c_heap", 0}, {"vram", 0},     {"vma", 0},
    {"tex", 0},   {"tex_rel", 0}, {"sparse", 0},   {"shadow", 0},  {"pt_cpu_free", 1}, {"pt_gpu_free", 1},
};
typedef struct {
    uint64_t ns;
    float v[M_N];
    uint32_t gov_soft, gov_hard, relieves, stg_deferred;   // since start
    float gov_freed;                                       // MiB, since start
} Sample;

static Sample samples[BENCH_MEM_SAMPLES];
static volatile uint32_t nsamples;
static volatile int running;
static uint64_t t0;
static int32_t pt_cpu_total, pt_gpu_total;
static PlatThread *thread;

static float mib(int64_t b) { return b < 0 ? -1.0f : (float)(b * MIB); }

static void sample(Sample *s) {
    memset(s, 0, sizeof *s);
    s->ns = kern_now_ns();
    for (int m = 0; m < M_N; m++) s->v[m] = -1.0f;
    PlatMemPressure p;
    plat_mem_pressure(&p);
    if (p.dmem_free >= 0 || p.guest_backed >= 0) {   // the PS5
        s->v[M_FREE] = mib(p.dmem_free), s->v[M_LARGEST] = mib(p.dmem_largest), s->v[M_RESIDENT] = mib(p.guest_backed);
        s->v[M_FLEX] = mib(p.flex_free);
        s->v[M_PTCPU] = (float)p.pt_cpu_free, s->v[M_PTGPU] = (float)p.pt_gpu_free;
        pt_cpu_total = p.pt_cpu_total, pt_gpu_total = p.pt_gpu_total;
    } else {
        uint64_t res = 0, priv = 0;
        float used = -1, total = -1, proc = -1;
        plat_process_memory(&res, &priv);
        plat_mem_info(&used, &total, &proc);
        if (res) s->v[M_RESIDENT] = (float)(res * MIB);
        if (priv) s->v[M_PRIVATE] = (float)(priv * MIB);
        if (used >= 0 && total > 0) s->v[M_FREE] = total - used;
    }
    // The runtime's own C heap: on the PS5 the SDK's title heap, direct memory it has mapped (ps5_sys.c); elsewhere
    // what the allocator reports (mapped, else in use). A leak of the runtime's own allocations shows here.
    uint64_t h_use = 0, h_free = 0, h_map = 0;
    plat_heap_stats(&h_use, &h_free, &h_map);
    if (h_map || h_use) s->v[M_CHEAP] = (float)((h_map ? h_map : h_use) * MIB);
    VkMemSnap vs;
    vk_mem_snap(&vs);
    s->v[M_VRAM] = vs.drv_usage ? (float)(vs.drv_usage * MIB) : -1.0f;   // 0: no VK_EXT_memory_budget
    s->v[M_VMA] = (float)(vs.vma_block * MIB), s->v[M_TEX] = (float)(vs.tex_live * MIB);
    s->v[M_TEXREL] = (float)(vs.tex_released * MIB), s->v[M_SPARSE] = (float)(vs.sparse * MIB);
    s->v[M_SHADOW] = (float)(vs.shadow * MIB);
    VkMemPressure vp;
    vk_mem_pressure(&vp);
    s->gov_soft = (uint32_t)vp.gov_soft, s->gov_hard = (uint32_t)vp.gov_hard, s->relieves = (uint32_t)vp.relieves;
    s->stg_deferred = (uint32_t)vp.stg_deferred, s->gov_freed = (float)(vp.gov_freed * MIB);
}

static void log_sample(const Sample *s) {
    char line[512];
    int o = snprintf(line, sizeof line, "bench: mem t=%.0f", (s->ns - t0) / 1e9);
    for (int m = 0; m < M_N && o < (int)sizeof line; m++)
        if (s->v[m] >= 0) o += snprintf(line + o, sizeof line - o, " %s=%.0f", measure[m].name, s->v[m]);
    if (o < (int)sizeof line)
        snprintf(line + o, sizeof line - o, " gov_hard=%u gov_soft=%u gov_freed=%.0f relieves=%u stg_deferred=%u",
                 s->gov_hard, s->gov_soft, s->gov_freed, s->relieves, s->stg_deferred);
    rt_log("%s", line);
}

static void sampler(void *arg) {
    (void)arg;
    uint64_t next_log = 0;
    while (__atomic_load_n(&running, __ATOMIC_ACQUIRE)) {
        uint32_t n = nsamples;
        if (n < BENCH_MEM_SAMPLES) {
            sample(&samples[n]);
            __atomic_store_n(&nsamples, n + 1, __ATOMIC_RELEASE);
            if (samples[n].ns >= next_log) {
                log_sample(&samples[n]);
                next_log = samples[n].ns + BENCH_MEM_LOG_S * 1000000000ull;
            }
        }
        plat_sleep_ms(1000);
    }
}

void bench_mem_start(uint64_t t0_ns) {
    if (thread) return;
    t0 = t0_ns;
    __atomic_store_n(&running, 1, __ATOMIC_RELEASE);
    if (!(thread = plat_thread_new(sampler, NULL, "bench-mem"))) {
        rt_log("bench: no memory sampler thread: no memory report");
        running = 0;
        return;
    }
    plat_thread_set_priority(thread, PLAT_PRIO_BELOW_NORMAL);
    rt_log("bench: memory sampled every second (bench-mem thread), a line every %d s", BENCH_MEM_LOG_S);
}

void bench_mem_stop(void) {
    if (!thread) return;
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    plat_thread_join_ns(thread, 1500000000ull);   // a sample takes well under a second; the report does not wait on it
}

// Least-squares slope of measure m over samples [a, b), in MiB per minute; 0 with fewer than 2 valid samples.
static double slope(int m, uint32_t a, uint32_t b) {
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint32_t i = a; i < b; i++) {
        if (samples[i].v[m] < 0) continue;
        double x = (samples[i].ns - t0) / 60e9, y = samples[i].v[m];
        n++, sx += x, sy += y, sxx += x * x, sxy += x * y;
    }
    double d = n * sxx - sx * sx;
    return n >= 2 && d > 0 ? (n * sxy - sx * sy) / d : 0;
}

void bench_mem_report(const char *label, uint64_t a_ns, uint64_t b_ns, int result) {
    uint32_t n = __atomic_load_n(&nsamples, __ATOMIC_ACQUIRE), a = 0, b;
    while (a < n && samples[a].ns < a_ns) a++;
    for (b = a; b < n && samples[b].ns < b_ns; b++) {}
    if (b - a < 2) {
        rt_log("bench: mem %s%u samples: no memory statistics", label, b - a);
        return;
    }
    const Sample *f = &samples[a], *l = &samples[b - 1];
    rt_log("bench: mem %s%u samples over %.0f s (MiB; start -> end, worst, trend per minute):", label, b - a,
           (l->ns - f->ns) / 1e9);
    char res[1600];   // the memresult line, under rt_log's 2048 bytes
    int o = 0;
    for (int m = 0; m < M_N; m++) {
        float worst = -1;
        double worst_t = 0;
        int any = 0;
        for (uint32_t i = a; i < b; i++) {
            float v = samples[i].v[m];
            if (v < 0) continue;
            if (!any || (measure[m].low_is_bad ? v < worst : v > worst)) worst = v, worst_t = (samples[i].ns - t0) / 1e9;
            any = 1;
        }
        if (!any) continue;
        double tr = slope(m, a, b);
        const char *unit = m == M_PTCPU || m == M_PTGPU ? " units" : "";
        int32_t tot = m == M_PTCPU ? pt_cpu_total : m == M_PTGPU ? pt_gpu_total : 0;
        char of[32] = "";
        if (tot > 0) snprintf(of, sizeof of, " of %d", tot);
        rt_log("bench: mem %s  %-11s %8.0f -> %8.0f, %s %8.0f%s%s at %.0f s, %+.1f/min", label, measure[m].name, f->v[m],
               l->v[m], measure[m].low_is_bad ? "min" : "max", worst, unit, of, worst_t, tr);
        if (result && o < (int)sizeof res)
            o += snprintf(res + o, sizeof res - o, " %s_start=%.0f %s_end=%.0f %s_%s=%.0f %s_trend=%.1f", measure[m].name,
                          f->v[m], measure[m].name, l->v[m], measure[m].name, measure[m].low_is_bad ? "min" : "max",
                          worst, measure[m].name, tr);
    }
    uint32_t hard = l->gov_hard - f->gov_hard, soft = l->gov_soft - f->gov_soft, rel = l->relieves - f->relieves,
             def = l->stg_deferred - f->stg_deferred;
    rt_log("bench: mem %s  governor %u hard and %u soft passes freeing %.0f MiB, %u out-of-memory reliefs, %u staging "
           "uploads deferred", label, hard, soft, l->gov_freed - f->gov_freed, rel, def);
    if (result) {
        if (o > (int)sizeof res - 1) o = (int)sizeof res - 1;
        rt_log("bench: memresult samples=%u secs=%.0f gov_hard=%u gov_soft=%u gov_freed=%.0f relieves=%u "
               "stg_deferred=%u%s", b - a, (l->ns - f->ns) / 1e9, hard, soft, l->gov_freed - f->gov_freed, rel, def,
               o ? res : "");
    }
}
