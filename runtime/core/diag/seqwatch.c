// Sequence watch (MKW_SEQ_WATCH=1): which of MKW's global sequence nodes (AIGlobalSeq*: TitleWait, TitleMenuMain,
// PartsSelectEx, WaitIntroMovie, WaitRaceStart... the game's own phases) are active, so screens are found by the game's
// state instead of frame numbers (Adrià, 01/11/2026: scripts by frame skipped the cup movie or stalled on a screen).
//
// Every node class has a small virtual returning its name; build/seq_nodes.tsv for 1.0.0 and build/seq_nodes-180.tsv
// for 1.8.0 (tools/analisis/bin/seq_nodes.py; the same vtable layout in both, compared with --shared on 13/11/2026) list
// each getter and the vtable slot holding it. Measured with every slot watched (live28, 01/11/2026): slot +10 from the
// name runs once when a node starts (TitleMenuMain at frame 466) and +12 once when it ends (TitleWait at 465); +11
// runs while it is active, +9/+13 when the graph is built, +17 every frame for every node. A node that finishes on
// its own may skip +12 (WaitIntroMovie at the movie's end, live29): a node whose +11 has not run for STALE presents
// is taken as ended (seqwatch_frame). The function-entry
// hook of the lifted code (FN_ENTER, g_fnprof) calls fnprof_hit for every guest call; for the +10/+12 methods x0 is
// the node, whose vtable names it. Costs a hash probe per guest call while on.
//
//   log           "seq: enter <node> frame N" / "seq: leave <node> frame N"
//   ctl seq       the active nodes (ctl.c)
//   MKW_SEQ_HOLD=<node>[,<node>...]   no input script presses from the node's start until it ends or a movie
//                 ends (seqwatch_movie_end, ov_codec.c): WaitIntroMovie keeps updating through the race (live30),
//                 so the cup movie's end of stream is what releases it
//   MKW_SEQ_UNTIL=<node>[,<node>...]   the watch goes off at the last of these events, each after the one before
//                 (<node>: its start, -<node>: its end): what follows is measured without the hook's cost. The race:
//                 -WaitIntroMovie,IsSequenceFlag (rt-seq and live31-35: IsSequenceFlag also runs in the menus, and
//                 ~280 frames after the cup movie when the race starts)
//   MKW_SEQ_EXIT=<s>       with MKW_SEQ_UNTIL: the run ends s seconds after the node started, with the MKW_HOST_TIME
//                 census of just that window (Adrià, 01/11/2026: 20 s of race are enough; runs waited minutes past it)
//   MKW_SEQ_CTL=<n>:<cmd>[;<n>:<cmd>...]   with MKW_SEQ_UNTIL: the ctl command (ctl.c: shot, dump...) n presents after
//                 its event, counted here: at.sh reads the log and lagged up to 10 frames, so a fault of five frames
//                 (the white square of the countdown, sq1-sq4, 06/11/2026) was missed again and again
//   MKW_FN_WATCH=main+<hex>[,...]   with MKW_SEQ_WATCH: "fn: main+X obj O vt main+V from main+L frame N" each time
//                 one of these functions runs on another object than its previous call, so a menu cursor's move is one
//                 line (menus driven by the log, not by screenshots). The watch goes off with
//                 MKW_SEQ_UNTIL's event, as the nodes'. Each function logs FN_LINES lines at most.
//   MKW_PROBE=<node>+<n>[:<frames>][,...]   with MKW_SEQ_WATCH: seqwatch_probe() is true for <frames> presents
//                 (default 2) from n presents after each start of <node> (<node> "until": MKW_SEQ_UNTIL's event, after
//                 which the nodes go unwatched). A probe logs every call while it is true, without deduplicating: uw18
//                 and uw19 (11/11/2026) grouped the 2D draws by a partial key and lost the panes they were after, and
//                 uw20 counted its own window from the wrong movie
//   MKW_UI_FOCUS=1 with MKW_SEQ_WATCH: "ui: focus <pane path> frame N" when a menu button takes the cursor: the game
//                 calls main+0xce8564 (slot 18 of the UIControl button vtables) on the button that takes it and
//                 main+0x21e12f8 on the one that leaves (tt1: one pair per DOWN of the script). The button object
//                 holds its pane's name ("L_Chara_00" at +0x21c, "L_CharaSheet_00/L_Chara_00" further on): the line
//                 gives the path with a '/' if there is one, else the first name
//   MKW_LIST_WATCH=1   main+0x154e14's list (listwatch_init below): its length, flags and calls, every 600 frames
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap.h"
#include "cfg.h"
#include "gamelib.h"
#include "host.h"
#include "input.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "version.h"

