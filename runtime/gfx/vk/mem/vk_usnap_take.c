// MKW_USNAP_AT_SUBMIT (default 0; "ab"): the uniform snapshot's bytes taken at the submit, on the game's thread, instead
// of where the replay starts recording the submission (vk_usnap.c). With the replay ~3 submits behind the game, the
// game is already writing the next frame's per-object blocks when the snapshot reads them: an object drawn with
// another's matrix for one frame, the flickering cacti and bushes (16 in 2 400 race frames
// without MKW_SUBMIT_CATCHUP, pop0p). The take collects the submission's uniform ranges as vk_usnap_begin does, merges
// them and copies their bytes; vk_usnap_begin pops it in submit order and copies regions from it. Costs the game's
// thread the walk and the copy (~0.3-1 ms a submission, the snapshot's own cost when it ran there, vk_usnap.c).
// MKW_USNAP_CMP=1 (a measurement, without MKW_USNAP_AT_SUBMIT): taken, but the snapshot still copies guest memory where
// the recording starts; vk_usnap.c counts
// per frame the ranges whose bytes changed in between (the replay drawing with the next frame's uniforms or handles,
// Hookshot's "MKW shadows in bands for a frame"; candados-lectura, the kart's shadow).
// MKW_USNAP_TAKE_THREAD (default 1; "ab"): the game's thread only queues the submission's handles and a thread of its
// own walks them and copies the bytes right after (Nothing serial that can run elsewhere, AGENTS.md); the replay waits
// for a take only when it reaches the submission before the thread is done with it. Why the take at all on the PS5
// (25/11/2026): with MKW_USNAP_ASYNC the snapshot read guest memory during the walk, later still, after the game had
// rewritten its double-buffered 0xc0-byte frame blocks (0x25a8f77000/0x25a8f77100 alternate; MKW_USNAP_CMP: 2-4
// ranges a frame changed between the submit and the recording): the countdown's scene pass lost the left half of the
// scenery to the sky or went dark every few frames, 19-125 odd frames a 90 s Grand Prix (cyan-base-r1/r2,
// opt1p-range1); 0 and 0 with USNAP_ASYNC=0, UBO_SNAPSHOT=0 or USNAP_AT_SUBMIT=1 (cyan-*-r1/r2, MKW_RTSIG showing the
// HDR scene target as the first pass to go wrong).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_usnap_int.h"
#include "platform/platform.h"

struct UsnapTake {
    UsnapTake *next;
    UsnapTake *job;    // MKW_USNAP_TAKE_THREAD: the thread's queue, in submit order
    uint64_t *hs;      // the handles the thread walks (a copy: the caller's array is gone once the submit returns)
    int ready;         // bytes and ranges filled in (under lock)
    uint64_t h0;   // the submission's first handle: a take that does not match its submission is dropped
    int n;
    Rng *rng;      // merged ranges, sorted by lo
    uint64_t *off; // where each range's bytes start in bytes
    int nrng;
    uint8_t *bytes;
};

static UsnapTake *head, *tail;
static int queued;
static PlatMutex lock = PLAT_MUTEX_INIT;

static PlatCond ready_cv = PLAT_COND_INIT;
static UsnapTake *job_head, *job_tail;
static int thr = -1;   // MKW_USNAP_TAKE_THREAD: 1 a thread of its own, 0 on the submitting thread

static int cmp_on = -1;

static int mode(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("USNAP_AT_SUBMIT", "0");
        cmp_on = !m && cfg_get("MKW_USNAP_CMP") && atoi(cfg_get("MKW_USNAP_CMP")) > 0;
        if (cmp_on) m = 1, rt_log("vk_usnap: uniform ranges the game rewrites between its submit and the recording "
                                  "counted per frame (MKW_USNAP_CMP)");
    }
    return m;
}

static void take_fill(UsnapTake *t, const uint64_t *handles, int n);

static void take_main(void *arg) {
    (void)arg;
    plat_mutex_lock(&lock);
    for (;;) {
        while (!job_head) plat_cond_wait(&ready_cv, &lock);
        UsnapTake *t = job_head;
        if (!(job_head = t->job)) job_tail = NULL;
        plat_mutex_unlock(&lock);
        take_fill(t, t->hs, t->n);
        free(t->hs), t->hs = NULL;
        plat_mutex_lock(&lock);
        t->ready = 1;
        plat_cond_broadcast(&ready_cv);
    }
}

