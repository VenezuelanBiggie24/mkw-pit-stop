// The game's queues ordered by their events (MKW_QUEUE_WAITS, default 1; 0 off, "ab": in the B blocks). MKW's two worker
// queues pass stages back and forth with nvnCommandBufferWaitEvent; replayed in submit order on our one Vulkan queue,
// a wait the signals submitted before it do not meet ran its work before what it waits for (wc1: 50-65 % of the waits,
// each met by the other queue's next submit; Hookshot's finding in another MKW recomp, an idea only: GPL-3.0).
//
// Here, at submit (game thread, under the front end's submit lock), each event's value is followed in the order the
// console's GPU would reach the signals. A submission that reaches a wait not met is held: its place in the replay
// queue is kept (gfx submit_held, so its fence value, captures and what follows it stay where they were), and the ops
// up to the wait go into the episode's stream. Every later submission while one is held is held too; each queue runs
// as far as its waits allow, in turns, and its ops follow in the stream behind an OP_QUEUE mark (the back end keeps a
// replay state per queue, vk_exec.c queue_switch). A signal that meets another queue's wait hands the turn to that
// queue (the oldest held first), as the console's GPU lets the waiting queue on at the signal while the signalling one
// goes on beside it. Each held submission gets the part of the stream from the end of the part before to where its
// own last op is (or nothing when an earlier part already holds it), as soon as that is known and the parts before
// are given: its fence value is signalled once its ops and those of every submission before it are done, never
// earlier than on the console. Before, the first held submission got the whole stream when the episode ended: in a
// race the compute queue's small submission waits for the main queue's, so its fence came only after the whole frame's
// draws, and the job threads' early-read waits on it (MKW_EARLY_READ=3, the page's previous writer) went from ~2.5 to
// ~19 ms each on the PS5 (base60 against qw-ab, 17/11/2026). A part after the first starts with an OP_QUEUE mark
// whose cont asks the back end to keep the queues' states from the part before. A CPU signal (nvnEventSignal) can let
// a queue on too.
//
// MKW_QORDER_PARTS (default 1; 0: the first held submission gets the whole stream, as before; "ab": parts in the B
// blocks), read once and applied per episode (decided when one opens, so a block boundary never splits one): the PS5
// A/B of MKW_QUEUE_WAITS (qw-ab-20261117-105911) measured 25.15 ms of work a frame with it off against 31.18 on
// (+6.0 ms), the early-read waits ~2.3 -> ~20 ms a frame, because slot 5k+3's fence covered the whole frame's draws.
//
// MKW_QORDER_PRE (default 1; 0 off; "ab": in the B blocks; with parts only, decided per episode as they are): the ops
// of the submission that opens an episode up to its first wait not met go to the replay at once, as a submission of
// their own (nvn_qorder_submit's *pre) before the held one, and what they write is watched with that fence
// (nvn_api_core.c early_watch_submit): on the console the queue runs them as soon as they are submitted and stops only
// at the wait. With parts alone the race frame was still ~5 ms over MKW_QUEUE_WAITS=0 (qp-ab2-20261117-140208: A 35.03,
// B 30.07 ms busy, against ~25): the compute queue's wait is met ~2/3 into the main queue's draws (its part 1,450-1,700
// draws, the main one's rest 600-950), and every early-read wait of the race (main+0x154e14, +0x3d4f80, +0x2c9e0, ~23 ms
// a frame summed) was a sibling 4 KiB page of a page the compute submission watches, waiting for that part.
//
// Let go as it stands, as before (gfx_held_force): when the replay reaches a held submission still empty and waits
// MKW_QUEUE_HOLD_MS (vk_backend.c), the oldest waiting queue goes past its wait, the others as far as theirs allow.
// MKW_QUEUE_WAITS=0 only counts the waits the submit order breaks (the census of wc1), one line every 1,500 submits.
// On by default since #430: in a split-screen race both views' culling blocks run on the compute queue and write the
// same buffers, each followed on the console by its own view; in submit order the second block ran before the first
// view, so every view drew with the last view's culling (a parked player 1 lost the crowd the far player 2 no longer
// saw: i430-def against i430-qw, GTX 1050 Ti, 13.8 against 14.8 fps). Off before for +3.1 ms a frame on a Radeon 660M
// (qw2j); the black cacti it seemed to add were a translator atomic, fixed in 9129803.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define EV_MAX 512
#define RUNS_MAX 4
#define SLOTS_MAX 64