#define MAIN_SPAN 0x10000000ull
#define MAX_NODES 512
#define HSIZE 2048   // power of two, > 2 * watched functions
#define SLOT_ENTER 10
#define SLOT_UPDATE 11
#define SLOT_LEAVE 12
#define SLOT_FN 20   // MKW_FN_WATCH
#define SLOT_FOCUS 21   // MKW_UI_FOCUS
#define UI_NAME_SPAN 0x1000    // bytes of the button object searched for its pane name
#define FN_MAX 32
#define STALE 3

typedef struct { uint64_t slot; char name[48]; } Node;   // slot: guest address of the vtable slot with the getter
static Node nodes[MAX_NODES];
static int nnodes;
static uint64_t haddr[HSIZE];   // watched function -> SLOT_ENTER or SLOT_LEAVE (a function serves one of the two)
static int8_t hval[HSIZE];
static volatile uint8_t active[MAX_NODES];
static volatile uint64_t last_update[MAX_NODES];   // present count at the node's latest +11 or +10
static volatile uint8_t held[MAX_NODES];   // MKW_SEQ_HOLD
static volatile int32_t nheld_active;
static volatile int32_t movie_ended;      // since the latest start of a held node
static volatile int seq_on;                // the watch is on (g_fnprof is shared with the import census)
static volatile int listw_on;              // MKW_LIST_WATCH (shares g_fnprof too)
static int64_t name_off = -1;              // name slot - vtable pointer, the same in every node class
#define UNTIL_MAX 4
static int until_node[UNTIL_MAX];          // MKW_SEQ_UNTIL: the chain of events, in order
static uint8_t until_on[UNTIL_MAX];        // 1: the node's start, 0: its end (-<node>)
static int nuntil, until_at;               // until_at: the next event of the chain (under set_active's lock)
static uint64_t exit_ns;                   // MKW_SEQ_EXIT, in ns
static volatile uint64_t window_end;       // when the run ends (kern_now_ns), 0 before MKW_SEQ_UNTIL's node started
#define SEQ_CTL_MAX 128
static struct { uint64_t n; char cmd[200]; } seq_ctl[SEQ_CTL_MAX];   // MKW_SEQ_CTL
static int nseq_ctl, seq_ctl_at;
static volatile uint64_t window_frame;     // the present count at MKW_SEQ_UNTIL's event + 1, 0 before
static volatile uint64_t window_t0;        // kern_now_ns at MKW_SEQ_UNTIL's event, 0 before: the A/B world clock
#define PROBE_MAX 8
#define PROBE_UNTIL -2                     // MKW_PROBE's node "until"
static struct { int node; uint64_t n, frames; volatile uint64_t from; } probes[PROBE_MAX];   // from: first present
static int nprobes;

#define FN_LINES 64   // lines per watched function: one called every frame on many objects goes quiet after them
static struct { uint64_t addr, obj; int lines; } fnw[FN_MAX];   // MKW_FN_WATCH, its previous call's object
static int nfnw;

static uint32_t h(uint64_t a) { return (uint32_t)((a >> 2) * 0x9E3779B1u) & (HSIZE - 1); }

