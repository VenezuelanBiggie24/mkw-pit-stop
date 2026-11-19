// Part of the kernel (kernel.c; split from it 06/11/2026, same sources and licences as its header).
// MKW_CRIT, the MKW_TRACE=sched accounting and report, and MKW_CORE_DIAG: diagnostics of the scheduler.
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gamelib.h"
#include "host.h"
#include "kernel_int.h"
#include "platform/platform.h"


// ---- MKW_CRIT=<first>-<last frame> with MKW_TRACE=sched: every state change and wake of the guest threads and
// every present over those frames, written to MKW_CRIT_OUT (default crit.bin) at the last one, for
// tools/crit_path.py, which walks the chain of waits back from each present (holding G). MKW_CRIT=w<lo>-<hi> takes
// world-clock seconds instead (frame_step_world_us): from the first present at or after lo to the first after hi, so a
#define CRIT_N (1u << 22)                // 128 MiB, about 2000 fight frames
static CritEv *g_crit;
static uint32_t g_crit_n;
int g_crit_on;
static uint64_t g_crit_lo, g_crit_hi;   // frames; UINT64_MAX in world mode until the world clock fixes them
static int64_t g_crit_wlo, g_crit_whi;  // world mode: the window in microseconds of world clock, else 0

// Under G or an address queue's lock (kernel.c addr_queue): the slot is claimed atomically.
void crit_add(int kind, uint32_t a, uint32_t b, int st, uint64_t obj, uint64_t ts) {
    uint32_t i = __atomic_fetch_add(&g_crit_n, 1, __ATOMIC_RELAXED);
    if (i < CRIT_N) g_crit[i] = (CritEv){ts, obj, a, b, (uint16_t)kind, (uint16_t)st, 0};
    else __atomic_store_n(&g_crit_n, CRIT_N, __ATOMIC_RELAXED);
}

// ---- MKW_TRACE=sched accounting (holding G)
#define RW_N 32
static RtThread *g_render;                               // the thread that presents (sched trace, core diag)
typedef struct { int st; uint32_t waker; uint64_t obj, ns; } WaitRec;
static WaitRec g_rw[RW_N], g_mw[RW_N];                   // the render and main thread's waits by object and waker
uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;  // preempt requests; releases after one and their latency
uint64_t g_sync_calls, g_sync_blocks;              // WaitSynchronization calls and the blocks inside them
uint64_t g_cw_ns[DG_N], g_cw_n[DG_N];              // core wait duration and count per DG_* class (MKW_TRACE=sched)