typedef struct Piece { struct Piece *next; size_t n; int slot; uint8_t ops[]; } Piece;   // a held submission's ops
// pos: head's wait not met, or (ready) the op to go on from: the queue gave the turn at a signal
typedef struct { const void *queue; Piece *head, *tail; size_t pos; int ready; } Run;
typedef struct { void *slot; int attached, decided; uint64_t h; } FeSlot;       // h 0: empty
typedef struct { const uint8_t *p, *end; } Span;

static PlatMutex qo = PLAT_MUTEX_INIT;
static struct { volatile uint32_t *val; uint32_t v; } ev[EV_MAX];
static Run runs[RUNS_MAX];
static int nruns;
static FeSlot *slots[SLOTS_MAX];
static int nslots, ngiven;   // the episode's held submissions; those whose part is decided
// Per held submission of the episode (its index in slots): where in the stream its last op ends and the queue there.
static struct { size_t end; const void *queue; int ended; } ends[SLOTS_MAX];
static size_t cut;             // the stream up to here is in the parts given
static int ep_parts;           // the open episode hands out parts (MKW_QORDER_PARTS when it opened)
static int ep_pre;             // and its first submission's ops before its wait went ahead (MKW_QORDER_PRE)
static const void *cut_queue;  // the queue whose ops are at cut
static struct { uint8_t *b; size_t n, cap; const void *queue; } out;   // the episode's stream
static uint64_t st_waits, st_unmet, st_episodes, st_held, st_forced, st_cpu, st_bytes;

static int mode(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("QUEUE_WAITS", "1");
        rt_log("nvn: the queues' work ordered by their event waits%s (MKW_QUEUE_WAITS)",
               !m ? ": off, a census only" : m == 2 ? " in the B blocks" : "");
    }
    return m;
}

static int parts_mode(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("QORDER_PARTS", "1");
        rt_log("nvn: each held submission gets its part of the queue order stream%s (MKW_QORDER_PARTS)",
               !m ? ": off, the first one gets it whole" : m == 2 ? " in the B blocks" : "");
    }
    return m;
}

static int pre_mode(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("QORDER_PRE", "1");
        rt_log("nvn: a held submission's ops before its wait go ahead with their own fence%s (MKW_QORDER_PRE)",
               !m ? ": off" : m == 2 ? " in the B blocks" : "");
    }
    return m;
}

static int ev_slot(volatile uint32_t *val) {   // the event's slot (taken if new); -1: the table is full
    uint32_t k = (uint32_t)(((uintptr_t)val >> 2) * 0x9E3779B1u) % EV_MAX;
    for (int n = 0; n < EV_MAX; n++, k = (k + 1) % EV_MAX) {
        if (ev[k].val == val) return (int)k;
        if (!ev[k].val) return ev[k].val = val, ev[k].v = *val, (int)k;
    }
    return -1;
}

static int wait_met(const p_wait_event *w) {   // (an event the table has no room for: as before, not waited for)
    int k = ev_slot(w->val);
    if (k < 0) return 1;
    return w->mode == NVN_EVENT_WAIT_MODE_EQUAL ? ev[k].v == w->value : (int32_t)(ev[k].v - w->value) >= 0;
}

static void signal(const p_signal_event *e) {
    int k = ev_slot(e->val);
    if (k >= 0) ev[k].v = e->mode == NVN_EVENT_SIGNAL_MODE_ADD ? ev[k].v + e->value : e->value;
}

static const op_hdr *op_at(const uint8_t *p) { return (const op_hdr *)(const void *)p; }

static int run_unblocked(const void *self) {   // a queue other than self waits and its wait is met now
    for (int i = 0; i < nruns; i++)
        if (runs[i].queue != self && !runs[i].ready && wait_met(nvn_op_payload(op_at(runs[i].head->ops + runs[i].pos))))
            return 1;
    return 0;
}

// Follows the ops of [p, end) (no BEGIN or END among them): their signals reach the events; returns the first wait not
// met (stop), or end. stop 0: past every wait (the census), counting those not met. paused (stop 1): set, with the
// position after it returned, at a signal that meets the wait of a queue other than self (the turn goes there).
static const uint8_t *follow(const uint8_t *p, const uint8_t *end, int stop, const void *self, int *paused) {
    for (; p < end; p += op_at(p)->size) {
        const op_hdr *h = op_at(p);
        if (h->op == OP_SIGNAL_EVENT) {
            signal(nvn_op_payload(h));
            if (paused && ep_parts && nruns && run_unblocked(self)) return *paused = 1, p + h->size;
        } else if (h->op == OP_WAIT_EVENT) {
            if (!wait_met(nvn_op_payload(h))) {
                if (stop) return p;
                st_unmet++;
            }
            st_waits++;
        }
    }
    return end;
}