static void watch(uint64_t fn, int slot) {
    if (fn < RT_MODULE_BASE || fn >= RT_MODULE_BASE + MAIN_SPAN) return;
    uint32_t i = h(fn);
    while (haddr[i] && haddr[i] != fn) i = (i + 1) & (HSIZE - 1);
    if (!haddr[i]) haddr[i] = fn, hval[i] = (int8_t)slot;
}

static int lookup(uint64_t fn) {
    for (uint32_t i = h(fn); haddr[i]; i = (i + 1) & (HSIZE - 1))
        if (haddr[i] == fn) return hval[i];
    return 0;
}

static int node_index(const char *name) {
    for (int k = 0; k < nnodes; k++)
        if (!strcmp(nodes[k].name, name)) return k;
    return -1;
}

void seqwatch_init(void) {
    // MKW_QUICK_RACE's presses wait for phases (input.c): the watch goes on for it and off where it arrives
    const char *on = cfg_get("MKW_SEQ_WATCH"), *quick = cfg_value("MKW_QUICK_RACE");
    int quick_on = strcmp(quick, "off") != 0;
    if ((!on || !*on || *on == '0') && !quick_on) return;
    // Each version's own table: another's offsets would watch unrelated code (1.8.0 had none, so MKW_SEQ_EXIT
    // never ended its runs; 13/11/2026)
    char own[48];
    snprintf(own, sizeof own, "build/seq_nodes-%d.tsv", rt_version()->number);
    const char *path = cfg_get("MKW_SEQ_NODES");
    if (!path || !*path) path = rt_version()->number == 100 ? "build/seq_nodes.tsv" : own;
    FILE *f = fopen(path, "r");
#ifdef __PROSPERO__
    char app[64];
    if (!f) {   // the title's own copy (tools/ps5/build_game.py puts the version's table in /app0)
        snprintf(app, sizeof app, "/app0/%s", strrchr(rt_version()->number == 100 ? "/seq_nodes.tsv" : own, '/') + 1);
        path = app;
        f = fopen(path, "r");
    }
#endif
    if (!f) { rt_log("seq: no %s (uv run tools/analisis/bin/seq_nodes.py): watch off", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f) && nnodes < MAX_NODES) {
        unsigned long long getter, slot;
        char name[48];
        if (line[0] == '#' || sscanf(line, "%llx %llx %47s", &getter, &slot, name) != 3) continue;
        Node *n = &nodes[nnodes++];
        n->slot = RT_MODULE_BASE + slot;
        snprintf(n->name, sizeof n->name, "%s", name);
    }
    fclose(f);
    for (int k = 0; k < nnodes; k++) {   // vtables are relocated in guest memory by now (ldr_load)
        watch(LD64(nodes[k].slot + 8u * SLOT_ENTER), SLOT_ENTER);
        watch(LD64(nodes[k].slot + 8u * SLOT_LEAVE), SLOT_LEAVE);
        watch(LD64(nodes[k].slot + 8u * SLOT_UPDATE), SLOT_UPDATE);
    }
    const char *hold = cfg_get("MKW_SEQ_HOLD");
    for (const char *p = hold; p && *p;) {
        size_t n = strcspn(p, ",");
        char name[48];
        snprintf(name, sizeof name, "%.*s", (int)n, p);
        int k = node_index(name);
        if (k < 0) rt_log("seq: MKW_SEQ_HOLD: no node %s", name);
        else held[k] = 1;
        p += n + (p[n] == ',');
    }
    const char *until = cfg_get("MKW_SEQ_UNTIL"), *ex = cfg_get("MKW_SEQ_EXIT");
    // free roam starts from the title's scene with no load: IntoRace2 five frames after Plus (fr1)
    // a Grand Prix's first race: after the cup movie (race.sh), as ReadyRace is the time trial's
    if ((!until || !*until) && quick_on)
        until = !strcmp(quick, "free") ? "IntoRace2" : !strcmp(quick, "gp") ? "-WaitIntroMovie,IsSequenceFlag" : "ReadyRace";
    for (const char *p = until; p && *p && nuntil < UNTIL_MAX;) {
        size_t n = strcspn(p, ",");
        char name[48];
        int end = *p == '-';
        snprintf(name, sizeof name, "%.*s", (int)(n - (size_t)end), p + end);
        int k = node_index(name);
        if (k < 0) rt_log("seq: MKW_SEQ_UNTIL: no node %s", name);
        else until_node[nuntil] = k, until_on[nuntil++] = (uint8_t)!end;
        p += n + (p[n] == ',');
    }
    if (ex && *ex) exit_ns = (uint64_t)(atof(ex) * 1e9);
    for (const char *p = cfg_get("MKW_SEQ_CTL"); p && *p && nseq_ctl < SEQ_CTL_MAX;) {   // in order of n
        size_t n = strcspn(p, ";");
        char *colon;
        uint64_t at = strtoull(p, &colon, 10);
        if (*colon == ':' && colon < p + n) {
            seq_ctl[nseq_ctl].n = at;
            snprintf(seq_ctl[nseq_ctl++].cmd, sizeof seq_ctl[0].cmd, "%.*s", (int)(p + n - colon - 1), colon + 1);
        } else rt_log("seq: MKW_SEQ_CTL: %.*s is not <n>:<command>", (int)n, p);
        p += n + (p[n] == ';');
    }
    for (const char *p = cfg_get("MKW_FN_WATCH"); p && *p && nfnw < FN_MAX;) {
        size_t n = strcspn(p, ",");
        unsigned long long off;
        if (sscanf(p, "main+%llx", &off) == 1) watch(fnw[nfnw++].addr = RT_MODULE_BASE + off, SLOT_FN);
        else rt_log("seq: MKW_FN_WATCH: %.*s is not main+<hex>", (int)n, p);
        p += n + (p[n] == ',');
    }
    for (const char *p = cfg_get("MKW_PROBE"); p && *p && nprobes < PROBE_MAX;) {
        size_t n = strcspn(p, ",");
        char name[48];
        unsigned long long at = 0, frames = 2;
        int got = sscanf(p, "%47[^+,]+%llu:%llu", name, &at, &frames);
        int k = got >= 2 ? (!strcmp(name, "until") ? PROBE_UNTIL : node_index(name)) : -1;
        if (k == -1) rt_log("seq: MKW_PROBE: %.*s is not <node>+<n>[:<frames>]", (int)n, p);
        else probes[nprobes].node = k, probes[nprobes].n = at, probes[nprobes++].frames = frames;
        p += n + (p[n] == ',');
    }
    const char *focus = cfg_get("MKW_UI_FOCUS");
    if (focus && *focus && *focus != '0') {   // version.c: 1.0.0 main+0xce8564; not found yet for 1.8.0
        if (rt_version()->ui_focus_fn) watch(rt_main_addr(rt_version()->ui_focus_fn), SLOT_FOCUS);
        else rt_log("seq: MKW_UI_FOCUS: no button focus function for game version %s", rt_version()->display);
    }
    rt_log("seq: watching %d sequence nodes and %d functions (MKW_SEQ_WATCH, MKW_FN_WATCH)", nnodes, nfnw);
    seq_on = 1;
    rt_fnprof_set(1);
}

