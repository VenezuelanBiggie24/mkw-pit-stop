// The game clock the shared runtime reads (host.h frame_step_*), for Mario Kart World: counted from presents, and
// for the A/B blocks from the phase MKW_SEQ_UNTIL waits for. totk-pc-recomp's resolution.c and frame_rate.c (patch
// sites and fixed addresses of TotK) have no MKW counterpart.
//
// Frame rate (MKW_FPS, Screen tab): the game picks its present interval each frame in its frame object's per-frame
// virtual (1.0.0: main+0x3a9e28, column main+0x329fc70 slot 28; 1.8.0: main+0x69cbb0, column main+0x367a9e0 slot 28;
// version.c) from the time it measured for the frame, and its game clock (1.0.0: [main+0x33e6ae8]) advances by that
// interval: a frame at interval 1 is 1/60 s of game time. Its wait for the GPU
// counts only through the GPU timestamps (vk_exec_xfer.c MKW_REPORT_GPU_END): with the submit's time instead, a
// machine near 30 fps stayed at interval 1 and the race ran at half speed (Adrià 09/11/2026: "como si estuviera en
// modo 60 yendo a 30"). [obj+0x210] = 1 or 2 is the game's own fixed interval, which skips that choice; any other
// non-zero value skips it too and keeps the interval in [+0x214] as it is (main+0x3aa034), so 20 and 15 fps (3 and 4)
// need no patch: both words are written before each call. MKW_FPS: auto (the game's own choice while it holds 30 fps,
// fps_auto.c's 3 or 4 below that in a race; auto_want), 60, 30, 20, 15, or game (the game's own choice, 1-2: the
// console's, for A/B). MKW_DYN_FPS (Hacks tab, off) replaces MKW_FPS's choice with 1-3 steps a frame from how far the
// presents ran behind the game clock (dyn_want), over a pacer held to one period.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "vk_reflex_frame.h"
#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "version.h"

#define FRAME_FIXED 0x210                         // frame object: fixed interval, 0 = the game picks
#define FRAME_INTERVAL 0x214                      // frame object: the interval of this frame
#define OUT_OF_RACE_MAX 2                         // 30 fps: the slowest interval outside a race
#define FRAME_MEASURED 0x128                      // frame object: the time measured for the last frame (ticks)
#define FRAME_HOST 0xfa0                          // patch page offset of host_frame_update (patches.c)
#define SPEED_WINDOW_NS 5000000000ull

static _Atomic uint64_t ticks;   // game time since the first present, in 60 Hz ticks whatever MKW_LOGIC_HZ is
static uint32_t ticks_acc;       // 60 Hz ticks owed times the step rate, below it (the vsync thread alone writes it)
static int ticks_hz;

// MKW has no save-load point: the A/B blocks (MKW_AB, perf.c rt_ab) count from the phase MKW_SEQ_UNTIL waits for
// (the race's start in tools/run/race.sh), so both halves measure the same scene.
int64_t frame_step_world_us(void) { return seqwatch_since_us(); }

// Game time in 60 Hz ticks whatever MKW_LOGIC_HZ is: the input script, the movie skip and the codec count in them
// (lhz120, 19/11/2026: the script counted steps of 1/120 s and pressed A at half the game time, skipping the intro).
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&ticks, memory_order_relaxed); }

// MKW_LOGIC_HZ (the menu's Logic rate, 60 by default): the vsync thread ticks N times a second and each game step is 1/N s
// (patches.c set logichz), so the game logic runs N steps a second instead of 60. 60 when unset or out of 30..480, and
// when the game version has no logichz set (frame_logic_hz_60): ticks at N with steps of 1/60 would run it N/60 fast.
// Also 60 where the game counts its inputs in 1/60 s frames: with MKW_LAN on, the whole run (the LAN game keeps its
// consoles in step frame by frame), and in time trial (race_scene.c: the ghosts keep one input per frame).
// MKW_LOGIC_HZ_TT=1 (a test, not a setting) keeps N in time trial too: its timer is the only one the race shows, so
// it measures whether the race follows game time at N (ghosts saved then would not replay at 60).
static int hz, hz_tt;