static Span handle_ops(uint64_t handle) {   // a recording's ops between its OP_BEGIN and OP_END
    const uint8_t *p = (const uint8_t *)nvn_op_next(nvn_op_first(handle)), *e = p;
    while (op_at(e)->op != OP_END) e += op_at(e)->size;
    return (Span){p, e};
}

static void out_put(const void *src, size_t n) {
    if (out.n + n > out.cap) {
        out.cap = (out.n + n) * 2 > 65536 ? (out.n + n) * 2 : 65536;
        if (!(out.b = realloc(out.b, out.cap))) rt_fatal("nvn: out of memory (queue order stream)");
    }
    memcpy(out.b + out.n, src, n);
    out.n += n, st_bytes += n;
}

static void out_ops(const void *queue, const uint8_t *p, const uint8_t *end) {   // the queue's ops [p, end)
    if (p == end) return;
    if (queue != out.queue) {
        struct { op_hdr h; p_queue q; } m = {{OP_QUEUE, sizeof m}, {(uintptr_t)queue, 0}};
        out_put(&m, sizeof m);
        out.queue = queue;
    }
    out_put(p, (size_t)(end - p));
}

static Piece *piece_new(size_t n, int slot) {
    Piece *pc = malloc(sizeof *pc + n);
    if (!pc) rt_fatal("nvn: out of memory (held submission, %zu bytes)", n);
    pc->next = NULL, pc->n = 0, pc->slot = slot;
    return pc;
}

static void slot_ended(int slot) {   // the held submission's ops are all in the stream
    ends[slot].end = out.n, ends[slot].queue = out.queue, ends[slot].ended = 1;
}

static void piece_add(Piece *pc, const uint8_t *p, const uint8_t *end) {
    memcpy(pc->ops + pc->n, p, (size_t)(end - p));
    pc->n += (size_t)(end - p);
}

static void run_push(Run *r, Piece *pc) {
    if (r->tail) r->tail->next = pc;
    else r->head = pc;
    r->tail = pc;
}

static Run *run_of(const void *queue) {
    for (int i = 0; i < nruns; i++)
        if (runs[i].queue == queue) return &runs[i];
    return NULL;
}

// A submission (index slot among the episode's held ones) of a queue that has no held work: its ops go into the stream
// as far as its waits allow, or until it gives the turn; the rest is held in a new run. Only signals reach the events
// (follow), so handles before the stop are complete.
static void run_new_submission(const void *queue, const uint64_t *handles, int n, int slot) {
    for (int i = 0; i < n; i++) {
        Span s = handle_ops(handles[i]);
        int paused = 0;
        const uint8_t *stop = follow(s.p, s.end, 1, queue, &paused);
        out_ops(queue, s.p, stop);
        if (stop == s.end && (!paused || i + 1 == n)) continue;
        size_t bytes = (size_t)(s.end - stop);
        for (int j = i + 1; j < n; j++) {
            Span t = handle_ops(handles[j]);
            bytes += (size_t)(t.end - t.p);
        }
        Piece *pc = piece_new(bytes, slot);
        piece_add(pc, stop, s.end);
        for (int j = i + 1; j < n; j++) {
            Span t = handle_ops(handles[j]);
            piece_add(pc, t.p, t.end);
        }
        runs[nruns++] = (Run){queue, pc, pc, 0, paused};
        return;
    }
    slot_ended(slot);
}

// A run goes on from its wait (past it: met now, or forced), or from where it gave the turn, as far as its waits allow
// or until it gives the turn again; 0 when it has no work left.
static int run_resume(Run *r) {
    Piece *pc = r->head;
    const uint8_t *p = pc->ops + r->pos;
    if (!r->ready) p += op_at(p)->size, st_waits++;
    r->ready = 0;
    for (;;) {
        int paused = 0;
        const uint8_t *end = pc->ops + pc->n, *stop = follow(p, end, 1, r->queue, &paused);
        out_ops(r->queue, p, stop);
        if (stop != end) return r->pos = (size_t)(stop - pc->ops), r->ready = paused, 1;
        slot_ended(pc->slot);
        r->head = pc->next;
        free(pc);
        if (!(pc = r->head)) return r->tail = NULL, 0;
        p = pc->ops;
        if (paused) return r->pos = 0, r->ready = 1, 1;   // (the turn given at its last op)
    }
}

static void run_drop(int i) {
    for (; i + 1 < nruns; i++) runs[i] = runs[i + 1];   // (in the order they were held: the oldest first)
    nruns--;
}

static void fill(FeSlot *s) {   // its content decided and its back-end slot known
    gfx_backend()->held_fill(s->slot, &s->h, s->h ? 1 : 0);
    free(s);
}