// The node whose vtable is v: the name slot sits at the same offset from v in every node class (0x90 in MKW 1.0.0).
static int node_of(uint64_t v) {
    if (v < RT_MODULE_BASE || v >= RT_MODULE_BASE + MAIN_SPAN || (v & 7)) return -1;
    for (int k = 0; k < nnodes; k++) {
        if (name_off >= 0 ? nodes[k].slot == v + (uint64_t)name_off : nodes[k].slot >= v && nodes[k].slot - v < 0x200) {
            if (name_off < 0) name_off = (int64_t)(nodes[k].slot - v);
            return k;
        }
    }
    return -1;
}

static void set_active(int k, uint8_t on, uint64_t frame, const char *why) {
    static PlatMutex lock = PLAT_MUTEX_INIT;   // the game thread and the present thread (seqwatch_frame)
    plat_mutex_lock(&lock);
    if (active[k] != on) {
        active[k] = on;
        if (held[k]) plat_add32(&nheld_active, on ? 1 : -1), movie_ended = 0;
        // the seconds since the guest started: how long a scene takes in real time (hacks: turbo, skipped movies)
        rt_log("seq: %s %s frame %llu at %.1f s%s", on ? "enter" : "leave", nodes[k].name, (unsigned long long)frame,
               (kern_now_ns() - rt_guest_start_ns) / 1e9, why);
        if (on) input_script_phase(nodes[k].name);   // MKW_INPUT_SCRIPT lines that wait for this phase
        for (int i = 0; i < nprobes; i++)
            if (on && probes[i].node == k) __atomic_store_n(&probes[i].from, frame + probes[i].n, __ATOMIC_RELEASE);
        if (until_at < nuntil && k == until_node[until_at] && on == until_on[until_at] && ++until_at == nuntil &&
            seq_on) {
            seq_on = 0;
            rt_fnprof_set(impcensus_on || g_fn_sampling || listw_on);   // the other users keep the hook on
            __atomic_store_n(&window_t0, kern_now_ns(), __ATOMIC_RELEASE);
            hostfn_time_window(frame);   // the census starts here
            __atomic_store_n(&window_frame, frame + 1, __ATOMIC_RELEASE);
            for (int i = 0; i < nprobes; i++)
                if (probes[i].node == PROBE_UNTIL) __atomic_store_n(&probes[i].from, frame + probes[i].n, __ATOMIC_RELEASE);
            if (exit_ns) window_end = kern_now_ns() + exit_ns;
            rt_log("seq: watch off at %s %s frame %llu (MKW_SEQ_UNTIL)%s", on ? "the start of" : "the end of", nodes[k].name, (unsigned long long)frame,
                   exit_ns ? "; the run ends after MKW_SEQ_EXIT" : "");
        }
    }
    plat_mutex_unlock(&lock);
}