static void ho_accum(struct AddrHoRec *s, uint64_t host_ns, uint64_t glock_ns, uint64_t cwait_ns,
                     uint64_t cgrant_ns, uint64_t post_ns, uint64_t tot_ns, int kind, int timed_out) {
    if (timed_out) {
        __atomic_fetch_add(&s->to_n, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->host_ns, host_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->glock_ns, glock_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->cwait_ns, cwait_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->cgrant_ns, cgrant_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->post_ns, post_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->tot_ns, tot_ns, __ATOMIC_RELAXED);
    if ((unsigned)kind < HO_KIND_N) __atomic_fetch_add(&s->k[kind], 1, __ATOMIC_RELAXED);
    uint64_t mx = __atomic_load_n(&s->max_ns, __ATOMIC_RELAXED);
    while (tot_ns > mx &&
           !__atomic_compare_exchange_n(&s->max_ns, &mx, tot_ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

void ho_note_addr(RtThread *t, uint64_t addr, uint32_t waker, uint64_t host_ns, uint64_t glock_ns,
                  uint64_t cwait_ns, uint64_t cgrant_ns, uint64_t post_ns, uint64_t tot_ns,
                  int kind, int timed_out) {
    ho_accum(&t->ho_tot, host_ns, glock_ns, cwait_ns, cgrant_ns, post_ns, tot_ns, kind, timed_out);
    struct AddrHoRec *slot = NULL;
    for (int i = 0; i < HO_SLOTS; i++) {
        uint64_t sa = __atomic_load_n(&t->ho_slots[i].addr, __ATOMIC_ACQUIRE);
        if (!sa) {
            __atomic_store_n(&t->ho_slots[i].waker, waker, __ATOMIC_RELAXED);
            __atomic_store_n(&t->ho_slots[i].addr, addr, __ATOMIC_RELEASE);
            slot = &t->ho_slots[i];
            break;
        }
        if (sa == addr && (!waker || !t->ho_slots[i].waker || t->ho_slots[i].waker == waker)) {
            if (waker && !t->ho_slots[i].waker)
                __atomic_store_n(&t->ho_slots[i].waker, waker, __ATOMIC_RELAXED);
            slot = &t->ho_slots[i];
            break;
        }
    }
    if (!slot) slot = &t->ho_slots[HO_SLOTS - 1];
    ho_accum(slot, host_ns, glock_ns, cwait_ns, cgrant_ns, post_ns, tot_ns, kind, timed_out);
}

static void wait_add(WaitRec *w, int st, uint64_t obj, uint32_t waker, uint64_t ns) {
    for (int i = 0; i < RW_N; i++)
        if (!w[i].ns || (w[i].st == st && w[i].obj == obj && w[i].waker == waker)) {
            w[i].st = st;
            w[i].obj = obj;
            w[i].waker = waker;
            w[i].ns += ns;
            return;
        }
}

// Close the current state's interval and enter st.
void acct(RtThread *t, int st) {
    if (!(rt_trace & RT_TRACE_SCHED)) return;
    uint64_t now = kern_now_ns();
    if (t->st_since) {
        t->st_ns[t->st] += now - t->st_since;
        if (t->st >= S_HOST && (t == g_render || t->is_main)) {
            uint32_t waker = t->st == S_HOST || t->st == S_SLEEP ? 0 : t->waker;
            wait_add(t == g_render ? g_rw : g_mw, t->st, t->wobj, waker, now - t->st_since);
        }
    }
    if (g_crit_on) crit_add(CRIT_STATE, t->handle, t->waker, st, t->wobj, now);
    t->st = st;
    t->st_since = now;
}

// ---- MKW_CORE_DIAG=1: why ready threads wait for a virtual core, and for how long
// Each core wait is split into classes: the owner asked to give the core up ran game code (DG_GAME, the part an
// asynchronous preemption would remove), was in a host call or SVC that kept the core (DG_HOST), every owner the waiter
// may displace was of better or equal priority (DG_EQ: Horizon waits too), or the worse owners were claimed by better
// waiters (DG_NOCORE). Per 60 presents: time per frame and a wait-length histogram per class, the owners that cost
// most; a line per present over 25 ms; a line per wait with over 2 ms of DG_GAME (where the owner reached its
// checkpoint), and one host RIP sample of the owner per such wait (exe RVA, for mkw.map).
static const char *const dg_name[DG_N] = {"game", "host", "eq", "nocore"};
#define DG_NB 8   // wait lengths: < 0.05, 0.25, 1, 2, 4, 8, 16 ms, longer
static const uint64_t dg_edge[DG_NB - 1] = {50000, 250000, 1000000, 2000000, 4000000, 8000000, 16000000};
#define DG_LONG_LINES 400
#define DG_SAMPLES 600
#define DG_OWN 32
int kern_host_track;
int g_diag = -1;
typedef struct {
    uint64_t ns[DG_N], n[DG_N], hist[DG_N][DG_NB];                    // this window
    struct { uint32_t h; int prio, cls; uint64_t ns; } own[DG_OWN];   // this window: owners by class
    uint64_t long_frames, long_game_frames;
} DgWindow;
static DgWindow g_dgw;
static uint64_t g_dgf[DG_N], g_dgfr[DG_N];   // this frame: all threads, the render thread
uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;

void diag_init(void) {
    if (g_diag >= 0) return;
    const char *e = cfg_get("MKW_CORE_DIAG");
    g_diag = e && *e && strcmp(e, "0");
    if (g_diag) {
        kern_host_track = 1;
        rt_log("core-diag: on (MKW_CORE_DIAG)");
    }
}

// A tracked host call or SVC returned to depth 0 with a preempt request pending (host.h); on the thread itself.
void kern_host_left(void *thread) {
    RtThread *t = thread;
    if (g_diag > 0 && t->dg_kick && t->dg_host_kick && !t->dg_host_end) t->dg_host_end = kern_now_ns();
}

int dg_in_host(const RtThread *o) { return __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0; }

static void dg_add(const RtThread *w, int cls, uint64_t ns, uint32_t oh, int oprio) {
    if (!ns) return;
    g_dgw.ns[cls] += ns;
    g_dgf[cls] += ns;
    if (w == g_render) g_dgfr[cls] += ns;
    if (!oh) return;
    int i = 0;
    for (; i < DG_OWN && g_dgw.own[i].ns; i++)
        if (g_dgw.own[i].h == oh && g_dgw.own[i].cls == cls) break;
    if (i == DG_OWN) return;
    g_dgw.own[i].h = oh;
    g_dgw.own[i].prio = oprio;
    g_dgw.own[i].cls = cls;
    g_dgw.own[i].ns += ns;
}

// The waiter's class at the start of its wait, after core_kick claimed (or did not claim) an owner for it.
int dg_class0(const RtThread *t) {
    if (t->target) return dg_in_host(t->target) ? DG_HOST : DG_GAME;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) return DG_NOCORE;
    return DG_EQ;
}

// The waiter got a core: split [dg_since, now] by what the owner that released it did after its preempt request.
void dg_done(RtThread *t) {
    uint64_t now = kern_now_ns(), t0 = t->dg_since, w = now - t0;
    t->dg_since = 0;
    if (!t0) return;
    const struct DgRel *r = &t->dg_rel;
    uint64_t part[DG_N] = {0};
    uint32_t oh = t->target ? t->target->handle : r->h;
    int oprio = t->target ? t->target->prio : r->prio;
    if (r->pre && r->kick) {
        uint64_t k = r->kick > t0 ? (r->kick < now ? r->kick : now) : t0;
        part[t->dg_cls] += k - t0;
        if (r->host) {
            uint64_t he = r->host_end > k && r->host_end < now ? r->host_end : now;
            part[DG_HOST] += he - k;
            part[DG_GAME] += now - he;
        } else
            part[DG_GAME] += now - k;
        oh = r->h;
        oprio = r->prio;
    } else
        part[t->dg_cls] += w;
    int top = 0;
    for (int c = 0; c < DG_N; c++) {
        dg_add(t, c, part[c], oh, oprio);
        if (part[c] > part[top]) top = c;
    }
    int b = 0;
    while (b < DG_NB - 1 && w >= dg_edge[b]) b++;
    g_dgw.n[top]++;
    g_dgw.hist[top][b]++;
    if ((part[DG_GAME] >= 2000000 || part[DG_HOST] >= 2000000) && g_dg_lines < DG_LONG_LINES) {
        g_dg_lines++;
        char a[128], l[128];
        rt_log("core-diag: wait %.2f ms (game %.2f host %.2f): %#x p%d mask %llx; released by %#x p%d%s at %s lr %s",
               w / 1e6, part[DG_GAME] / 1e6, part[DG_HOST] / 1e6, t->handle, t->prio, (unsigned long long)t->mask, r->h,
               r->prio, r->pre ? " (preempted)" : "", r->pc ? ldr_describe(r->pc, a, sizeof a) : "-",
               r->lr ? ldr_describe(r->lr, l, sizeof l) : "-");
    }
    t->target = NULL;
    memset(&t->dg_rel, 0, sizeof t->dg_rel);
}

// Once per long DG_GAME or DG_HOST wait: where the owner's host thread runs (module and offset), its latest SVC and
// its guest lr. Holding G; the owner is resumed before anything that could take a lock it holds.
void dg_sample(RtThread *t) {
    RtThread *o = t->target;
    if (!o || o->cur_core < 0 || !o->hthr || g_dg_samples >= DG_SAMPLES) return;
    uint64_t rip, rsp;
    if (!plat_intr_stop(o->hthr, &rip, &rsp)) return;
    int depth = __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED);
    plat_intr_resume(o->hthr, 0);
    g_dg_samples++;
    char mn[256] = "?";
    uintptr_t m = 0;
    plat_module_of((uintptr_t)rip, mn, sizeof mn, &m);
    char lr[128];
    rt_log("core-diag: sample owner %#x p%d depth %d rip %s+%#llx, last svc %#x, guest lr %s (waiter %#x p%d, %s, "
           "%.2f ms)", o->handle, o->prio, depth, mn, (unsigned long long)(rip - m), o->dg_svc,
           o->cur ? ldr_describe(o->cur->x[30], lr, sizeof lr) : "-", t->handle, t->prio,
           t->dg_cls == DG_HOST ? "host" : "game", (kern_now_ns() - t->dg_since) / 1e6);
}


// perf_present, every present: a line for a present over 25 ms, the 60-present summary.
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns) {
    if (g_ap > 0 && frame && frame % 600 == 0) {   // MKW_ASYNC_PREEMPT counters since the start
        lock();
        uint64_t v[8] = {g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok, g_sync36};
        unlock();
        rt_log("kernel: frame %llu: asynchronous preemption: %llu interrupted, %llu cores given up there; refused: "
               "%llu in a host call, %llu outside game code, %llu preemption disabled, %llu failed; %llu waits ended "
               "in the first spin; SVC 0x36 %llu",
               (unsigned long long)frame, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
               (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
               (unsigned long long)v[7]);
    }
    if (g_diag <= 0) return;
    DgWindow w;
    uint64_t f[DG_N], fr[DG_N], s36, s36f;
    lock();
    g_render = tcur;
    memcpy(f, g_dgf, sizeof f);
    memcpy(fr, g_dgfr, sizeof fr);
    memset(g_dgf, 0, sizeof g_dgf);
    memset(g_dgfr, 0, sizeof g_dgfr);
    int lng = interval_ns > 25000000ull;
    if (lng) {
        g_dgw.long_frames++;
        if (f[DG_GAME] > 1000000) g_dgw.long_game_frames++;
    }
    int rep = frame && frame % 60 == 0;
    if (rep) {
        w = g_dgw;
        memset(&g_dgw, 0, sizeof g_dgw);
    }
    s36 = g_sync36;
    s36f = g_sync36_flag;
    unlock();
    if (lng)
        rt_log("core-diag: frame %llu: %.1f ms; core wait game %.2f host %.2f eq %.2f nocore %.2f ms; render thread game "
               "%.2f host %.2f eq %.2f nocore %.2f ms",
               (unsigned long long)frame, interval_ns / 1e6, f[0] / 1e6, f[1] / 1e6, f[2] / 1e6, f[3] / 1e6,
               fr[0] / 1e6, fr[1] / 1e6, fr[2] / 1e6, fr[3] / 1e6);
    if (!rep) return;
    char s[1024];
    int o = 0;
    for (int c = 0; c < DG_N; c++) {
        o += snprintf(s + o, sizeof s - o, "%s %.3f ms/frame %llu waits [", dg_name[c], w.ns[c] / 60e6,
                      (unsigned long long)w.n[c]);
        for (int b = 0; b < DG_NB; b++)
            o += snprintf(s + o, sizeof s - o, "%s%llu", b ? " " : "", (unsigned long long)w.hist[c][b]);
        o += snprintf(s + o, sizeof s - o, "]%s", c < DG_N - 1 ? "; " : "");
    }
    rt_log("core-diag: frames %llu-%llu: %s; long frames %llu (game wait > 1 ms in %llu); SVC 0x36 %llu (flag set %llu)",
           (unsigned long long)frame - 59, (unsigned long long)frame, s, (unsigned long long)w.long_frames,
           (unsigned long long)w.long_game_frames, (unsigned long long)s36, (unsigned long long)s36f);
    o = 0;
    for (int k = 0; k < 6; k++) {
        int bi = -1;
        for (int i = 0; i < DG_OWN; i++)
            if (w.own[i].ns && (bi < 0 || w.own[i].ns > w.own[bi].ns)) bi = i;
        if (bi < 0) break;
        o += snprintf(s + o, sizeof s - o, " %#x p%d %s %.3f;", w.own[bi].h, w.own[bi].prio, dg_name[w.own[bi].cls],
                      w.own[bi].ns / 60e6);
        w.own[bi].ns = 0;
    }
    if (o) rt_log("core-diag: owners (ms/frame):%s", s);
}
// MKW_TRACE=sched: every 120 presents, the time split of the busiest threads (running, waiting for a core, host
// calls) and of the render thread, the render and the main thread's longest waits by object and by the thread whose
// signal ended them, and the preemption latency.
#define REP_N 8
// At each present (holding G): starts recording at MKW_CRIT's first frame, stops and writes the file at its last.
// File: "CRIT", thread count, event count (u32 each); per thread handle, priority, flags (1 main, 2 presents), host
// tid (u32 each); then the CritEv records.
static void crit_frame(RtThread *self, uint64_t frame) {
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("MKW_CRIT");
        unsigned long long lo, hi;
        double wl, wh;
        if (e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh &&
            (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_wlo = (int64_t)(wl * 1e6), g_crit_whi = (int64_t)(wh * 1e6);
            g_crit_lo = g_crit_hi = UINT64_MAX;
            rt_log("crit: recording world seconds %g-%g", wl, wh);
        } else if (e && sscanf(e, "%llu-%llu", &lo, &hi) == 2 && lo < hi && (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_lo = lo;
            g_crit_hi = hi;
            rt_log("crit: recording frames %llu-%llu", lo, hi);
        }
    }
    if (g_crit && g_crit_whi && g_crit_hi == UINT64_MAX) {   // world mode: fix the frames as the clock passes lo and hi
        int64_t w = frame_step_world_us();
        if (g_crit_lo == UINT64_MAX) {
            if (w < g_crit_wlo) return;
            g_crit_lo = frame;
        }
        if (w > g_crit_whi) g_crit_hi = frame;
    }
    if (!g_crit || frame < g_crit_lo || frame > g_crit_hi) return;
    g_crit_on = 1;
    crit_add(CRIT_FRAME, self ? self->handle : 0, 0, 0, frame, kern_now_ns());
    if (frame < g_crit_hi) return;
    g_crit_on = 0;
    const char *path = cfg_get("MKW_CRIT_OUT");
    FILE *f = fopen(path && *path ? path : "crit.bin", "wb");
    if (!f) {
        rt_log("crit: cannot write %s", path && *path ? path : "crit.bin");
        return;
    }
    uint32_t nt = 0;
    for (RtThread *t = g_all; t; t = t->all) nt++;
    fwrite("CRIT", 1, 4, f);
    fwrite(&nt, 4, 1, f);
    fwrite(&g_crit_n, 4, 1, f);
    for (RtThread *t = g_all; t; t = t->all) {
        uint32_t row[4] = {t->handle, (uint32_t)t->prio, (uint32_t)(t->is_main | (t == g_render) << 1), t->tid};
        fwrite(row, 4, 4, f);
    }
    fwrite(g_crit, sizeof *g_crit, g_crit_n, f);
    fclose(f);
    rt_log("crit: %u events, %u threads, frames %llu-%llu%s", g_crit_n, nt, (unsigned long long)g_crit_lo,
           (unsigned long long)g_crit_hi, g_crit_n >= CRIT_N ? " (buffer full: the last frames are incomplete)" : "");
}

static void ho_take(struct AddrHoRec *dst, struct AddrHoRec *src) {
    memset(dst, 0, sizeof *dst);
    dst->addr = __atomic_load_n(&src->addr, __ATOMIC_ACQUIRE);
    dst->waker = __atomic_load_n(&src->waker, __ATOMIC_RELAXED);
    dst->n = __atomic_exchange_n(&src->n, 0, __ATOMIC_RELAXED);
    dst->to_n = __atomic_exchange_n(&src->to_n, 0, __ATOMIC_RELAXED);
    dst->tot_ns = __atomic_exchange_n(&src->tot_ns, 0, __ATOMIC_RELAXED);
    dst->host_ns = __atomic_exchange_n(&src->host_ns, 0, __ATOMIC_RELAXED);
    dst->glock_ns = __atomic_exchange_n(&src->glock_ns, 0, __ATOMIC_RELAXED);
    dst->cwait_ns = __atomic_exchange_n(&src->cwait_ns, 0, __ATOMIC_RELAXED);
    dst->cgrant_ns = __atomic_exchange_n(&src->cgrant_ns, 0, __ATOMIC_RELAXED);
    dst->post_ns = __atomic_exchange_n(&src->post_ns, 0, __ATOMIC_RELAXED);
    dst->max_ns = __atomic_exchange_n(&src->max_ns, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < HO_KIND_N; i++) dst->k[i] = __atomic_exchange_n(&src->k[i], 0, __ATOMIC_RELAXED);
}

static void ho_add(struct AddrHoRec *dst, const struct AddrHoRec *src) {
    dst->n += src->n;
    dst->to_n += src->to_n;
    dst->tot_ns += src->tot_ns;
    dst->host_ns += src->host_ns;
    dst->glock_ns += src->glock_ns;
    dst->cwait_ns += src->cwait_ns;
    dst->cgrant_ns += src->cgrant_ns;
    dst->post_ns += src->post_ns;
    if (src->max_ns > dst->max_ns) dst->max_ns = src->max_ns;
    for (int i = 0; i < HO_KIND_N; i++) dst->k[i] += src->k[i];
}

static void ho_fmt(char *b, size_t sz, const struct AddrHoRec *h) {
    double n = h->n ? (double)h->n : 1.0;
    snprintf(b, sz,
             "addr handoff %llu (max %.0f us, to %llu): host %.1f + glock %.1f + cwait %.1f + cgrant %.1f + post %.1f = "
             "%.1f us [uncont %llu free %llu direct %llu spin %llu gate %llu pre %llu]",
             (unsigned long long)h->n, h->max_ns / 1e3, (unsigned long long)h->to_n,
             h->host_ns / 1e3 / n, h->glock_ns / 1e3 / n, h->cwait_ns / 1e3 / n,
             h->cgrant_ns / 1e3 / n, h->post_ns / 1e3 / n, h->tot_ns / 1e3 / n,
             (unsigned long long)h->k[HO_UNCONT], (unsigned long long)h->k[HO_FREE],
             (unsigned long long)h->k[HO_DIRECT], (unsigned long long)h->k[HO_SPIN],
             (unsigned long long)h->k[HO_GATE], (unsigned long long)h->k[HO_PREEMPT]);
}

static void sched_reset_win(RtThread *self, int clear_ho) {
    memset(g_cw_ns, 0, sizeof g_cw_ns);
    memset(g_cw_n, 0, sizeof g_cw_n);
    for (RtThread *t = g_all; t; t = t->all) {
        memset(t->st_ns, 0, sizeof t->st_ns);
        t->nblk = t->npre = 0;
        t->lw_ns = t->lw_n = t->lw_max = t->lg_ns = t->lg_n = t->lg_max = t->sl_n = t->sl_req = t->sl_act = 0;
        t->sp_n = t->sp_ho = 0;   // racy against the thread's own increments: a few passes may be lost
        if (clear_ho || (t != self && t != g_main)) {
            struct AddrHoRec sink;
            if (t->ho_tot.n || t->ho_tot.to_n) ho_take(&sink, &t->ho_tot);
            for (int s = 0; s < HO_SLOTS && t->ho_slots[s].addr; s++)
                if (t->ho_slots[s].n || t->ho_slots[s].to_n) ho_take(&sink, &t->ho_slots[s]);
        }
    }
    memset(g_rw, 0, sizeof g_rw);
    memset(g_mw, 0, sizeof g_mw);
    g_pre_req = g_pre_n = g_pre_ns = g_pre_max = g_pre_stale = 0;
    g_sync_calls = g_sync_blocks = 0;
}

static void sched_emit_line(const char *line) { rt_log("sched: os-thread: %s", line); }

void kern_sched_report(uint64_t frame) {
    static uint64_t last;
    static int os_logged;
    typedef struct {
        RtThread *thr;
        uint32_t h, tid;
        int prio, render, is_main;
        uint64_t mask, entry, ns[S_N], nblk, npre, lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;
        uint64_t sp_n, sp_ho, sp_site;
        uint64_t cpu, cpu_last, cpu_last_at, cpu_last_frame, cpu_cur, cpu_dt, cpu_df;
        struct AddrHoRec ho;
    } Row;
    Row rows[REP_N + 1] = {0};
    WaitRec rw[2][4] = {0};   // the render and the main thread's longest waits
    struct AddrHoRec rwh[2][4] = {0};
    uint64_t waker_entry[2][4] = {0};
    int nr = 0;
    RtThread *self = tcur;
    lock();
    if (g_render != self) {
        g_render = self;
        memset(g_rw, 0, sizeof g_rw);
    }
    crit_frame(self, frame);
    if (!last) {
        last = kern_now_ns();
        for (RtThread *t = g_all; t; t = t->all) acct(t, t->st);
        sched_reset_win(self, 1);
    }
    if (frame % 120) {
        unlock();
        return;
    }
    uint64_t now = kern_now_ns(), span = last ? now - last : 0;
    last = now;
    RtThread *top[REP_N + 1] = {0};
    uint64_t busy[REP_N + 1] = {0}, all_run = 0, all_core = 0;
    for (RtThread *t = g_all; t; t = t->all) {
        acct(t, t->st);   // close the open interval
        uint64_t b = t->st_ns[S_RUN] + t->st_ns[S_CORE] + t->st_ns[S_HOST];
        all_run += t->st_ns[S_RUN];
        all_core += t->st_ns[S_CORE];
        if (t == self) continue;
        for (int i = 0; i < REP_N; i++)
            if (!top[i] || b > busy[i]) {
                memmove(top + i + 1, top + i, (REP_N - 1 - i) * sizeof *top);
                memmove(busy + i + 1, busy + i, (REP_N - 1 - i) * sizeof *busy);
                top[i] = t;
                busy[i] = b;
                break;
            }
    }
    if (self) {   // the render thread first
        memmove(top + 1, top, REP_N * sizeof *top);
        top[0] = self;
    }
    for (int i = 0; i <= REP_N && top[i]; i++) {
        RtThread *t = top[i];
        Row *r = &rows[nr++];
        r->thr = t;
        r->h = t->handle;
        r->tid = t->tid;
        r->prio = t->prio;
        r->render = t == self;
        r->is_main = t->is_main;
        r->mask = t->mask;
        r->entry = t->entry;
        memcpy(r->ns, t->st_ns, sizeof r->ns);
        r->nblk = t->nblk;
        r->npre = t->npre;
        r->lw_ns = t->lw_ns, r->lw_n = t->lw_n, r->lw_max = t->lw_max;
        r->lg_ns = t->lg_ns, r->lg_n = t->lg_n, r->lg_max = t->lg_max;
        r->sl_n = t->sl_n, r->sl_req = t->sl_req, r->sl_act = t->sl_act;
        r->sp_n = t->sp_n, r->sp_ho = t->sp_ho, r->sp_site = t->sp_site;
        r->cpu_last = t->cpu_last;
        r->cpu_last_at = t->cpu_last_at;
        r->cpu_last_frame = t->cpu_last_frame;
        ho_take(&r->ho, &t->ho_tot);
    }
    for (int k = 0; k < 2; k++) {
        const WaitRec *src = k ? g_mw : g_rw;
        for (int i = 0; i < RW_N && src[i].ns; i++)
            for (int j = 0; j < 4; j++)
                if (src[i].ns > rw[k][j].ns) {
                    memmove(rw[k] + j + 1, rw[k] + j, (3 - j) * sizeof **rw);
                    rw[k][j] = src[i];
                    break;
                }
        for (int j = 0; j < 4 && rw[k][j].ns; j++)
            if (rw[k][j].waker)
                for (RtThread *t = g_all; t; t = t->all)
                    if (t->handle == rw[k][j].waker) { waker_entry[k][j] = t->entry; break; }
    }
    for (int k = 0; k < 2; k++) {
        RtThread *wt = k ? g_main : self;
        if (!wt) continue;
        for (int s = 0; s < HO_SLOTS; s++) {
            uint64_t sa = __atomic_load_n(&wt->ho_slots[s].addr, __ATOMIC_ACQUIRE);
            if (!sa) break;
            struct AddrHoRec tmp;
            ho_take(&tmp, &wt->ho_slots[s]);
            for (int j = 0; j < 4 && rw[k][j].ns; j++) {
                if (rw[k][j].st == S_ADDR && rw[k][j].obj == sa &&
                    (!rw[k][j].waker || !tmp.waker || tmp.waker == rw[k][j].waker)) {
                    ho_add(&rwh[k][j], &tmp);
                    break;
                }
            }
        }
    }
    uint64_t cw_ns[DG_N], cw_n[DG_N];
    memcpy(cw_ns, g_cw_ns, sizeof cw_ns);
    memcpy(cw_n, g_cw_n, sizeof cw_n);
    uint64_t pre_stale = g_pre_stale, pre_req = g_pre_req, pre_n = g_pre_n, pre_ns = g_pre_ns, pre_max = g_pre_max;
    uint64_t sc = g_sync_calls, sb = g_sync_blocks;
    sched_reset_win(self, 0);
    unlock();
    // Sample host per-thread CPU time outside G (on PS5 sigsample waits for SIGPROF handler).
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        if (!r->tid) continue;
        uint64_t sample_at = kern_now_ns();
        uint64_t c = plat_thread_cpu_ns(r->tid);
        if (c) {
            r->cpu_dt = r->cpu_last_at && sample_at > r->cpu_last_at ? sample_at - r->cpu_last_at : 0;
            r->cpu_df = r->cpu_last_at && frame > r->cpu_last_frame ? frame - r->cpu_last_frame : 0;
            r->cpu = (r->cpu_last_at && c >= r->cpu_last) ? c - r->cpu_last : 0;
            r->cpu_cur = c;
            r->cpu_last_at = sample_at;
        }
    }
    lock();
    for (int i = 0; i < nr; i++) {
        if (!rows[i].cpu_cur) continue;
        for (RtThread *t = g_all; t; t = t->all)
            if (t == rows[i].thr && t->handle == rows[i].h) {
                t->cpu_last = rows[i].cpu_cur;
                t->cpu_last_at = rows[i].cpu_last_at;
                t->cpu_last_frame = frame;
                break;
            }
    }
    unlock();
    if (!span) return;
    uint64_t cw_tot_n = cw_n[0] + cw_n[1] + cw_n[2] + cw_n[3];
    char cwb[160] = "";
    if (cw_tot_n)
        snprintf(cwb, sizeof cwb, "; core wait by game %.2f (%llu) host %.2f (%llu) eq %.2f (%llu) nocore %.2f (%llu) s",
                 cw_ns[DG_GAME] / 1e9, (unsigned long long)cw_n[DG_GAME], cw_ns[DG_HOST] / 1e9,
                 (unsigned long long)cw_n[DG_HOST], cw_ns[DG_EQ] / 1e9, (unsigned long long)cw_n[DG_EQ],
                 cw_ns[DG_NOCORE] / 1e9, (unsigned long long)cw_n[DG_NOCORE]);
    rt_log("sched: frame %llu: %.2f s, cores busy %.2f of %d, core wait %.2f thread-s%s, preempt req %llu released %llu "
           "avg %.2f ms max %.2f ms, WaitSynchronization %llu calls %llu blocks, stale preempt %llu%s",
           (unsigned long long)frame, span / 1e9, (double)all_run / span, NCORE, all_core / 1e9, cwb,
           (unsigned long long)pre_req, (unsigned long long)pre_n, pre_n ? pre_ns / 1e6 / pre_n : 0.0, pre_max / 1e6,
           (unsigned long long)sc, (unsigned long long)sb, (unsigned long long)pre_stale, self ? "" : " (render thread is not a guest thread)");
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        char d[128], s[256];
        int o = 0;
        for (int k = S_RUN; k < S_N; k++)
            o += snprintf(s + o, sizeof s - o, " %s %.0f%%", s_name[k], 100.0 * r->ns[k] / span);
        ldr_describe(r->entry, d, sizeof d);
        double cpu_pct = r->cpu_dt ? 100.0 * r->cpu / r->cpu_dt : 0.0;
        double cpu_msf = r->cpu_df ? r->cpu / 1e6 / r->cpu_df : 0.0;
        rt_log("sched: %s%s%#x p%d m%llx%s cpu %.0f%% (%.2f ms/f) blocks %llu preempted %llu (%s)",
               r->render ? "render " : "", r->is_main ? "main " : "",
               r->h, r->prio, (unsigned long long)r->mask, s, cpu_pct, cpu_msf,
               (unsigned long long)r->nblk, (unsigned long long)r->npre, r->entry ? d : "main");
        rt_log("sched:   %#x wake to run avg %.1f us max %.0f us (%llu), core grant to run avg %.1f us max %.0f us "
               "(%llu), sleep %llu asked avg %.1f us took avg %.1f us", r->h, r->lw_n ? r->lw_ns / 1e3 / r->lw_n : 0.0,
               r->lw_max / 1e3, (unsigned long long)r->lw_n, r->lg_n ? r->lg_ns / 1e3 / r->lg_n : 0.0, r->lg_max / 1e3,
               (unsigned long long)r->lg_n, (unsigned long long)r->sl_n, r->sl_n ? r->sl_req / 1e3 / r->sl_n : 0.0,
               r->sl_n ? r->sl_act / 1e3 / r->sl_n : 0.0);
        if (r->ho.n || r->ho.to_n) {
            char hb[256];
            ho_fmt(hb, sizeof hb, &r->ho);
            rt_log("sched:   %#x %s", r->h, hb);
        }
        if (r->sp_n) {
            ldr_describe(r->sp_site, d, sizeof d);
            rt_log("sched:   %#x spin wait passes %llu, core handoffs %llu, last at %s", r->h,
                   (unsigned long long)r->sp_n, (unsigned long long)r->sp_ho, d);
        }
    }
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 4 && rw[k][j].ns; j++) {
            char by[160] = "", wd[128];
            if (rw[k][j].waker) {
                if (waker_entry[k][j]) {
                    ldr_describe(waker_entry[k][j], wd, sizeof wd);
                    snprintf(by, sizeof by, ", woken by %#x (%s)", rw[k][j].waker, wd);
                } else {
                    snprintf(by, sizeof by, ", woken by %#x", rw[k][j].waker);
                }
            }
            rt_log("sched: %s wait %d: %s on %s%#llx%s: %.1f%% (%.2f ms/f)", k ? "main" : "render", j + 1, s_name[rw[k][j].st],
                   rw[k][j].st == S_HOST ? "exe+" : "", (unsigned long long)rw[k][j].obj, by,
                   100.0 * rw[k][j].ns / span, rw[k][j].ns / 1e6 / 120.0);
            if (rwh[k][j].n || rwh[k][j].to_n) {
                char hb[256];
                ho_fmt(hb, sizeof hb, &rwh[k][j]);
                rt_log("sched:   %s wait %d %s", k ? "main" : "render", j + 1, hb);
            }
        }
    static const char *const host_names[] = {
        "vk replay", "vk present", "vk shadow", "vk chunks", "vk headroom", "vk tex pool", "pipe lib", "pipe autosave",
    };
    enum { NH = sizeof host_names / sizeof *host_names };
    static int64_t host_prev[NH];
    char hbuf[384];
    int ho = 0;
    for (int i = 0; i < (int)NH; i++) {
        int64_t c = plat_threads_cpu_ns_named(host_names[i]);
        if (c >= 0) {
            int64_t d = host_prev[i] ? c - host_prev[i] : 0;
            host_prev[i] = c;
            ho += snprintf(hbuf + ho, sizeof hbuf - ho, "%s%s %.2f ms/f", ho ? ", " : "", host_names[i], d / 1e6 / 120.0);
        }
    }
    if (ho) rt_log("sched: host threads: %s", hbuf);
    // MKW_THREAD_CPU_CHECK=1: the same times read on each thread through a signal (sigsample.c, the PS5's way), next
    // to the OS's own (Linux: /proc), to check that way where both exist.
    static int check = -1;
    if (check < 0) check = cfg_get("MKW_THREAD_CPU_CHECK") != NULL;
    if (check) {
        static int64_t sig_prev[NH];
        ho = 0;
        for (int i = 0; i < (int)NH; i++) {
            int64_t c = plat_sample_cpu_named(host_names[i]);
            if (c >= 0) {
                int64_t d = sig_prev[i] ? c - sig_prev[i] : 0;
                sig_prev[i] = c;
                ho += snprintf(hbuf + ho, sizeof hbuf - ho, "%s%s %.2f ms/f", ho ? ", " : "", host_names[i], d / 1e6 / 120.0);
            }
        }
        if (ho) rt_log("sched: host threads (signal): %s", hbuf);
    }
    if (!os_logged) {
        os_logged = 1;
        plat_threads_log(sched_emit_line);
        // The guest's threads on the OS's (join on tid with the os-thread lines): guest priority and core mask.
        enum { GT = 96 };
        struct { uint32_t h, tid; int prio; uint64_t mask, entry; } gt[GT];
        int ng = 0;
        lock();
        for (RtThread *t = g_all; t && ng < GT; t = t->all)
            gt[ng].h = t->handle, gt[ng].tid = t->tid, gt[ng].prio = t->prio, gt[ng].mask = t->mask,
            gt[ng].entry = t->entry, ng++;
        unlock();
        for (int i = 0; i < ng; i++) {
            char d[128];
            ldr_describe(gt[i].entry, d, sizeof d);
            rt_log("sched: guest-thread: handle %#x tid=%u guest prio %d guest mask %#llx (%s)", gt[i].h, gt[i].tid,
                   gt[i].prio, (unsigned long long)gt[i].mask, gt[i].entry ? d : "main");
        }
    }
}