// The stream [cut, end) as one recording: after the first part, behind a mark that keeps the queues' replay states
// (cont) and names the queue whose ops are at cut.
static uint64_t part_handle(size_t end) {
    if (!cut) return nvn_stream_handle(out.b, end);
    static uint8_t *tmp;
    static size_t cap;
    struct { op_hdr h; p_queue q; } m = {{OP_QUEUE, sizeof m}, {(uintptr_t)cut_queue, 1}};
    size_t n = sizeof m + (end - cut);
    if (n > cap) {
        cap = n * 2 > 65536 ? n * 2 : 65536;
        if (!(tmp = realloc(tmp, cap))) rt_fatal("nvn: out of memory (queue order part)");
    }
    memcpy(tmp, &m, sizeof m);
    memcpy(tmp + sizeof m, out.b + cut, end - cut);
    return nvn_stream_handle(tmp, n);
}

// The held submissions whose ops are all in the stream, in their order: each gets the stream from cut to its last op
// (nothing when an earlier one's part holds it: fence values only grow).
static void give_parts(void) {
    while (ep_parts && ngiven < nslots && ends[ngiven].ended) {
        FeSlot *s = slots[ngiven];
        size_t e = ends[ngiven].end;
        const void *q = ends[ngiven].queue;
        ngiven++;
        s->decided = 1, s->h = 0;
        if (e > cut) s->h = part_handle(e), cut = e, cut_queue = q;
        if (s->attached) fill(s);
    }
}

// Every queue as far as its waits allow, in turns: one whose wait is met first (the oldest held), else one that gave
// the turn at a signal.
static void schedule(void) {
    for (;;) {
        int k = -1;
        for (int i = 0; i < nruns && k < 0; i++)
            if (!runs[i].ready && wait_met(nvn_op_payload(op_at(runs[i].head->ops + runs[i].pos)))) k = i;
        for (int i = 0; i < nruns && k < 0; i++)
            if (runs[i].ready) k = i;
        if (k < 0) break;
        if (!run_resume(&runs[k])) run_drop(k);
    }
    give_parts();
}

// No queue waits: every held submission's ops are in the stream; those left get their parts.
static void close_episode(void) {
    if (!ep_parts) {   // (MKW_QORDER_PARTS=0: the first gets the stream, the others nothing)
        uint64_t h = out.n ? nvn_stream_handle(out.b, out.n) : 0;
        for (int i = 0; i < nslots; i++) {
            FeSlot *s = slots[i];
            s->decided = 1, s->h = i ? 0 : h;
            if (s->attached) fill(s);
        }
        ngiven = nslots;
    }
    give_parts();
    if (ngiven != nslots) rt_fatal("nvn: queue order episode closed with %d of %d submissions unplaced", nslots - ngiven, nslots);
    nslots = ngiven = 0, out.n = cut = 0, out.queue = cut_queue = NULL;
}

static int force(void) {   // qo held: every queue on, the oldest waiting one past its wait whenever all of them wait
    int n = 0;
    while (nruns) {
        schedule();
        if (!nruns) break;
        st_forced++, n++;
        if (!run_resume(&runs[0])) run_drop(0);
    }
    if (nslots) close_episode();
    return n;
}

int gfx_held_force(void) {
    plat_mutex_lock(&qo);
    int n = force();
    plat_mutex_unlock(&qo);
    return n;
}

// The submit numbers (nvn_submit_count) of the latest submissions with a wait the submit order does not meet (the
// census) or that were held for one: vk_texture_astc_copy's zero-block line asks about the submission it is replaying
// (task G: the 8192x8192 terrain staging texture decoded as all error blocks at loading, as if read before the CPU
// wrote it, which on the console a wait on a CPU-signalled event would prevent). Written under qo, read without it.
#define UNMET_RING 1024
static uint64_t unmet_ring[UNMET_RING];
static uint64_t unmet_n;

static void unmet_note(void) {
    uint64_t sub = nvn_submit_count(), n = __atomic_load_n(&unmet_n, __ATOMIC_RELAXED);
    if (n && __atomic_load_n(&unmet_ring[(n - 1) % UNMET_RING], __ATOMIC_RELAXED) == sub) return;
    __atomic_store_n(&unmet_ring[n % UNMET_RING], sub, __ATOMIC_RELAXED);
    __atomic_store_n(&unmet_n, n + 1, __ATOMIC_RELEASE);
}