static int pane_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/';
}

// The pane name a button object holds (MKW_UI_FOCUS): the first run of 4+ name characters with a '_' (pane names are
// "L_Btn_02"; a shorter rule took bytes of pointers, tt2), the one with a '/' (the path) when there is one.
static void ui_name(const char *o, char *out, size_t cap) {
    out[0] = 0;
    for (size_t i = 0; i < UI_NAME_SPAN;) {
        size_t n = 0;
        int slash = 0, under = 0;
        while (i + n < UI_NAME_SPAN && pane_char(o[i + n])) slash |= o[i + n] == '/', under |= o[i + n] == '_', n++;
        if (n >= 4 && under && (slash || !out[0])) {
            snprintf(out, cap, "%.*s", (int)n, o + i);
            if (slash) return;
        }
        i += n + 1;
    }
}

static void listw_hit(uint64_t addr, uint64_t x0);

#ifdef RT_FN_ENTER_THUNK
__attribute__((naked, used)) void fnprof_thunk(void) { __asm__(RT_FN_THUNK_BODY); }
#endif

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    if (g_fn_sampling) kern_fn_note(addr, lr);
    if (impcensus_on) impcensus_hit(addr, lr);
    if (listw_on) listw_hit(addr, x0);
    if (!seq_on) return;
    int slot = lookup(addr);
    if (!slot || x0 < 0x1000) return;
    if (slot == SLOT_FOCUS) {
        char name[96];
        ui_name((const char *)(uintptr_t)x0, name, sizeof name);
        rt_log("ui: focus %s frame %llu", name[0] ? name : "?",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
        return;
    }
    if (slot == SLOT_FN) {
        for (int i = 0; i < nfnw; i++) {
            if (fnw[i].addr != addr) continue;
            if (__atomic_exchange_n(&fnw[i].obj, x0, __ATOMIC_RELAXED) == x0) return;
            int line = __atomic_fetch_add(&fnw[i].lines, 1, __ATOMIC_RELAXED);
            if (line > FN_LINES) return;
            uint64_t v = LD64(x0), fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            rt_log("fn: main+%llx obj %#llx vt main+%llx from main+%llx frame %llu%s", (unsigned long long)(addr - RT_MODULE_BASE),
                   (unsigned long long)x0, (unsigned long long)(v - RT_MODULE_BASE), (unsigned long long)(lr - RT_MODULE_BASE),
                   (unsigned long long)fr, line == FN_LINES ? " (no more lines for it)" : "");
            return;
        }
        return;
    }
    int k = node_of(LD64(x0));
    if (k < 0) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint8_t on = slot != SLOT_LEAVE;
    if (on) last_update[k] = fr;
    if (active[k] == on) return;
    set_active(k, on, fr, slot == SLOT_UPDATE ? " (update)" : "");
}