// ---- MKW_PROFILE_FN: where the game's threads run, without signals
// On the PS5 a SIGPROF reaches a thread only when it enters the kernel, so signal sampling lands at system calls and
// finds ~17 of the ~70 ms a frame the game's threads run (15/11/2026). Here the function-entry hook of the lifted code
// (fnprof_hit, g_fnprof) stores each thread's last lifted function in its record, and a sampler thread counts, every
// period, those of the threads that hold a virtual core (running game code or a host call it made; -1 while blocked).
// A function's samples x period is the time its thread spent there or in what it called that is not lifted (host
// calls, leaf code without an entry hook). It costs one call and a store per guest call while on.
// The function is the last one ENTERED, not the one running: code after a call returns counts for the callee's last
// entered function (main+0x154e14's list walk, after nn::os::LockMutex, counts for the mutex's Enter). And a thread in
// an early-read wait for the GPU (memory_early.c) keeps its virtual core, so the wait counts as running in the function
// that read: main+0x154e14's 6.3-7 ms a frame were such a wait at its start (task 36, 16/11/2026). Those samples are
// counted apart too (w), and the report gives them beside each function.
volatile int g_fn_sampling;
static uint32_t fns_period_us = 1000;
#define FNS_N 16384
static struct { uint64_t addr, n, w; } fns[FNS_N];
// By call site (the return address of the last call): who called the hot function. A stub a wrapper tail-calls
// (nn::os::YieldThread -> svc 0x0B) keeps its caller's return address, so the site is the game's own code.
static struct { uint64_t fn, lr, n; } fsites[FNS_N];
static uint64_t fns_dropped, fns_run, fns_ticks, fns_threads, fns_wait;
static PlatMutex fns_lock = PLAT_MUTEX_INIT;
// MKW_PROFILE_FN_MAINWAIT=1: the game's main thread spends ~9.5 ms of a 22 ms race frame blocked on an address that
// the job chain signals (MKW_TRACE=sched, 19/11/2026). At each tick where main is so blocked: how many other game
// threads hold a core, how many are off a core without a wait (waiting for a core, or in a blocking host call), and
// the functions the running ones are in. Found (mw-fn3-20261119-172424): ~3.7 running, all 6 cores busy a third of
// the time, ~20 % of it the runtime's own WaitForAddress, CriticalSection and GetTlsValue.
#define MW_N 1024
static int mw_on = -1;
static struct { uint64_t addr, n; } mwf[MW_N];
static uint64_t mw_nrun[8];   // ticks with N other game threads running (7: 7 or more)
static uint64_t mw_ticks, mw_run, mw_core;

