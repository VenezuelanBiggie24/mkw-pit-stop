// Part of the kernel (kernel.c; split from it 06/11/2026, same sources and licences as its header).
// Synchronization objects and the thread and synchronization SVCs (kern_svc).
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "kernel_int.h"
#include "platform/platform.h"

// ---- synchronization objects
static int sig_obj(uint32_t h, int *bad) {   // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) {
        *bad = 1;
        return 0;
    }
    switch (g_h[i].type) {
    case O_THREAD: return ((RtThread *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((Event *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}

int kern_thread_state(uint32_t h) {
    lock();
    RtThread *t = h_peek(h, O_THREAD);
    int r = !t ? -1 : t->exited ? KERN_THREAD_EXITED : t->wkind;
    unlock();
    return r;
}

// CloseHandle: an event goes with its last end; a thread object once it has also exited.
static uint32_t handle_close(uint32_t h) {
    unsigned i = h - 0x1000;
    int ok = 0;
    void *tmem = NULL;
    lock();
    if (i > 0 && i < MAX_H) {
        int type = g_h[i].type;
        void *o = g_h[i].obj;
        if (type == O_EVENT_R || type == O_EVENT_W) {
            Event *e = o;
            if (--e->refs == 0) free(e);
            ok = 1;
        } else if (type == O_TMEM) {
            tmem = o;
            ok = 1;
        } else if (type == O_THREAD) {
            RtThread *t = o;
            t->closed = 1;
            if (t->exited) thr_free(t);
            ok = 1;
        }
        if (ok) {
            g_h[i].type = O_FREE;
            g_h[i].obj = NULL;
        }
    }
    unlock();
    free(tmem);
    return ok ? R_OK : R_BAD_HANDLE;
}

void kern_handle_close(uint32_t h) {
    if (handle_close(h)) rt_fatal("kern_handle_close(%#x): bad handle", h);
}

// Hooks of the probe's condvar negative control.
volatile int kern_test_cv_old_order;
void (*volatile kern_test_cv_hook)(void);

static void cv_hook(void) {
    void (*h)(void) = kern_test_cv_hook;
    if (h) h();
}

static int kern_svc_impl(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    if (g_diag > 0 && tcur) tcur->dg_svc = imm;
    switch (imm) {
    case 0x08: {   // CreateThread(-> w1 handle; x1 entry, x2 arg, x3 stack top, w4 priority, w5 core)
        int prio = (int)x[4], core = (int)x[5];
        if (!x[1] || !x[3] || prio < 28 || prio > 59 || core < -3 || core >= NCORE) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        RtThread *t = thr_new(prio, core >= 0 ? core : MAIN_CORE, core >= 0 ? 1ull << core : APP_MASK);
        t->entry = x[1];
        t->arg = x[2];
        t->stack_top = x[3] & ~15ull;
        lock();
        thr_link(t);
        unlock();
        if (!plat_thread_spawn(thread_main, t, "guest", HOST_STACK)) rt_fatal("kernel: cannot start a guest thread");
        if (rt_trace & RT_TRACE_SVC) {
            char d[128];
            rt_log("CreateThread %#x entry %s stack %#llx prio %d core %d", t->handle,
                   ldr_describe(t->entry, d, sizeof d), (unsigned long long)t->stack_top, prio, core);
        }
        x[0] = R_OK;
        x[1] = t->handle;
        return 1;
    }
    case 0x09: {   // StartThread(w0)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->started = 1;
            plat_cond_signal(&t->cv);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x0A: {   // ExitThread
        RtThread *t = cur(imm);
        if (t->fiber) {   // on a guest fiber: unwind from the thread's own context (kern_fiber_switch resumes it)
            t->exit_pending = 1;
            t->leaving = t->fiber;
            t->fiber = NULL;
            t->cur = t->cpu;
            plat_fiber_switch(t->base_fiber);
            rt_fatal("kernel: ExitThread on a fiber came back");
        }
        exit_now(t);
    }
    case 0x0B: {   // SleepThread(x0 ns); 0, -1, -2 yield
        int64_t ns = (int64_t)x[0];
        RtThread *t = cur(imm);
        if (ns > 0) {
            lock();
            core_put(t);
            acct(t, S_SLEEP);
            t->wobj = (uint64_t)ns;
            t->nblk++;
            unlock();
            uint64_t t0 = kern_now_ns();
            sleep_ns(t, ns);
            uint64_t took = kern_now_ns() - t0;
            lock();
            t->sl_n++;
            t->sl_req += (uint64_t)ns;
            t->sl_act += took;
            core_take(t);
            unlock();
            return 1;
        }
        // Yield: hand the core to a waiting thread of equal or better priority.
        // MKW_YIELD_NOLOCK (default 1, "ab" to compare): when no waiting thread may take this core (its bit is clear
        // in g_core_wait_mask, kept atomically), there is nothing to hand off and G is not taken. A race of 15/11/2026
        // made ~590 yields a frame (nn::os::YieldThread in the job system's spin loops) at 8.3 us each, 4.9 ms a frame;
        // a waiter that comes after the check is the same race as one after the locked scan below.
        static int nolock = -1;
        if (nolock < 0) nolock = rt_knob("YIELD_NOLOCK", "1");
        if (RT_KNOB_ON(nolock) && t->cur_core >= 0 &&
            !((__atomic_load_n(&g_core_wait_mask, __ATOMIC_ACQUIRE) >> t->cur_core) & 1u)) {
            plat_yield();
            return 1;
        }
        lock();
        int give = 0;
        int tp = prio_of(t);
        for (RtThread *w = g_core_wait; w && !give; w = w->cnext)
            give = t->cur_core >= 0 && core_allowed(w, t->cur_core) && prio_of(w) <= tp;
        if (give) {
            core_put(t);
            core_take(t);
        }
        unlock();
        if (!give) plat_yield();
        return 1;
    }
    case 0x0C: {   // GetThreadPriority(w1 handle -> w1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = (uint64_t)t->prio;
        unlock();
        return 1;
    }
    case 0x0D: {   // SetThreadPriority(w0 handle, w1 priority)
        if ((uint32_t)x[1] < 28 || (uint32_t)x[1] > 59) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x0E: {   // GetThreadCoreMask(w2 handle -> w1 core, x2 mask)
        lock();
        RtThread *t = h_get((uint32_t)x[2], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) {
            x[1] = (uint64_t)(uint32_t)t->core;
            x[2] = t->mask;
        }
        unlock();
        return 1;
    }
    case 0x0F: {   // SetThreadCoreMask(w0 handle, w1 ideal core, x2 mask): -1 don't care, -2 the process's, -3 no update
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        int core = (int)x[1];
        uint64_t mask = core == -2 ? 1ull << MAIN_CORE : x[2];   // -2: the process's ideal core, alone
        if (core == -2) core = MAIN_CORE;
        // the checks of Atmosphère's kern_svc_thread.cpp SetThreadCoreMask (-1/-3: svc_types_common.hpp)
        int keep = core == -3 && t ? t->core : core;   // no update: the ideal core stays and must be in the mask
        uint32_t bad = (mask & ~APP_MASK) || core < -3 || core >= NCORE ? R_INVALID_CORE
                       : !mask || (keep >= 0 && !(mask >> keep & 1)) ? R_INVALID_COMBINATION : R_OK;
        if (t && bad) {
            x[0] = bad;
            unlock();
            return 1;
        }
        if (t) {
            if (core >= 0) t->core = core;   // -1: no ideal core; ours stays a hint, core_take checks the mask
            t->mask = mask;
            if (t == tcur && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) {   // move off a forbidden core
                core_put(t);
                core_take(t);
            }
        }
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x10: {   // GetCurrentProcessorNumber: the virtual core held
        RtThread *t = cur(imm);
        if (t->cur_core < 0) {   // MKW_CORE_NUM_GATE: a core of its own while it uses the number
            lock();
            core_take_num(t);
            unlock();
        }
        x[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core);
        return 1;
    }
    case 0x36: {   // SynchronizePreemptionState: the SDK's spin locks call it after clearing TLR+0x100 (disable_count)
                   // when TLR+0x102 (interrupt_flag) is set; rt_svc's checkpoint has already honoured a pending preempt
        RtThread *t = cur(imm);
        volatile uint16_t *flag = (volatile uint16_t *)(t->tlr + 0x102);
        __atomic_fetch_add(&g_sync36, 1, __ATOMIC_RELAXED);
        if (*flag) __atomic_fetch_add(&g_sync36_flag, 1, __ATOMIC_RELAXED);
        *flag = 0;
        ap_sync(t);
        x[0] = R_OK;
        return 1;
    }
    case 0x25: {   // GetThreadId(w1 handle -> x1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = t->id;
        unlock();
        return 1;
    }

    case 0x1A: {   // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
        RtThread *t = cur(imm);
        uint64_t addr = x[1];
        uint32_t r = R_OK;
        lock();
        if (ld32(addr) == ((uint32_t)x[0] | WAITERS_BIT)) {
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            t->wobj = (uint32_t)x[0];
            r = block(t, W_LOCK, -1);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1B:   // ArbitrateUnlock(x0 address)
        lock();
        lock_handoff(x[0]);
        unlock();
        x[0] = R_OK;
        return 1;
    case 0x1C: {   // WaitProcessWideKeyAtomic(x0 lock, x1 key, w2 tag, x3 timeout)
        // The key is set before the lock is released. The SDK's Signal reads the key in user mode and skips svc 0x1D
        // when it is 0, and user mode takes the lock without G: releasing first lets another thread take the lock,
        // change the condition, read key 0 and wake nobody, after which this thread waits forever (MNG threads.c
        // 439-446). Linking into the wait list happens in block, still holding G, so svc 0x1D cannot come between.
        RtThread *t = cur(imm);
        uint64_t addr = x[0], key = x[1];
        lock();
        if (!kern_test_cv_old_order) {
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            lock_handoff(addr);
            cv_hook();
        } else {   // the probe's negative control: MNG's pre-fix order
            lock_handoff(addr);
            cv_hook();
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
        }
        t->wobj = key;
        uint32_t r = block(t, W_CONDVAR, (int64_t)x[3]);   // on timeout user mode takes the lock itself
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1D: {   // SignalProcessWideKey(x0 key, w1 count; <= 0 all)
        uint64_t key = x[0];
        int cnt = (int)x[1], n = 0;
        lock();
        for (RtThread *w; (cnt <= 0 || n < cnt) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0;
        for (RtThread *w = g_wait; w && !left; w = w->next) left = w->wkind == W_CONDVAR && w->wkey == key;
        if (!left) st32(key, 0);
        unlock();
        x[0] = R_OK;
        return 1;
    }

    case 0x34: {   // WaitForAddress(x0 address, w1 type, x2 value, x3 timeout): 0 if less, 1 decrement if less, 2 if
                   // equal, 3 if the 64-bit word is equal (nn::os::WaitForAddress(long const*, long))
        RtThread *t = cur(imm);
        uint64_t addr = x[0];
        int type = (int)x[1];
        int32_t val = (int32_t)x[2];
        if (addr & (type == 3 ? 7 : 3)) {   // Atmosphère's kern_svc_address_arbiter.cpp checks this first
            x[0] = KERN_R_INVALID_ADDRESS;
            return 1;
        }
        if (type < 0 || type > 3) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        AddrQueue q = addr_queue(addr);
        plat_mutex_lock(q.m);
        uint32_t r, v = ld32(addr);
        int ok = type == 3 ? ld64(addr) == x[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1)
            while (!cas32(addr, &v, v - 1))
                if (!((int32_t)v < val)) {
                    ok = 0;
                    break;
                }
        if (!ok) {
            plat_mutex_unlock(q.m);
            r = R_BAD_STATE;
        } else {
            t->waddr = addr;
            t->wobj = addr;
            r = addr_block(t, q, (int64_t)x[3]);
        }
        x[0] = r;
        return 1;
    }
    case 0x35: {   // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal,
                   // 2 if equal modify by the number of waiters
        uint64_t addr = x[0];
        int type = (int)x[1], cnt = (int)x[3];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        AddrQueue q = addr_queue(addr);
        plat_mutex_lock(q.m);
        int nw = 0;
        for (RtThread *w = *q.list; w; w = w->next) nw += w->wkind == W_ADDRESS && w->waddr == addr;
        uint32_t r = R_OK;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        int retake = 0;
        if (!r)
            for (int n = 0; cnt <= 0 || n < cnt; n++) {
                RtThread *w = pick_in(*q.list, W_ADDRESS, addr);
                if (!w) break;
                wake_addr_prepare(q.list, w, 0);
                if (!w->w_nosig && q.m != &G && core_handoff_wake(w, retake ? NULL : tcur)) retake = 1;
                wake_addr_commit(w);
            }
        plat_mutex_unlock(q.m);
        if (retake && tcur) {
            lock();
            core_take(tcur);
            unlock();
        }
        x[0] = r;
        return 1;
    }

    case 0x45: {   // CreateEvent(-> w1 writable, w2 readable)
        Event *e = calloc(1, sizeof *e);
        if (!e) rt_fatal("kernel: event allocation");
        e->refs = 2;
        lock();
        uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e);
        unlock();
        x[0] = R_OK;
        x[1] = hw;
        x[2] = hr;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   // SignalEvent(writable), ClearEvent(either end), ResetSignal(readable)
        uint32_t h = (uint32_t)x[0];
        uint32_t r = R_OK;
        lock();
        Event *e = imm == 0x11 ? h_peek(h, O_EVENT_W) : imm == 0x17 ? h_peek(h, O_EVENT_R)
                 : h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R);
        if (!e) {
            r = imm == 0x17 && h_peek(h, O_THREAD) ? R_BAD_STATE : R_BAD_HANDLE;
            if (r == R_BAD_HANDLE) rt_log("kernel: event svc %#x: invalid handle %#x", imm, h);
        } else if (imm == 0x11) {
            e->signaled = 1;
            wake_sync_waiters();
        } else if (imm == 0x17 && !e->signaled) {
            r = R_BAD_STATE;
        } else {
            e->signaled = 0;
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x18: {   // WaitSynchronization(-> w1 index; x1 handles, w2 count, x3 timeout)
        RtThread *t = cur(imm);
        const uint32_t *hs = (const uint32_t *)(uintptr_t)x[1];
        int n = (int)x[2];
        int64_t ns = (int64_t)x[3];
        if (n < 0 || n > 64 || (!hs && n)) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        uint64_t start = kern_now_ns();
        uint32_t r;
        int idx = -1, core_released = 0;
        lock();
        g_sync_calls++;
        for (;;) {
            if (t->cancel) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++)
                if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                r = R_BAD_HANDLE;
                break;
            }
            if (idx >= 0) {
                r = R_OK;
                break;
            }
            int64_t left = ns;
            if (ns > 0) {
                uint64_t spent = kern_now_ns() - start;
                if (spent >= (uint64_t)ns) {
                    r = R_TIMEOUT;
                    break;
                }
                left = ns - (int64_t)spent;
            }
            if (left == 0) {
                r = R_TIMEOUT;
                break;
            }
            if (!core_released) {
                core_put(t);
                acct(t, S_SYNC);
                t->nblk++;
                core_released = 1;
            }
            t->wobj = n ? hs[0] | (uint64_t)n << 32 : 0;
            t->waker = 0;
            g_sync_blocks++;
            if (block_inner(t, W_SYNC, left) == R_TIMEOUT) {
                r = R_TIMEOUT;
                break;
            }
        }
        if (core_released) core_take(t);
        unlock();
        x[0] = r;
        x[1] = idx >= 0 ? (uint64_t)idx : 0;
        return 1;
    }
    case 0x19: {   // CancelSynchronization(w0 thread)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->cancel = 1;
            if (t->wkind == W_SYNC) wake(t, (uint32_t)-1);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x16:   // CloseHandle(w0)
        x[0] = handle_close((uint32_t)x[0]);
        return 1;
    }
    return 0;
}

// MKW_SVC_STATS=1 (with MKW_TRACE=perf): the kernel's SVCs by number, every 600 frames: calls a frame and host time,
// split by whether the call blocked (its thread's nblk moved: a wait or a sleep, whose time includes the wait) or not
// (the runtime's own cost of the call). For the 60 fps work (15/11/2026): yields (0x0B), WaitForAddress (0x34),
// SignalToAddress (0x35) and ArbitrateLock/Unlock (0x1A/0x1B) held ~16-20 ms a frame of the game threads' time.
static int svc_stats = -1;
int g_svc_stats;
static struct { uint64_t n, ns, nb, nsb; } sst[0x80];
// Address waits (addr_block): how long they last, and the wake latency, from the waker's signal to the thread holding
// its core again (the delay a job's successor sees). Bins in us: <10 <20 <50 <100 <200 <500 <1000 <5000 >=5000.
#define WB 9
static const uint64_t wb_lim[WB - 1] = {10, 20, 50, 100, 200, 500, 1000, 5000};
static uint64_t wh_wait[WB], wh_wake[WB], wake_sum_ns, wake_n;
static int wbin(uint64_t ns) {
    int b = 0;
    while (b < WB - 1 && ns / 1000 >= wb_lim[b]) b++;
    return b;
}
// Slow wakes (200 us or more) by thread: how many, their total, and the part before the thread left its wait (the OS
// wake: signal to wait_woken's return) against the rest (taking its virtual core back, core_take).
#define SW_N 32
static struct { uint32_t h; uint64_t n, ns, os; } sw[SW_N];
static PlatMutex sw_lock = PLAT_MUTEX_INIT;
void kern_wait_note(uint64_t wait_ns, uint64_t wake_ns, uint64_t os_ns, uint32_t handle) {
    if (wake_ns >= 200000) {
        plat_mutex_lock(&sw_lock);
        int i = 0;
        while (i < SW_N && sw[i].h && sw[i].h != handle) i++;
        if (i < SW_N) sw[i].h = handle, sw[i].n++, sw[i].ns += wake_ns, sw[i].os += os_ns < wake_ns ? os_ns : wake_ns;
        plat_mutex_unlock(&sw_lock);
    }
    __atomic_fetch_add(&wh_wait[wbin(wait_ns)], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wh_wake[wbin(wake_ns)], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wake_sum_ns, wake_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wake_n, 1, __ATOMIC_RELAXED);
}

int kern_svc(Cpu *c, uint32_t imm) {
    if (svc_stats < 0) {
        const char *e = cfg_get("MKW_SVC_STATS");
        svc_stats = e && atoi(e) > 0;
        g_svc_stats = svc_stats;
    }
    if (!svc_stats || imm >= 0x80) return kern_svc_impl(c, imm);
    RtThread *t = tcur;
    uint64_t b0 = t ? __atomic_load_n(&t->nblk, __ATOMIC_RELAXED) : 0, t0 = kern_now_ns();
    int r = kern_svc_impl(c, imm);
    uint64_t d = kern_now_ns() - t0;
    if (r) {
        if (t && __atomic_load_n(&t->nblk, __ATOMIC_RELAXED) != b0)
            __atomic_fetch_add(&sst[imm].nb, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&sst[imm].nsb, d, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&sst[imm].n, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&sst[imm].ns, d, __ATOMIC_RELAXED);
    }
    return r;
}

void kern_svc_stats_report(uint64_t frame, uint64_t frames) {
    if (svc_stats <= 0 || !frames) return;
    struct { uint32_t imm; uint64_t n, ns, nb, nsb; } row[0x80];
    int nr = 0;
    for (uint32_t i = 0; i < 0x80; i++) {
        uint64_t n = __atomic_exchange_n(&sst[i].n, 0, __ATOMIC_RELAXED), ns = __atomic_exchange_n(&sst[i].ns, 0, __ATOMIC_RELAXED);
        uint64_t nb = __atomic_exchange_n(&sst[i].nb, 0, __ATOMIC_RELAXED), nsb = __atomic_exchange_n(&sst[i].nsb, 0, __ATOMIC_RELAXED);
        if (n || nb) row[nr].imm = i, row[nr].n = n, row[nr].ns = ns, row[nr].nb = nb, row[nr].nsb = nsb, nr++;
    }
    for (int i = 1; i < nr; i++)   // by the non-blocking time, the runtime's own cost
        for (int j = i; j > 0 && row[j].ns > row[j - 1].ns; j--) { __typeof__(row[0]) x = row[j]; row[j] = row[j - 1]; row[j - 1] = x; }
    rt_log("kernel: frame %llu: SVCs over %llu frames (calls/frame, host time/frame and per call; blocking ones apart):",
           (unsigned long long)frame, (unsigned long long)frames);
    for (int i = 0; i < nr && i < 12; i++)
        rt_log("kernel:   svc %#04x: %8.1f calls/f %7.3f ms/f (%5.2f us each) | blocked %6.1f calls/f %7.3f ms/f",
               row[i].imm, (double)row[i].n / frames, row[i].ns / 1e6 / frames, row[i].n ? row[i].ns / 1e3 / row[i].n : 0.0,
               (double)row[i].nb / frames, row[i].nsb / 1e6 / frames);
    uint64_t hw[WB], hk[WB], sum = __atomic_exchange_n(&wake_sum_ns, 0, __ATOMIC_RELAXED),
             n = __atomic_exchange_n(&wake_n, 0, __ATOMIC_RELAXED);
    char a[200], b[200];
    int oa = 0, ob = 0;
    for (int i = 0; i < WB; i++) {
        hw[i] = __atomic_exchange_n(&wh_wait[i], 0, __ATOMIC_RELAXED);
        hk[i] = __atomic_exchange_n(&wh_wake[i], 0, __ATOMIC_RELAXED);
        oa += snprintf(a + oa, sizeof a - (size_t)oa, " %.1f", (double)hw[i] / frames);
        ob += snprintf(b + ob, sizeof b - (size_t)ob, " %.1f", (double)hk[i] / frames);
    }
    if (n)
        rt_log("kernel:   address waits a frame by length, us <10 <20 <50 <100 <200 <500 <1000 <5000 more:%s; wake to "
               "running a frame by latency, same bins:%s; mean wake latency %.1f us over %.1f wakes a frame",
               a, b, sum / 1e3 / n, (double)n / frames);
    plat_mutex_lock(&sw_lock);
    for (int i = 1; i < SW_N && sw[i].h; i++)   // by total, longest first
        for (int j = i; j > 0 && sw[j].ns > sw[j - 1].ns; j--) { __typeof__(sw[0]) x = sw[j]; sw[j] = sw[j - 1]; sw[j - 1] = x; }
    for (int i = 0; i < 8 && i < SW_N && sw[i].h; i++)
        rt_log("kernel:   slow wakes (>= 200 us) of %#x: %.1f a frame, %.3f ms/f, %.0f%% before leaving the wait (OS), the "
               "rest taking the core back", sw[i].h, (double)sw[i].n / frames, sw[i].ns / 1e6 / frames,
               sw[i].ns ? 100.0 * sw[i].os / sw[i].ns : 0.0);
    memset(sw, 0, sizeof sw);
    plat_mutex_unlock(&sw_lock);
}