int64_t seqwatch_since_us(void) {
    uint64_t t0 = __atomic_load_n(&window_t0, __ATOMIC_ACQUIRE);
    return t0 ? (int64_t)((kern_now_ns() - t0) / 1000) : -1;
}

int seqwatch_window(void) { return __atomic_load_n(&window_end, __ATOMIC_RELAXED) != 0; }

void seqwatch_frame(uint64_t frame) {
    uint64_t end = __atomic_load_n(&window_end, __ATOMIC_RELAXED);
    if (end && kern_now_ns() >= end && !cap_busy()) {   // a capture in progress ends first (its file would be cut)
        rt_log("seq: MKW_SEQ_EXIT: %.1f s after MKW_SEQ_UNTIL, frame %llu: exit", exit_ns / 1e9, (unsigned long long)frame);
        hostfn_time_window(frame);
        prof_write();
        fflush(stderr);
        plat_exit(0);
    }
    uint64_t w = __atomic_load_n(&window_frame, __ATOMIC_ACQUIRE);
    while (w && seq_ctl_at < nseq_ctl && frame >= w - 1 + seq_ctl[seq_ctl_at].n) {
        rt_log("seq: MKW_SEQ_CTL +%llu (frame %llu): %s", (unsigned long long)seq_ctl[seq_ctl_at].n,
               (unsigned long long)frame, seq_ctl[seq_ctl_at].cmd);
        ctl_run(seq_ctl[seq_ctl_at++].cmd);
    }
    for (int k = 0; k < nnodes; k++)
        if (active[k] && frame > last_update[k] + STALE) set_active(k, 0, frame, " (no update)");
}

int seqwatch_hold(void) {
    return __atomic_load_n(&nheld_active, __ATOMIC_RELAXED) > 0 && !__atomic_load_n(&movie_ended, __ATOMIC_RELAXED);
}

void seqwatch_movie_end(void) {
    if (__atomic_load_n(&nheld_active, __ATOMIC_RELAXED) > 0 && !__atomic_exchange_n(&movie_ended, 1, __ATOMIC_RELAXED))
        rt_log("seq: movie ended frame %llu: MKW_SEQ_HOLD released",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
}

int seqwatch_probe(void) {
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < nprobes; i++) {
        uint64_t from = __atomic_load_n(&probes[i].from, __ATOMIC_ACQUIRE);
        if (from && fr >= from && fr < from + probes[i].frames) return 1;
    }
    return 0;
}

void seqwatch_log_active(void) {
    char buf[1024];
    int n = 0;
    for (int k = 0; k < nnodes && n < (int)sizeof buf - 50; k++)
        if (active[k]) n += snprintf(buf + n, sizeof buf - (size_t)n, " %s", nodes[k].name);
    rt_log("ctl: seq%s", n ? buf : (nnodes ? " (none active)" : " (MKW_SEQ_WATCH off)"));
}