void kern_fn_note(uint64_t addr, uint64_t lr) {
    RtThread *t = tcur;
    if (!t) return;
    __atomic_store_n(&t->prof_fn, addr, __ATOMIC_RELAXED);
    __atomic_store_n(&t->prof_lr, lr, __ATOMIC_RELAXED);
}

static void fns_add(uint64_t a, int waiting) {   // fns_lock held
    for (uint32_t i = (uint32_t)((a >> 2) * 0x9E3779B1u) % FNS_N, k = 0; k < 64; k++, i = (i + 1) % FNS_N) {
        if (fns[i].addr == a) { fns[i].n++, fns[i].w += (uint64_t)waiting; return; }
        if (!fns[i].addr) { fns[i].addr = a, fns[i].n = 1, fns[i].w = (uint64_t)waiting; return; }
    }
    fns_dropped++;
}

static void fsites_add(uint64_t fn, uint64_t lr) {   // fns_lock held
    uint64_t h = (fn ^ (lr * 0x9E3779B97F4A7C15ull)) >> 7;
    for (uint32_t i = (uint32_t)h % FNS_N, k = 0; k < 64; k++, i = (i + 1) % FNS_N) {
        if (fsites[i].fn == fn && fsites[i].lr == lr) { fsites[i].n++; return; }
        if (!fsites[i].fn) { fsites[i].fn = fn, fsites[i].lr = lr, fsites[i].n = 1; return; }
    }
}