int nvn_qorder_unmet(uint64_t sub) {
    uint64_t n = __atomic_load_n(&unmet_n, __ATOMIC_ACQUIRE);
    uint64_t k = n < UNMET_RING ? n : UNMET_RING;
    for (uint64_t i = 0; i < k; i++)
        if (__atomic_load_n(&unmet_ring[(n - 1 - i) % UNMET_RING], __ATOMIC_RELAXED) == sub) return 1;
    // older than the oldest one kept: not known
    if (n > UNMET_RING && sub < __atomic_load_n(&unmet_ring[n % UNMET_RING], __ATOMIC_RELAXED)) return -1;
    return 0;
}

static void log_line(void) {
    static uint64_t subs;
    if (++subs % 1500) return;
    rt_log("nvn: queue order in the last 1500 submits: %llu event waits, %llu not met in submit order (the census, "
           "MKW_QUEUE_WAITS=0); %llu episodes, %llu submissions held, %llu waits let go, %llu queues let on by the CPU, "
           "%.1f MiB of ops ordered", (unsigned long long)st_waits, (unsigned long long)st_unmet,
           (unsigned long long)st_episodes, (unsigned long long)st_held, (unsigned long long)st_forced,
           (unsigned long long)st_cpu, st_bytes / 1048576.0);
    st_waits = st_unmet = st_episodes = st_held = st_forced = st_cpu = st_bytes = 0;
}

int nvn_qorder_submit(const void *queue, const uint64_t *handles, int n, void **fe, uint64_t *pre, int *pre_waits) {
    int m = mode(), held = 0;
    *pre = 0, *pre_waits = 0;
    plat_mutex_lock(&qo);
    log_line();
    if (!RT_KNOB_ON(m)) {
        if (nslots) force();   // (the B block ended)
        if (nvn_sub_events || nvn_sub_waits) {
            uint64_t unmet0 = st_unmet;
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                follow(s.p, s.end, 0, queue, NULL);
            }
            if (st_unmet != unmet0) unmet_note();
        }
    } else if (!nslots && !nvn_sub_waits) {   // (nothing held and no wait: as it is; its signals are followed)
        if (nvn_sub_events)
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                follow(s.p, s.end, 1, queue, NULL);
            }
    } else {
        if (nslots == SLOTS_MAX || (!run_of(queue) && nruns == RUNS_MAX)) force();
        int was_open = nslots > 0, slot = nslots;
        if (!was_open) ep_parts = RT_KNOB_ON(parts_mode()), ep_pre = ep_parts && RT_KNOB_ON(pre_mode());
        ends[slot].ended = 0;
        Run *r = run_of(queue);
        if (r) {   // (behind its queue's held work)
            size_t bytes = 0;
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                bytes += (size_t)(s.end - s.p);
            }
            Piece *pc = piece_new(bytes, slot);
            for (int i = 0; i < n; i++) {
                Span s = handle_ops(handles[i]);
                piece_add(pc, s.p, s.end);
            }
            run_push(r, pc);
        } else {
            size_t at = out.n;
            run_new_submission(queue, handles, n, slot);
            schedule();
            if (!was_open && !nruns) st_bytes -= out.n - at, out.n = at, out.queue = NULL;   // (nothing waited: as it is)
        }
        held = was_open || nruns;
        if (held) {
            unmet_note();
            if (!was_open) st_episodes++;
            st_held++;
            FeSlot *s = calloc(1, sizeof *s);
            if (!s) rt_fatal("nvn: out of memory (held submission)");
            slots[nslots++] = s;
            *fe = s;
            // (an episode just opened: the stream holds this submission's ops before its wait and nothing else)
            if (!was_open && ep_pre && out.n) {
                *pre = nvn_stream_handle(out.b, out.n);
                for (size_t at = 0; at < out.n; at += op_at(out.b + at)->size) *pre_waits += op_at(out.b + at)->op == OP_WAIT_EVENT;
                cut = out.n, cut_queue = out.queue;   // (the parts go on from it, the queues' states too: cont)
            }
            give_parts();
            if (!nruns) close_episode();
        }
    }
    plat_mutex_unlock(&qo);
    return held;
}

void nvn_qorder_attach(void *fe, void *slot) {
    FeSlot *s = fe;
    plat_mutex_lock(&qo);
    s->slot = slot, s->attached = 1;
    if (s->decided) fill(s);
    plat_mutex_unlock(&qo);
}

void nvn_event_cpu_signal(volatile uint32_t *val) {   // the event's memory is its value again
    plat_mutex_lock(&qo);
    for (int k = 0; k < EV_MAX; k++)
        if (ev[k].val == val) ev[k].v = *val;
    if (nruns) {
        int before = nruns;
        schedule();
        st_cpu += (uint64_t)(before - nruns);
        if (!nruns) close_episode();
    }
    plat_mutex_unlock(&qo);
}