void frame_logic_hz_60(void) { hz = 60; }

int frame_logic_hz(void) {
    if (!hz) {
        const char *v = cfg_get("MKW_LOGIC_HZ");
        int n = v ? atoi(v) : 60;
        hz = n >= 30 && n <= 480 ? n : 60;
        hz_tt = rt_knob("LOGIC_HZ_TT", "0");   // a test switch, not a menu setting: no row in the settings table
        if (hz != 60 && cfg_bool("MKW_LAN")) {
            rt_log("game: MKW_LOGIC_HZ=%d with MKW_LAN on: 60 Hz, as the LAN game counts in 1/60 s frames", hz);
            hz = 60;
        }
    }
    return hz != 60 && !hz_tt && race_scene_time_trial() ? 60 : hz;
}

// Game speed, for tools/run/summary.py: game seconds per real second over each window, and how often the interval
// changed. Below 1 the game runs in slow motion.
static struct { uint64_t t0; uint32_t clock0; unsigned frames, changes; int interval; } sp;

static void speed_note(int interval) {
    uint64_t now = kern_now_ns(), at = rt_main_addr(rt_version()->game_clock);
    if (!at) return;
    uint32_t clock = *(const volatile uint32_t *)(uintptr_t)at;
    if (!sp.t0) {
        sp.t0 = now, sp.clock0 = clock, sp.interval = interval;
        return;
    }
    sp.frames++;
    if (interval != sp.interval) sp.changes++, sp.interval = interval;
    if (now - sp.t0 < SPEED_WINDOW_NS) return;
    double real = (double)(now - sp.t0) / 1e9;
    rt_log("game: speed %.2fx over %.1f s (%u frames, %u interval changes, MKW_FPS %s)",
           (double)(clock - sp.clock0) / frame_logic_hz() / real, real, sp.frames, sp.changes, cfg_value("MKW_FPS"));
    sp.t0 = now, sp.clock0 = clock, sp.frames = sp.changes = 0;
}

static _Atomic int last_interval = 1;

int frame_present_interval(void) { return atomic_load_explicit(&last_interval, memory_order_relaxed); }

// MKW_DYN_FPS (Hacks tab, off by default, on in the PS5 profile; the idea of a 1.0.0 IPS mod shared on Reddit in 11/2026, which credits
// Eglantine's Dynamic FPS build: no code of either). Each frame takes 1/60 to 3/60 s of game time (in MKW_LOGIC_HZ
// ticks; 3/60 only in a race, 2/60 at least in a split screen MKW_SPLIT_FPS holds at 30), from how far the presents ran behind the game time they
// showed (fps_auto.c fps_dyn_*). The pacer meanwhile holds presents to the smallest of those intervals
// (nvn_api_obj.c present_period), not the frame's own: paced to its own interval, a frame at 3 would last exactly its
// 3 steps and the debt that chose it never come down. So a 45 fps machine shows 45 pictures a second at full game
// speed, in steps of 1 and 2 (motion less even than at a fixed rate: the post's own caveat); below 20 fps the race
// slows down. Off with frame generation (its factor follows a whole interval, auto_want) and with the turbo.
static _Atomic int dyn_floor;      // the pacer's interval while MKW_DYN_FPS picks the steps, 0 otherwise
static _Atomic int64_t dyn_acc;    // ns the presents ran behind the game time (fps_dyn_present), any thread reads
static uint64_t dyn_last;          // the present thread: the last present's time, 0 after a stop
uint64_t gfx_vsync_tick_ns(void);   // gfx_present.c: 1/MKW_LOGIC_HZ s, or locked to the display's refresh
static int64_t dyn_tick_ns(void) { return (int64_t)gfx_vsync_tick_ns(); }   // one step of game time

int frame_dyn_interval(void) { return atomic_load_explicit(&dyn_floor, memory_order_relaxed); }