static void fn_sampler_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_sleep_ns((uint64_t)fns_period_us * 1000ull);
        lock();   // the thread list; held for a few microseconds
        plat_mutex_lock(&fns_lock);
        for (RtThread *t = g_all; t; t = t->all) {
            fns_threads++;
            if (t->cur_core < 0) continue;
            uint64_t a = __atomic_load_n(&t->prof_fn, __ATOMIC_RELAXED);
            if (!a) continue;
            int w = __atomic_load_n(&t->gpu_wait, __ATOMIC_RELAXED) != 0;
            fns_add(a, w);
            fns_wait += (uint64_t)w;
            fsites_add(a, __atomic_load_n(&t->prof_lr, __ATOMIC_RELAXED));
            fns_run++;
        }
        fns_ticks++;
        RtThread *m = g_main;
        // While main is blocked on an address: how many game threads run then, and in what. A job chain that works
        // shows threads running; one that waits on something outside the game's threads (the GPU, the replay) shows
        // none. (The thread that woke main last is no guide: main is woken by a different worker from wait to wait.)
        if (mw_on > 0 && m && m->cur_core < 0 && m->wkind == W_ADDRESS) {
            mw_ticks++;
            int nrun = 0;
            for (RtThread *w = g_all; w; w = w->all) {
                if (w == m) continue;
                if (w->cur_core >= 0) {
                    nrun++;
                    uint64_t a = __atomic_load_n(&w->prof_fn, __ATOMIC_RELAXED);
                    for (uint32_t i = (uint32_t)((a >> 2) * 0x9E3779B1u) % MW_N, k = 0; k < 32; k++, i = (i + 1) % MW_N) {
                        if (mwf[i].addr == a) { mwf[i].n++; break; }
                        if (!mwf[i].addr) { mwf[i].addr = a, mwf[i].n = 1; break; }
                    }
                } else if (w->wkind == W_NONE) {
                    mw_core++;   // waiting for a core, or in a blocking host call (thread-ticks)
                }
            }
            mw_nrun[nrun < 7 ? nrun : 7]++;
            mw_run += (uint64_t)nrun;
        }
        plat_mutex_unlock(&fns_lock);
        unlock();
    }
}