// ---- MKW_LIST_WATCH=1: main+0x154e14's list (task 36, 16/11/2026)
// main+0x154e14 (MKW 1.0.0) is the per-frame update of a pool of registrations that main+0x154b00's object holds at
// +0x140 (the owner, x0 of all three functions here; the caller also names CmnObj_Cactus000_A_00, FernsJG, MobStones).
// A node is 0x150 bytes: a mask of slots at +0x130, an id at +0x134, flags at +0x138 and the link at +0x140 (the next
// link at +0x148, the head at owner+0xd8, its first link at owner+0xe0). The owner's mutex is at +0xb8, the node count
// at +0xe8, the free nodes at +0xf0 and the capacity at +0x100.
//   main+0x847680 (from main+0x847614)  adds a node under the mutex while count < capacity: id from *x2, a mask bit for
//                                       each entry of x3 found in [[main+0x33b9788]+0xc8]'s table
//   main+0x1b16850 (from main+0x1b179f4, 8 callers)  a kill: one walk of the whole list, bit 0 on each node of that id
//   main+0x154e14  under the mutex: bit 1 -> the node is freed (a virtual call, +0x90, on owner+0x5898, then back to
//                  the free list); bit 0 -> bit 1; then a second walk ORs every node's mask into owner+0x5518
// Nothing there reads a fence, a counter or a timestamp: a node leaves the list two updates after a kill of its id.
// The entry hook (FN_ENTER) counts the three functions' calls by owner; this thread reads each owner's count every
// frame and walks its list every LW_WALK_EVERY frames, through plat_mem_read (no fault on memory freed with the owner,
// and a page the early-read watch protects keeps its watch: the read fails, counted as torn), without the game's
// mutex: a node freed during the walk has its link zeroed and ends the walk (torn). The game threads pay the hook only.
//   "list: frame N, owner A: update U/f, add a/f, kill k/f; count mean m max x (capacity c); in W walks: kill asked b0,
//    to free b1, empty mask z, torn t; node visits ~V/f (two walks an update, one a kill)"
#define LW_UPDATE 0x154e14
#define LW_ADD 0x847680
#define LW_KILL 0x1b16850
#define LW_OWNERS 8
#define LW_WALK_EVERY 10
#define LW_MAX_NODES (1u << 20)
static struct {
    uint64_t owner, update, add, kill;   // the hook writes these (atomics)
    uint64_t frames, count_sum, count_max, cap, walks, walked, b0, b1, zero, torn;   // the watch thread's
} lw[LW_OWNERS];

static void listw_hit(uint64_t addr, uint64_t x0) {
    uint64_t off = addr - RT_MODULE_BASE;
    if (off != LW_UPDATE && off != LW_ADD && off != LW_KILL) return;
    for (int j = 0; j < LW_OWNERS; j++) {
        uint64_t o = __atomic_load_n(&lw[j].owner, __ATOMIC_ACQUIRE);
        if (!o) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&lw[j].owner, &zero, x0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) o = zero;
            else o = x0;
        }
        if (o != x0) continue;
        __atomic_fetch_add(off == LW_UPDATE ? &lw[j].update : off == LW_ADD ? &lw[j].add : &lw[j].kill, 1,
                           __ATOMIC_RELAXED);
        return;
    }
}

static void listw_walk(int j, uint64_t o) {
    uint64_t link, sent = o + 0xd8;
    if (!plat_mem_read(o + 0xe0, &link, 8)) { lw[j].torn++; return; }
    uint32_t n = 0;
    while (link != sent && n < LW_MAX_NODES) {
        uint8_t b[0x20];   // node +0x130..+0x150: mask, id, flags, the link (prev, next)
        if (!link || (link & 7) || !plat_mem_read(link - 0x10, b, sizeof b)) { lw[j].torn++; break; }
        uint32_t mask;
        memcpy(&mask, b, 4);
        lw[j].b0 += b[8] & 1, lw[j].b1 += b[8] >> 1 & 1, lw[j].zero += !mask;
        memcpy(&link, b + 0x18, 8);
        n++;
    }
    lw[j].walks++, lw[j].walked += n;
}