static void dyn_present(uint64_t display_ns, int interval) {
    if (!atomic_load_explicit(&dyn_floor, memory_order_relaxed)) {
        dyn_last = 0;
        return;
    }
    // the picture's time: its slot, or now when the frame came after it (the pacer then presents at once)
    uint64_t now = kern_now_ns(), t = display_ns > now ? display_ns : now;
    if (dyn_last && t > dyn_last) {
        int64_t acc = atomic_load_explicit(&dyn_acc, memory_order_relaxed);
        fps_dyn_present(&acc, (int64_t)(t - dyn_last), interval > 0 ? interval : 1, dyn_tick_ns());
        atomic_store_explicit(&dyn_acc, acc, memory_order_relaxed);
    }
    dyn_last = t;
}

void frame_step_present(uint64_t display_ns, int interval) {
    dyn_present(display_ns, interval);
    atomic_store_explicit(&last_interval, interval > 0 ? interval : 1, memory_order_relaxed);
    // interval steps of 1/N s in 60 Hz ticks; N changes on entering or leaving time trial, so the remainder restarts
    uint32_t n = (uint32_t)frame_logic_hz();
    if ((int)n != ticks_hz) ticks_hz = (int)n, ticks_acc = 0;
    ticks_acc += 60u * (interval > 0 ? (uint32_t)interval : 1u);
    atomic_fetch_add_explicit(&ticks, ticks_acc / n, memory_order_relaxed);
    ticks_acc %= n;
    speed_note(interval);
}

// MKW_FRAME_TRACE=1 (diagnosis, juego/velocidad): one line per frame with what the game's interval choice reads. The
// frame object keeps [+0xf0] the tick its last frame ended, [+0x128] the measured time (the CPU up to after its update,
// raised by main+0x13e174 to the GPU timestamp of the frame's last counter report minus [+0xf0], read with
// nvnDeviceGetTimestampInNanoseconds), [+0xe8] the whole CPU frame, [+0x214] the interval and [+0x218] the accumulated
// lateness. Times in ms.
static void frame_trace(uint64_t obj, uint64_t t0) {
    static int on = -1;
    static uint64_t prev;
    if (on < 0) on = rt_knob("FRAME_TRACE", "0");
    if (!on) return;
    uint64_t now = kern_now_ns(), ts = nvn_timestamp_last;
    const uint8_t *f = (const uint8_t *)(uintptr_t)obj;
    uint64_t end = *(const uint64_t *)(f + 0xf0);
    double tk = RT_TICK_NS / 1e6;
    rt_log("frame: period %.2f call %.2f meas %.2f cpu %.2f ts %.2f iv %u acc %.2f", (now - prev) / 1e6,
           (now - t0) / 1e6, (double)*(const int64_t *)(f + 0x128) * tk, (double)*(const int64_t *)(f + 0xe8) * tk,
           ts ? ((double)ts - (double)end) * tk : 0.0,
           *(const uint32_t *)(f + 0x214), (double)*(const int64_t *)(f + 0x218) * tk);
    prev = now;
}

// The interval counts vsync ticks of MKW_LOGIC_HZ (n): MKW_FPS's rates are n/fps ticks (120 Hz: 30 fps is 4)
static int fixed_interval(const char *fps, int n) {
    static const struct { const char *name; int iv; } t[] = {{"60", 1}, {"30", 2}, {"20", 3}, {"15", 4}};
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(fps, t[i].name)) return (t[i].iv * n + 30) / 60;
    return 0;
}

// MKW_FPS=auto: the frame's interval from the time the game measured for the last one ([+0x128], ticks), logged
// when it changes with the median that moved it.
static int auto_interval(uint64_t obj) {
    static FpsAuto a;
    static int ready;
    static uint64_t last;   // when the last frame began: the real period, which the game's measure can fall short of
    int n = frame_logic_hz();   // its ticks: back to 60 in time trial starts it over
    if (!ready || a.hz != n) fps_auto_reset(&a), a.hz = n, ready = 1;
    uint64_t now = kern_now_ns();
    double ms = (double)*(const int64_t *)(uintptr_t)(obj + FRAME_MEASURED) * RT_TICK_NS / 1e6;
    double real = last ? (now - last) / 1e6 / turbo_speed() : 0;
    last = now;
    int prev = a.iv, iv = fps_auto_step(&a, ms, real);
    if (iv != prev) rt_log("game: auto interval %d -> %d (frame %.1f ms, real %.1f ms)", prev, iv, ms, real);
    return iv;
}