int kern_fn_profile_start(uint32_t period_us) {
    if (period_us >= 100) fns_period_us = period_us;
    g_fn_sampling = 1;
    if (mw_on < 0) {
        const char *e = cfg_get("MKW_PROFILE_FN_MAINWAIT");
        mw_on = e && *e && strcmp(e, "0");
    }
    rt_fnprof_set(1);
    if (!plat_thread_new(fn_sampler_main, NULL, "fn sampler")) {
        g_fn_sampling = 0;
        rt_log("prof: MKW_PROFILE_FN: the sampler thread did not start");
        return 0;
    }
    rt_log("prof: MKW_PROFILE_FN: the game threads' last lifted function, those holding a virtual core, every %u us",
           fns_period_us);
    return 1;
}

void kern_fn_profile_report(uint64_t frame, uint64_t frames) {
    enum { TOP = 30 };
    struct { uint64_t addr, n, w; } top[TOP] = {{0}};
    plat_mutex_lock(&fns_lock);
    uint64_t run = fns_run, ticks = fns_ticks, dropped = fns_dropped, wait = fns_wait;
    for (int i = 0; i < FNS_N; i++) {
        if (!fns[i].n) continue;
        for (int j = 0; j < TOP; j++)
            if (fns[i].n > top[j].n) {
                memmove(top + j + 1, top + j, (size_t)(TOP - 1 - j) * sizeof *top);
                top[j].addr = fns[i].addr, top[j].n = fns[i].n, top[j].w = fns[i].w;
                break;
            }
    }
    enum { TOPS = 30 };
    struct { uint64_t fn, lr, n; } tops[TOPS] = {{0}};
    for (int i = 0; i < FNS_N; i++) {
        if (!fsites[i].n) continue;
        for (int j = 0; j < TOPS; j++)
            if (fsites[i].n > tops[j].n) {
                memmove(tops + j + 1, tops + j, (size_t)(TOPS - 1 - j) * sizeof *tops);
                tops[j].fn = fsites[i].fn, tops[j].lr = fsites[i].lr, tops[j].n = fsites[i].n;
                break;
            }
    }
    memset(fns, 0, sizeof fns);
    memset(fsites, 0, sizeof fsites);
    fns_run = fns_ticks = fns_dropped = fns_threads = fns_wait = 0;
    enum { MWTOP = 20 };
    struct { uint64_t addr, n; } mwt[MWTOP] = {{0}};
    for (int i = 0; i < MW_N; i++) {
        if (!mwf[i].n) continue;
        for (int j = 0; j < MWTOP; j++)
            if (mwf[i].n > mwt[j].n) {
                memmove(mwt + j + 1, mwt + j, (size_t)(MWTOP - 1 - j) * sizeof *mwt);
                mwt[j].addr = mwf[i].addr, mwt[j].n = mwf[i].n;
                break;
            }
    }
    uint64_t m_nrun[8];
    memcpy(m_nrun, mw_nrun, sizeof m_nrun);
    uint64_t m_ticks = mw_ticks, m_run = mw_run, m_core = mw_core;
    memset(mwf, 0, sizeof mwf);
    memset(mw_nrun, 0, sizeof mw_nrun);
    mw_ticks = mw_run = mw_core = 0;
    plat_mutex_unlock(&fns_lock);
    if (!frames || !ticks) return;
    double per = (double)fns_period_us / 1e3 / (double)frames;   // ms a frame per sample
    rt_log("prof: frame %llu: game threads running %.2f ms/frame (%llu samples in %llu ticks of %u us, %llu not "
           "counted: table full; %.2f ms/frame of it waiting for the GPU in an early read), by last lifted function:",
           (unsigned long long)frame, run * per, (unsigned long long)run, (unsigned long long)ticks, fns_period_us,
           (unsigned long long)dropped, wait * per);
    for (int j = 0; j < TOP && top[j].n; j++) {
        char d[160], w[64] = "";
        ldr_describe(top[j].addr, d, sizeof d);
        if (top[j].w) snprintf(w, sizeof w, "  (%.2f ms/f of it waiting for the GPU)", top[j].w * per);
        rt_log("prof:   f%-2d %5.1f%% %6.2f ms/f  %s%s", j + 1, 100.0 * top[j].n / run, top[j].n * per, d, w);
    }
    rt_log("prof: frame %llu: by call site (function <- the return address of its call):", (unsigned long long)frame);
    for (int j = 0; j < TOPS && tops[j].n; j++) {
        char d[160], l[160];
        ldr_describe(tops[j].fn, d, sizeof d);
        ldr_describe(tops[j].lr, l, sizeof l);
        rt_log("prof:   s%-2d %5.1f%% %6.2f ms/f  %s <- %s", j + 1, 100.0 * tops[j].n / run, tops[j].n * per, d, l);
    }
    if (m_ticks) {
        char hist[256];
        int o = 0;
        for (int j = 0; j < 8; j++)
            o += snprintf(hist + o, sizeof hist - (size_t)o, " %d%s:%.0f%%", j, j == 7 ? "+" : "", 100.0 * m_nrun[j] / m_ticks);
        rt_log("prof: frame %llu: main blocked on an address %.2f ms/f; other game threads running then %.2f on average, "
               "off a core without a wait %.2f; share of that time with N running:%s (MKW_PROFILE_FN_MAINWAIT)",
               (unsigned long long)frame, m_ticks * per, (double)m_run / m_ticks, (double)m_core / m_ticks, hist);
        for (int j = 0; j < MWTOP && mwt[j].n; j++) {
            char d[160];
            ldr_describe(mwt[j].addr, d, sizeof d);
            rt_log("prof:   while main waits, running in %s: %.2f thread-ms/f", d, mwt[j].n * per);
        }
    }
}