void vk_usnap_take(const uint64_t *handles, int n) {
    int m = mode();
    if (!m || !RT_KNOB_ON(m) || n <= 0) return;
    if (thr < 0) {
        thr = rt_knob("USNAP_TAKE_THREAD", "1");
        if (thr && !plat_thread_spawn(take_main, NULL, "vk usnap take", 0)) thr = 0;
        rt_log("vk_usnap: the submit's copy taken by a thread of its own%s (MKW_USNAP_TAKE_THREAD)",
               thr == 2 ? " in the B blocks" : thr ? "" : ": off");
    }
    UsnapTake *t = calloc(1, sizeof *t);
    if (!t) rt_fatal("vk_usnap: out of memory (take)");
    t->h0 = handles[0], t->n = n;
    int on_thread = RT_KNOB_ON(thr);
    if (on_thread) {
        if (!(t->hs = malloc(sizeof *t->hs * (size_t)n))) rt_fatal("vk_usnap: out of memory (take)");
        memcpy(t->hs, handles, sizeof *t->hs * (size_t)n);
    } else {
        take_fill(t, handles, n);
        t->ready = 1;
    }
    plat_mutex_lock(&lock);
    if (tail) tail->next = t;
    else head = t;
    tail = t, queued++;
    if (on_thread) {
        if (job_tail) job_tail->job = t;
        else job_head = t;
        job_tail = t;
        plat_cond_broadcast(&ready_cv);
    }
    plat_mutex_unlock(&lock);
}

// The submission's uniform ranges, merged, and their bytes (the submitting thread or take_main).
static void take_fill(UsnapTake *t, const uint64_t *handles, int n) {
    static _Thread_local RngVec v;
    v.n = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) usnap_collect(&v, nvn_op_first(handles[i]), 0);
    usnap_sort(&v);
    if (v.n && (!(t->rng = malloc(sizeof *t->rng * (size_t)v.n)) || !(t->off = malloc(sizeof *t->off * (size_t)v.n))))
        rt_fatal("vk_usnap: out of memory (take)");
    uint64_t total = 0;
    for (int i = 0; i < v.n;) {
        uint64_t lo = v.r[i].lo, hi = v.r[i].hi;
        for (i++; i < v.n && v.r[i].lo <= hi; i++)
            if (v.r[i].hi > hi) hi = v.r[i].hi;
        t->rng[t->nrng] = (Rng){lo, hi}, t->off[t->nrng++] = total, total += hi - lo;
    }
    if (total && !(t->bytes = malloc(total))) rt_fatal("vk_usnap: out of memory (take of %llu bytes)", (unsigned long long)total);
    // Never waiting (mem_copy_out_try): a range with a page the early-read watch holds is left out of the take (made
    // empty, so usnap_take_bytes finds nothing there) and the snapshot reads guest memory for it as before.
    static uint64_t n_out;
    for (int i = 0; i < t->nrng; i++)
        if (!mem_copy_out_try(t->bytes + t->off[i], t->rng[i].lo, t->rng[i].hi - t->rng[i].lo)) {
            t->rng[i].hi = t->rng[i].lo;
            uint64_t k = __atomic_add_fetch(&n_out, 1, __ATOMIC_RELAXED);
            if (!(k & (k - 1))) rt_log("vk_usnap: %llu uniform ranges left out of a submit's copy: a page the GPU has yet to write", (unsigned long long)k);
        }
}

int usnap_take_compare(void) { return mode() && cmp_on; }

// The oldest take of this submission, wherever it is in the queue: a held submission (nvn_qorder.c) keeps its place in
// the replay but gets its handles, and so its take (vk_be_held_fill), only once its wait is met, after the takes of
// submissions the game made meanwhile. Held ones took nothing before, and popping the head only would pair every take
// after a held one with the wrong submission, all dropped (a race holds ~2 submissions a frame, inflb-1). Takes no
// submission claims (a submission of no handles) are dropped once TAKE_KEEP newer ones wait behind them.
#define TAKE_KEEP 64
UsnapTake *usnap_take_pop(const uint64_t *handles, int n) {
    if (!head || n <= 0) return NULL;   // read without the lock: a take queued meanwhile belongs to a later submission
    UsnapTake *t, *prev = NULL, *stale = NULL;
    plat_mutex_lock(&lock);
    for (t = head; t && (t->h0 != handles[0] || t->n != n); prev = t, t = t->next) {}
    if (t) {
        if (prev) prev->next = t->next;
        else head = t->next;
        if (tail == t) tail = prev;
        queued--;
    }
    if (queued > TAKE_KEEP && head && head->ready) {   // the oldest has waited too long (and the thread is done with it)
        stale = head, head = head->next, queued--;
        if (!head) tail = NULL;
    }
    while (t && !t->ready) plat_cond_wait(&ready_cv, &lock);   // MKW_USNAP_TAKE_THREAD: the thread not done with it yet
    plat_mutex_unlock(&lock);
    if (stale) {
        static volatile int32_t said;
        if (plat_inc32(&said) <= 3) rt_log("vk_usnap: a submit's copy no submission claimed: dropped");
        usnap_take_free(stale);
    }
    return t;   // NULL: none of this submission's (guest memory as before)
}

const uint8_t *usnap_take_bytes(const UsnapTake *t, uint64_t lo, uint64_t hi) {
    int a = 0, b = t->nrng;   // the last range starting at or before lo
    while (b - a > 1) {
        int m = (a + b) / 2;
        if (t->rng[m].lo <= lo) a = m;
        else b = m;
    }
    if (!t->nrng || t->rng[a].lo > lo || t->rng[a].hi < hi) return NULL;
    return t->bytes + t->off[a] + (lo - t->rng[a].lo);
}

void usnap_take_free(UsnapTake *t) {
    if (!t) return;
    free(t->rng), free(t->off), free(t->bytes), free(t);
}