// MKW_FPS=auto's interval to write, 0 for the game's own choice. While the controller fits 30 fps (interval 1 or 2) the
// game picks 1 or 2 each frame from its own lateness, as on the console: a PC at 45-55 fps alternates them at 1.00x
// (v0.1.11, fl124a). A whole interval there held such a PC at 30, since 60 needs a 2 s median under 14.2 ms, and 60
// forced ran it in slow motion (#442, #446: auto at 2 on 20-25 ms frames; v0.1.12). Below 30 the game's choice stops
// at 2 and the race ran at 0.75-0.83x (Radeon 660M, 36 ms frames), so there the controller's 3 or 4 (only in a race).
// Frame generation keeps the controller's whole interval: its factor follows the game's interval (vk_framegen.c fits),
// which would change from one frame to the next.
// At MKW_LOGIC_HZ other than 60 the game's own choice is wrong: it compares its frame with 1/60 s, so a 12 ms frame
// got interval 1 at 120 Hz, 1/120 s of game time, and the game ran at half speed; the controller's interval always.
static int auto_want(uint64_t obj) {
    int iv = auto_interval(obj);
    if (strcmp(cfg_value("MKW_FRAMEGEN"), "off") || frame_logic_hz() != 60) return iv;
    return iv > OUT_OF_RACE_MAX && race_scene_on() ? iv : 0;
}

// MKW_DYN_FPS on and possible now: not with frame generation or the turbo (see dyn_floor), logged when that changes.
static int dyn_on(void) {
    static int was = -1;
    int want = cfg_bool("MKW_DYN_FPS"), fg = strcmp(cfg_value("MKW_FRAMEGEN"), "off") != 0, turbo = turbo_speed() > 1;
    int on = want && !fg && !turbo;
    if (on != was && (want || was == 1))
        rt_log("game: dynamic frame rate %s (MKW_DYN_FPS)", on ? "on: 1-3 steps a frame" :
               !want ? "off" : fg ? "waits: frame generation is on" : "waits: the turbo is on");
    was = on;
    if (!on) atomic_store_explicit(&dyn_acc, 0, memory_order_relaxed);
    return on;
}

// The steps of this frame in ticks of n Hz, 1 to 3/60 s (2/60 s at most outside a race: below, the cap applies as to
// MKW_FPS's 3 and 4).
static int dyn_want(int n) {
    return fps_dyn_interval(atomic_load_explicit(&dyn_acc, memory_order_relaxed), dyn_tick_ns(), 1, 3 * n / 60);
}