static void listw_main(void *arg) {
    (void)arg;
    uint64_t last = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), next_log = (last / 600 + 1) * 600;
    for (;;) {
        plat_sleep_ns(2000000ull);
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f == last) continue;
        last = f;
        for (int j = 0; j < LW_OWNERS; j++) {
            uint64_t o = __atomic_load_n(&lw[j].owner, __ATOMIC_ACQUIRE);
            uint32_t cc[2];   // +0xe8 count; capacity at +0x100
            if (!o) continue;
            lw[j].frames++;
            if (!plat_mem_read(o + 0xe8, &cc[0], 4) || !plat_mem_read(o + 0x100, &cc[1], 4)) { lw[j].torn++; continue; }
            lw[j].count_sum += cc[0], lw[j].cap = cc[1];
            if (cc[0] > lw[j].count_max) lw[j].count_max = cc[0];
            if (f % LW_WALK_EVERY == 0) listw_walk(j, o);
        }
        if (f < next_log) continue;
        next_log = (f / 600 + 1) * 600;
        for (int j = 0; j < LW_OWNERS; j++) {
            uint64_t o = __atomic_load_n(&lw[j].owner, __ATOMIC_ACQUIRE);
            if (!o) continue;
            uint64_t u = __atomic_exchange_n(&lw[j].update, 0, __ATOMIC_RELAXED);
            uint64_t a = __atomic_exchange_n(&lw[j].add, 0, __ATOMIC_RELAXED);
            uint64_t k = __atomic_exchange_n(&lw[j].kill, 0, __ATOMIC_RELAXED);
            double fr = lw[j].frames ? (double)lw[j].frames : 1.0, mean = (double)lw[j].count_sum / fr;
            if (u || a || k)
                rt_log("list: frame %llu, owner %#llx: update %.2f/f, add %.2f/f, kill %.2f/f; count mean %.0f max %llu "
                       "(capacity %llu); in %llu walks of %.0f nodes: kill asked %llu, to free %llu, empty mask %llu, "
                       "torn %llu; node visits ~%.0f/f (two walks an update, one a kill)",
                       (unsigned long long)f, (unsigned long long)o, u / fr, a / fr, k / fr, mean,
                       (unsigned long long)lw[j].count_max, (unsigned long long)lw[j].cap,
                       (unsigned long long)lw[j].walks, lw[j].walks ? (double)lw[j].walked / (double)lw[j].walks : 0.0,
                       (unsigned long long)lw[j].b0, (unsigned long long)lw[j].b1, (unsigned long long)lw[j].zero,
                       (unsigned long long)lw[j].torn, (2.0 * (double)u + (double)k) / fr * mean);
            else   // no call in a whole window: the owner is gone (a course change), its slot free for the next one
                __atomic_store_n(&lw[j].owner, 0, __ATOMIC_RELEASE);
            lw[j].frames = lw[j].count_sum = lw[j].count_max = lw[j].walks = lw[j].walked = 0;
            lw[j].b0 = lw[j].b1 = lw[j].zero = lw[j].torn = 0;
        }
    }
}

void listwatch_init(void) {
    const char *on = cfg_get("MKW_LIST_WATCH");
    if (!on || !*on || *on == '0') return;
    if (!plat_thread_new(listw_main, NULL, "list watch")) {
        rt_log("list: MKW_LIST_WATCH: the watch thread did not start");
        return;
    }
    listw_on = 1;
    rt_fnprof_set(1);
    rt_log("list: MKW_LIST_WATCH: main+0x154e14's list (calls of main+%#x, +%#x, +%#x by owner; a walk every %d frames)",
           LW_UPDATE, LW_ADD, LW_KILL, LW_WALK_EVERY);
}