// The game thread, once per frame: MKW_FPS into the fixed interval, then the game's own function. saved: the game's
// value before the first forced frame, put back with MKW_FPS=game.
static void host_frame_update(Cpu *c) {
    static int saved = -1;
    uint32_t *fixed = (uint32_t *)(uintptr_t)(c->x[0] + FRAME_FIXED);
    const char *fps = cfg_value("MKW_FPS");
    // A full-screen movie (a cup opening) goes at the game's own choice, 60 or 30 as on the console, whatever MKW_FPS
    // says: at 15 it played at 15 and its skip (core/hacks/skip.c, presses counted in 60 Hz ticks) never landed
    // (gp15a, vis2: "no end of the movie after 600 ticks"; Adrià 15/11/2026: "el vídeo no se salta y se ve laggeado")
    // A cup opening goes at 1 from its file's open until its decoder closes (core/hacks/skip.c): opened at 2 or 4 it
    // was not skipped in 7 of 9 runs, at 1 in none of 13 (dn2), and the movie itself shows its pictures at their rate
    int movie = ov_movie_playing(), opening = skip_opening_playing();
    static int was_movie, was_opening;
    if (movie != was_movie) rt_log("game: %s", movie ? "a movie plays: the game picks the interval" : "movie over");
    if (opening != was_opening) rt_log("game: %s", opening ? "a cup opening: interval 1" : "cup opening over");
    was_movie = movie, was_opening = opening;
    // At n Hz a cup opening goes at n / 60 (the console's 1), and a movie or MKW_FPS=game at auto's (auto_want)
    int n = frame_logic_hz();
    int dyn = !opening && !movie && dyn_on();
    int want = opening                       ? (n + 59) / 60
             : dyn                           ? dyn_want(n)
             : movie || !strcmp(fps, "game") ? (n != 60 ? auto_want(c->x[0]) : 0)
             : !strcmp(fps, "auto")          ? auto_want(c->x[0])
                                             : fixed_interval(fps, n);
    // MKW_SPLIT_FPS=30 (Hacks tab, off): split screen at 30 at most, whatever MKW_FPS says, outside the movies above.
    // At 60 the game counts each frame as 1/60 s, and on a base PS5 split frames take longer (22/11/2026, Grand Prix
    // benchmark at 60, one build): 1 player 17.1 ms (0.98x), 2 players 21.7 ms (0.77x), 3 and 4 41.9 ms (0.40x); at 30
    // a frame has 33.3 ms at full speed.
    static int was_split;
    int split = !opening && !movie && ov_split_players() > 1 && !strcmp(cfg_value("MKW_SPLIT_FPS"), "30");
    if (split != was_split)
        rt_log("game: %s", split ? "split screen: 30 fps at most (MKW_SPLIT_FPS)" : "split screen 30 fps cap off");
    was_split = split;
    if (split && want < 2 * n / 60) want = 2 * n / 60;   // 30 fps in MKW_LOGIC_HZ ticks
    atomic_store_explicit(&dyn_floor, dyn ? (split ? 2 * n / 60 : 1) : 0, memory_order_relaxed);   // the pacer's
    // 20 and 15 only in a race (race_scene.c): menus and loads go at 30 at most, as on the console (Adrià 15/11/2026);
    // a menu counting in 1/60 s steps risks at 3 or 4 what the cup opening showed
    static int was_capped;
    int cap = OUT_OF_RACE_MAX * n / 60, capped = want > cap && !race_scene_on();
    if (capped != was_capped)
        rt_log("game: interval %d %s %d (out of a race)", want, capped ? "capped at" : "no longer capped at", cap);
    was_capped = capped;
    if (capped) want = cap;
    if (want) {
        if (saved < 0) {
            saved = (int)*fixed;
            rt_log("game: present interval fixed at %d (MKW_FPS %s; the game's value was %d)", want,
                   dyn ? "set aside by MKW_DYN_FPS" : fps, saved);
        }
        *fixed = (uint32_t)want;
        *(uint32_t *)(uintptr_t)(c->x[0] + FRAME_INTERVAL) = (uint32_t)want;   // 3 and 4: the game leaves it
    } else if (saved >= 0) {
        *fixed = (uint32_t)saved;
        saved = -1;
        rt_log("game: present interval chosen by the game again (MKW_FPS %s)", fps);
    }
    uint64_t obj = c->x[0];
    vk_rfx_frame_start(vk_be_next_frame_id());   // NVIDIA Reflex: its sleep, then the simulation starts (vk_reflex_frame.c)
    uint64_t t0 = kern_now_ns();
    lat_mark("frame update start");
    rt_call(c, rt_main_addr(rt_version()->frame_update), 1, obj);
    lat_mark("frame update end");
    vk_rfx_frame_end();
    frame_trace(obj, t0);
}

void frame_rate_host(uint64_t patch_page) { rt_register_host(patch_page + FRAME_HOST, host_frame_update); }

// After the loader relocated main: the vtable slot calls host_frame_update instead.
void frame_rate_init(uint64_t patch_page) {
    const RtVersion *v = rt_version();
    uint64_t at = rt_main_addr(v->frame_slot), fn = rt_main_addr(v->frame_update);
    if (!at || !fn) {
        rt_log("game: no frame update address for game version %s: MKW_FPS has no effect", v->display);
        return;
    }
    uint64_t *slot = (uint64_t *)(uintptr_t)at;
    if (*slot != fn) {
        rt_log("game: vtable slot %#llx holds %#llx, not the frame update: MKW_FPS has no effect",
               (unsigned long long)at, (unsigned long long)*slot);
        return;
    }
    *slot = patch_page + FRAME_HOST;
}
