// Benchmark mode: MKW_BENCH=tt|gp|ko (Hacks tab for tt and gp, or mkw.ini [hacks] bench = ko) runs races with no player
// and reports how smoothly they ran and what they did to memory (bench_mem.c; tools/ps5/bench.py runs it on the PS5
// unattended). Every piece is something the console or the runtime already does:
//
// - The way in: MKW_QUICK_RACE=<mode>, MKW_SEED=1 and MKW_SKIP_MOVIES=1 unless the player set them (cfg.c, BENCH
//   preset). tt: the same time trial (Mario, standard kart, the course the seed gives). gp: a Grand Prix with every
//   default choice, A pressed every 1.5 s through the menus, the races and the results between them (input_script.c).
//   ko: a Knockout Tour (the game's "Survival": the main menu's second entry), every default choice and A every 1.5 s
//   the same way. Its legs run one into the next over the open world with no menu or load screen between them, and a
//   rally into the next: the most streaming per minute and the longest stretch without the reset a menu gives, so it
//   is the memory test (MKW_BENCH_MAX up to 4 h).
// - The driver: the game's own Smart Steering, Auto-Accelerate and Auto-Use Item (pause menu > Settings), switched on
//   in the save as the player would switch them. GameDataList.Product.100 has no names, only murmur3 hashes; the
//   settings are the nine bools of the struct whose members include murmur3("MinimapOn") and murmur3("CameraReverseX")
//   (hash 664761721, 3932007048). main+0x79ec7c copies each into the byte array at [[main+0x32f1e18]] + 0xb84 in the
//   pause menu's order: +0 Smart Steering (main+0x79f058), +1 Tilt Controls, +2 Auto-Accelerate (main+0x79f76c), +3
//   Auto-Use Item (main+0x79f5ec / main+0x79f6a0, member 1981820604 -> GameData 2088854732), +4 and +5 the cameras.
//   account.sav keeps each bool as a (hash, value) pair of u32 in the table between its header and the data (offset
//   at +8, 0x620 in 1.0.0).
//   In benchmark mode only, bench_patch_hosts() also overrides the AI driver's per-frame virtual (1.8.0 main+0x448f50
//   and its delegate invoker main+0x448970; 1.0.0 main+0xd6728c / main+0x287a04) via rt_dispatch_override (zero cost
//   outside MKW_BENCH): for racer 0 it clears [kart + 0x110] bit 2 (human pad, checked before bit 5 at 1.8.0
//   main+0x656e18) and sets bit 5 (CPU AI, 1.8.0 main+0xb48c68 / main+0x449060), keeping [racer_entry + 0xd0] & 3 == 3
//   so camera, HUD and race results still treat racer 0 as player 1. Through SetComLevel (1.8.0 main+0x43cf00, 1.0.0
//   main+0x2a5570; table at 1.8.0 main+0x35572f8), player 1 gets level 0 (Almighty) and rivals get level 9 (Easy3).
//   In ko runs, AIRaceSurvivalRankChecker vtable +c8 (1.8.0 main+0xb1cb70, 1.0.0 main+0x1aa9bd4) logs each leg's place
//   ("bench: leg N place P of K") and, when MKW_BENCH_NO_DROPOUT is on (default in ko, never outside bench), cancels
//   player 1's queued dropout slot (+0x3f0) and marks the leg passed (+0x340) so a bad leg cannot end the tour.
// - The window starts with the first race: ReadyRace for tt, the first IsSequenceFlag after the cup movie for gp
//   (seqwatch.c turns its watch off there, so the races run without the entry hook). It ends with what fs_host.c sees
//   for free, where a phase watch would cost a probe on every guest call of the race:
//     tt: a time trial saves its result when it ends: the first save file opened for writing at least
//         BENCH_MIN_RACE_S into the race (the earlier ones are the menus').
//     gp: the cup's ending, romfs Movie/EdEmblem_<cup>_00.webm, opened after its last race.
//     ko: MKW_BENCH_RALLIES=<n> (0: none) ends at the opening of rally n + 1, romfs Movie/<rally>_OP.webm (the files
//         presence.c tells a rally by); the first rally's opens before the window. How the game goes on from a rally's
//         results (or the kart's elimination) with A pressed is to confirm on the first run: the marks say.
//   MKW_BENCH_MAX=<s> (0: 300 for tt, 1800 for gp, 2400 for ko) ends a run that never sees it.
// - Marks: every save write and movie opened in the window is logged with its time and frame ("bench: mark"); save
//   writes more than BENCH_MARK_GAP_S apart split the window into segments with their own statistics, so a Grand
//   Prix's races show apart if the game saves after each (to confirm on the first run: the marks say). In ko the
//   rally openings split it instead, so each rally's memory shows apart and a leak shows as a rise from one to the
//   next.
// - The report: time, frames, average frame time and rate, the 1 % and 0.1 % lows (the frame time 99 % and 99.9 % of
//   frames stay under, as a rate), the worst frame and the frames over 33.3 and 50 ms, the same per segment, the
//   memory report of each (bench_mem.c), then exit. The run exits before the game commits the last save, so the
//   result and the assist change are not kept unless the game committed the save earlier (the menus save too): the
//   log says what the assists were before.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "bench_mem.h"
#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "platform/platform.h"
#include "version.h"

#define BENCH_MIN_RACE_S 45      // 3 laps of a time trial take longer than this with any driver
#define BENCH_MARK_GAP_S 10      // save writes closer than this are one save (several files)
#define BENCH_FRAMES 108000      // at least 30 min at 60 fps; more for a longer MKW_BENCH_MAX (bench_max_s)
#define BENCH_MARKS 64
#define BENCH_SMART_STEERING 3029987336u   // murmur3 of its GameData name, member 3087104944 of the settings struct
#define BENCH_AUTO_ACCELERATE 4206093005u  // member 3690836888
#define BENCH_AUTO_USE_ITEM 2088854732u    // member 1981820604 (0x76202ebc), +0xb84+3 (1.0.0 main+0x79f5ec/0x79f6a0)
#define SAVE_HEADER_MAGIC 0x01020304u

enum { MODE_OFF, MODE_TT, MODE_GP, MODE_KO };
enum { MARK_MOVIE, MARK_SAVE, MARK_RALLY };   // MARK_SAVE splits tt and gp into segments, MARK_RALLY ko
typedef struct {
    uint32_t frame;   // frames recorded before it
    uint64_t ns;
    int kind;         // MARK_*
    char what[56];
} Mark;
static const char *const mode_names[] = {"off", "tt", "gp", "ko"};

static float *frame_ms, *sorted;   // cap_frames each, allocated at the window's start (a 4 h ko run needs 3.5 MB each)
static uint32_t cap_frames;
static volatile uint32_t nframes;
static uint64_t last_ns, race_t0;
static volatile uint64_t finish_ns;   // the end (fs_host.c thread), 0 before
static int save_edited, done;
static Mark marks[BENCH_MARKS];
static int nmarks;
static PlatMutex mark_lock = PLAT_MUTEX_INIT;   // marks: fs threads; the report reads them after finish_ns
static volatile int32_t rallies;                 // ko: rally openings seen, the first one's before the window too
static int32_t ko_legs, ko_best_place, ko_last_place, ko_dropouts_prevented;   // ko: game thread, read at report()

static int bench_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *v = cfg_value("MKW_BENCH");
        mode = !strcmp(v, "tt") ? MODE_TT : !strcmp(v, "gp") ? MODE_GP : !strcmp(v, "ko") ? MODE_KO : MODE_OFF;
    }
    return mode;
}

int bench_on(void) { return bench_mode() != MODE_OFF; }

// The value slot of a bool's (hash, value) pair in the save's table, or NULL.
static uint8_t *save_bool(uint8_t *b, uint64_t n, uint32_t hash) {
    uint32_t end;
    memcpy(&end, b + 8, 4);
    if (end > n) return NULL;
    for (uint32_t o = 0x20; o + 8 <= end; o += 8) {
        uint32_t h;
        memcpy(&h, b + o, 4);
        if (h == hash) return b + o + 4;
    }
    return NULL;
}

int bench_save(uint8_t *b, uint64_t n) {
    uint32_t magic = 0;
    if (n >= 0x20) memcpy(&magic, b, 4);
    uint8_t *smart = magic == SAVE_HEADER_MAGIC ? save_bool(b, n, BENCH_SMART_STEERING) : NULL;
    uint8_t *accel = magic == SAVE_HEADER_MAGIC ? save_bool(b, n, BENCH_AUTO_ACCELERATE) : NULL;
    uint8_t *item = magic == SAVE_HEADER_MAGIC ? save_bool(b, n, BENCH_AUTO_USE_ITEM) : NULL;
    if (!smart || !accel || !item) {
        rt_log("bench: account.sav (%llu bytes) has no assist settings where 1.0.0 keeps them: left as it is; the "
               "kart will not drive itself", (unsigned long long)n);
        return 0;
    }
    uint32_t s, a, it, one = 1;
    memcpy(&s, smart, 4);
    memcpy(&a, accel, 4);
    memcpy(&it, item, 4);
    rt_log("bench: Smart Steering %s -> on, Auto-Accelerate %s -> on, Auto-Use Item %s -> on (MKW_BENCH; the save "
           "keeps them only if the game commits it before the run ends)",
           s ? "on" : "off", a ? "on" : "off", it ? "on" : "off");
    memcpy(smart, &one, 4);
    memcpy(accel, &one, 4);
    memcpy(item, &one, 4);
    save_edited = 1;
    return s != 1 || a != 1 || it != 1;
}

// A mark in the window; 0 before the window or after the end.
static int mark(int kind, const char *path, uint64_t *t0_out) {
    uint64_t t0 = __atomic_load_n(&race_t0, __ATOMIC_ACQUIRE);   // set by the present thread
    if (!bench_on() || !t0 || __atomic_load_n(&finish_ns, __ATOMIC_ACQUIRE)) return 0;
    uint64_t now = kern_now_ns();
    uint32_t fr = __atomic_load_n(&nframes, __ATOMIC_RELAXED);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    plat_mutex_lock(&mark_lock);
    if (nmarks < BENCH_MARKS) {
        Mark *m = &marks[nmarks++];
        m->frame = fr, m->ns = now, m->kind = kind;
        snprintf(m->what, sizeof m->what, "%s", name);
    }
    plat_mutex_unlock(&mark_lock);
    static const char *const kinds[] = {"movie", "save", "rally opening"};
    rt_log("bench: mark %.1f s frame %u: %s %s", (now - t0) / 1e9, fr, kinds[kind], name);
    *t0_out = t0;
    return 1;
}

void bench_save_write(const char *host_path) {
    uint64_t t0;
    if (!mark(MARK_SAVE, host_path, &t0) || bench_mode() != MODE_TT) return;
    uint64_t now = kern_now_ns();
    if (now - t0 < (uint64_t)BENCH_MIN_RACE_S * 1000000000ull) return;
    __atomic_store_n(&finish_ns, now, __ATOMIC_RELEASE);
    rt_log("bench: finish at %.1f s (the game saves %s)", (now - t0) / 1e9, host_path);
}

void bench_romfs_opened(const char *path) {
    if (!bench_on() || !strstr(path, "Movie/")) return;   // every romfs open passes here
    size_t n = strlen(path);
    int rally = bench_mode() == MODE_KO && n >= 8 && !strcmp(path + n - 8, "_OP.webm");   // a GP cup's is "<Cup>Op"
    int r = rally ? plat_inc32(&rallies) : 0;
    uint64_t t0;
    if (!mark(rally ? MARK_RALLY : MARK_MOVIE, path, &t0)) return;
    uint64_t now = kern_now_ns();
    if (rally) {
        int limit = atoi(cfg_value("MKW_BENCH_RALLIES"));
        rt_log("bench: rally %d opens at %.1f s", r, (now - t0) / 1e9);
        if (limit <= 0 || r <= limit) return;
        __atomic_store_n(&finish_ns, now, __ATOMIC_RELEASE);
        rt_log("bench: %d rallies done at %.1f s (MKW_BENCH_RALLIES)", limit, (now - t0) / 1e9);
        return;
    }
    if (bench_mode() != MODE_GP || !strstr(path, "Movie/EdEmblem_")) return;
    __atomic_store_n(&finish_ns, now, __ATOMIC_RELEASE);
    rt_log("bench: the cup ends at %.1f s (%s)", (now - t0) / 1e9, path);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// Statistics of frames [a, b) over secs; label names them in the lines ("" for the whole window).
static void stats(const char *label, uint32_t a, uint32_t b, double secs, int finished) {
    if (b > cap_frames) b = cap_frames;   // a mark after the buffer filled, or no buffer
    if (a > b) a = b;
    uint32_t n = b > a ? b - a : 0;
    if (!n) {
        rt_log("bench: %sno frames", label);
        return;
    }
    memcpy(sorted, frame_ms + a, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, cmp_float);
    double sum = 0;
    uint32_t over33 = 0, over50 = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += sorted[i];
        over33 += sorted[i] > 33.4f;
        over50 += sorted[i] > 50.0f;
    }
    double avg = sum / n, p99 = sorted[(uint32_t)(n * 0.99)], p999 = sorted[(uint32_t)(n * 0.999)];
    if (!*label) {
        const char *how = !finished                  ? "no finish seen (MKW_BENCH_MAX)"
                          : bench_mode() == MODE_GP ? "Grand Prix finished"
                          : bench_mode() == MODE_KO ? "rallies finished"
                                                    : "race finished";
        rt_log("bench: %s, %.1f s, %u frames", how, secs, n);
        rt_log("bench: average %.2f ms (%.1f fps), 1%% low %.1f fps, 0.1%% low %.1f fps, worst %.1f ms", avg,
               1000.0 / avg, 1000.0 / p99, 1000.0 / p999, sorted[n - 1]);
        rt_log("bench: frames over 33.3 ms %u, over 50 ms %u", over33, over50);
        if (bench_mode() == MODE_KO) {
            rt_log("bench: result mode=%s avg_ms=%.2f fps=%.1f low1=%.1f low01=%.1f worst_ms=%.1f frames=%u secs=%.1f "
                   "finished=%d legs=%d best_place=%d last_place=%d dropouts_prevented=%d",
                   mode_names[bench_mode()], avg, 1000.0 / avg, 1000.0 / p99, 1000.0 / p999, sorted[n - 1], n, secs,
                   finished, ko_legs, ko_best_place, ko_last_place, ko_dropouts_prevented);
        } else {
            rt_log("bench: result mode=%s avg_ms=%.2f fps=%.1f low1=%.1f low01=%.1f worst_ms=%.1f frames=%u secs=%.1f "
                   "finished=%d", mode_names[bench_mode()], avg, 1000.0 / avg, 1000.0 / p99, 1000.0 / p999,
                   sorted[n - 1], n, secs, finished);
        }
    } else {
        rt_log("bench: %s%.1f s, %u frames, average %.2f ms (%.1f fps), 1%% low %.1f fps, worst %.1f ms, over 33.3 ms "
               "%u", label, secs, n, avg, 1000.0 / avg, 1000.0 / p99, sorted[n - 1], over33);
    }
}

static void report(uint64_t end_ns, int finished) {
    uint32_t n = nframes < cap_frames ? nframes : cap_frames;
    stats("", 0, n, (end_ns - race_t0) / 1e9, finished);
    bench_mem_report("", race_t0, end_ns, 1);
    // segments: tt and gp split at save writes BENCH_MARK_GAP_S or more after the previous boundary, ko at each rally
    // opening (the first rally's is before the window: segment 1 is that rally)
    int split = bench_mode() == MODE_KO ? MARK_RALLY : MARK_SAVE;
    plat_mutex_lock(&mark_lock);
    uint32_t from = 0;
    uint64_t from_ns = race_t0, last_save = 0;
    int seg = 0;
    for (int i = 0; i < nmarks; i++) {
        if (marks[i].kind != split || (last_save && marks[i].ns - last_save < BENCH_MARK_GAP_S * 1000000000ull)) continue;
        last_save = marks[i].ns;
        char label[48];
        snprintf(label, sizeof label, "segment %d (to %s): ", ++seg, marks[i].what);
        stats(label, from, marks[i].frame, (marks[i].ns - from_ns) / 1e9, 1);
        bench_mem_report(label, from_ns, marks[i].ns, 0);
        from = marks[i].frame, from_ns = marks[i].ns;
    }
    plat_mutex_unlock(&mark_lock);
    if (seg) {
        char label[48];
        snprintf(label, sizeof label, "segment %d (to the end): ", seg + 1);
        stats(label, from, n, (end_ns - from_ns) / 1e9, finished);
        bench_mem_report(label, from_ns, end_ns, 0);
    }
}

static double bench_max_s(void) {
    double max_s = atof(cfg_value("MKW_BENCH_MAX"));
    return max_s > 0 ? max_s : bench_mode() == MODE_GP ? 1800 : bench_mode() == MODE_KO ? 2400 : 300;
}

void bench_frame(uint64_t frame) {
    (void)frame;
    if (!bench_on() || done) return;
    int64_t since_us = seqwatch_since_us();   // -1 before the first race
    if (since_us < 0) return;
    uint64_t now = kern_now_ns();
    if (!race_t0) {
        __atomic_store_n(&race_t0, now - (uint64_t)since_us * 1000, __ATOMIC_RELEASE);
        last_ns = now;
        if (!save_edited) rt_log("bench: account.sav was not edited (no save yet?): the assists are the game's defaults");
        rt_log("bench: window starts (%s)", bench_mode() == MODE_GP   ? "the Grand Prix's first race"
                                            : bench_mode() == MODE_KO ? "the Knockout Tour's first race"
                                                                      : "ReadyRace");
        double want = (bench_max_s() + 60) * 60;   // 60 fps for the whole window
        uint32_t cap = want > BENCH_FRAMES ? (uint32_t)want : BENCH_FRAMES;
        frame_ms = malloc(cap * sizeof *frame_ms), sorted = malloc(cap * sizeof *sorted);
        if (frame_ms && sorted) cap_frames = cap;
        else rt_log("bench: no memory for %u frame times: no frame statistics", cap);
        bench_mem_start(race_t0);
        return;
    }
    uint32_t n = nframes;
    if (n < cap_frames) frame_ms[n] = (float)((now - last_ns) / 1e6), __atomic_store_n(&nframes, n + 1, __ATOMIC_RELEASE);
    else if (n == cap_frames && cap_frames) {
        rt_log("bench: %u frame times kept, the later frames are not in the statistics", n);
        __atomic_store_n(&nframes, n + 1, __ATOMIC_RELEASE);   // logged once; report() clamps to cap_frames
    }
    last_ns = now;
    uint64_t fin = __atomic_load_n(&finish_ns, __ATOMIC_ACQUIRE);
    double max_s = bench_max_s();
    if (fin || now - race_t0 >= (uint64_t)(max_s * 1e9)) {
        done = 1;
        __atomic_store_n(&finish_ns, fin ? fin : now, __ATOMIC_RELEASE);   // no more marks
        bench_mem_stop();
        report(fin ? fin : now, fin != 0);
        fflush(stderr);
        plat_exit(0);
    }
}

// ---- Benchmark driver hooks (MKW_BENCH only, registered via rt_dispatch_override in bench_patch_hosts) ----
// Zero cost outside MKW_BENCH: overrides are never installed when bench_on() is 0.
static uint64_t kart_calc_addr, race_mgr_got_addr, ai_update_addr, ai_thunk_addr, com_level_set_addr, survival_cp_addr;
static GuestFn orig_kart_calc, orig_ai_update, orig_ai_thunk, orig_survival_cp;

static int bench_no_dropout(void) {
    if (bench_mode() != MODE_KO) return 0;
    const char *v = cfg_get("MKW_BENCH_NO_DROPOUT");
    return v && *v ? cfg_bool("MKW_BENCH_NO_DROPOUT") : 1;
}

static uint64_t racer0_entry(int32_t *count_out) {
    if (!race_mgr_got_addr) return 0;
    uint64_t p = *(uint64_t *)(uintptr_t)race_mgr_got_addr;
    p = p ? *(uint64_t *)(uintptr_t)p : 0;
    p = p ? *(uint64_t *)(uintptr_t)(p + 0xc0) : 0;
    p = p ? *(uint64_t *)(uintptr_t)(p + 0x08) : 0;
    p = p ? *(uint64_t *)(uintptr_t)p : 0;
    p = p ? *(uint64_t *)(uintptr_t)(p + 0x08) : 0;
    p = p ? *(uint64_t *)(uintptr_t)p : 0;
    int32_t n = p ? *(int32_t *)(uintptr_t)(p + 8) : 0;
    if (count_out) *count_out = n;
    if (n <= 0) return 0;
    return *(uint64_t *)(uintptr_t)(p + 0x10);
}

static void host_kart_calc(Cpu *c) {
    if (!orig_kart_calc) orig_kart_calc = rt_dispatch_original(kart_calc_addr);
    // KartUnit::calc (1.8.0 main+0x655980 -> main+0x656360) recomputes [kart+0x110] every frame from racer_entry[0]:
    //   +0x54 (player_index >= 0) -> bit 0 (0x01 = human pad; main+0x656f1c overwrites [kart+0x118] with pad)
    //   +0xa8 (driver_type)       -> main+0x6564c4 compares against human/ghost types; if human, clears bit 3 & bit 4!
    //                                (main+0x656efc overwrites [kart+0x118] with pad at main+0x6574fc if bit 4 == 0)
    //   +0xd0 (flags)             -> bit 5 (0x20 = CPU AI) and clears bit 2 (0x04).
    // Temporarily copying racer_entry[1]'s (a real CPU, stride 0xd8) +0x54/+0xa8/+0xd0 onto racer_entry[0] only
    // across KartUnit::calc gives racer 0 exact CPU flags (0x9038) while restoring immediately keeps camera, HUD and
    // race results on player 1.
    uint64_t unit = c->x[0];
    uint64_t kart = unit ? *(uint64_t *)(uintptr_t)(unit + 0x48) : 0;
    int32_t n = 0;
    uint64_t e0 = (kart && *(int32_t *)(uintptr_t)(kart + 0x10) == 0) ? racer0_entry(&n) : 0;
    int32_t saved_pidx = -1;
    uint32_t saved_type = 0, saved_flags = 0;
    int patched = 0;
    if (e0 && n > 1) {
        uint64_t e1 = e0 + 0xd8;
        int32_t *pidx = (int32_t *)(uintptr_t)(e0 + 0x54);
        uint32_t *tp = (uint32_t *)(uintptr_t)(e0 + 0xa8);
        uint32_t *fp = (uint32_t *)(uintptr_t)(e0 + 0xd0);
        uint32_t cpu_type = *(uint32_t *)(uintptr_t)(e1 + 0xa8);
        uint32_t cpu_flags = *(uint32_t *)(uintptr_t)(e1 + 0xd0);
        if (*pidx >= 0 || *tp != cpu_type || *fp != cpu_flags) {
            saved_pidx = *pidx;
            saved_type = *tp;
            saved_flags = *fp;
            *pidx = -1;
            *tp = cpu_type;
            *fp = cpu_flags;
            patched = 1;
            static int logged;
            if (__atomic_load_n(&race_t0, __ATOMIC_RELAXED) && !logged++)
                rt_log("bench: kart calc CPU takeover racer=0 pidx %d->-1 type %#x->%#x flags %#x->%#x",
                       saved_pidx, saved_type, cpu_type, saved_flags, cpu_flags);
        }
    }
    orig_kart_calc(c);
    if (patched) {
        *(int32_t *)(uintptr_t)(e0 + 0x54) = saved_pidx;
        *(uint32_t *)(uintptr_t)(e0 + 0xa8) = saved_type;
        *(uint32_t *)(uintptr_t)(e0 + 0xd0) = saved_flags;
    }
}

static uint64_t ai_kart(uint64_t ai) {
    uint64_t p = ai ? *(uint64_t *)(uintptr_t)(ai + 0x68) : 0;
    p = p ? *(uint64_t *)(uintptr_t)(p + 0x248) : 0;
    p = p ? *(uint64_t *)(uintptr_t)(p + 0x100) : 0;
    return p ? *(uint64_t *)(uintptr_t)(p + 0x48) : 0;
}

static void ai_enforce_level(Cpu *c, uint64_t ai, int32_t racer_id) {
    if (!com_level_set_addr || !ai) return;
    uint64_t ctrl = *(uint64_t *)(uintptr_t)(ai + 0x58);
    if (!ctrl) return;
    // ComLevel table (1.8.0 main+0x35572f8): 0 = Almighty, 1 = Hard1 .. 9 = Easy3.
    // Some modes' ComLevel maps may omit Almighty (leaving [ctrl+0x18] == 0); fall back to 1 (Hard1) if so.
    static int32_t best_level = 0;
    int32_t want = racer_id == 0 ? best_level : 9;
    int32_t cur = *(int32_t *)(uintptr_t)(ctrl + 0x20);
    uint64_t param = *(uint64_t *)(uintptr_t)(ctrl + 0x18);
    if (cur != want || !param) {
        uint64_t saved[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
        rt_call(c, com_level_set_addr, 2, ctrl, (uint64_t)(uint32_t)want);
        param = *(uint64_t *)(uintptr_t)(ctrl + 0x18);
        if (racer_id == 0 && want == 0 && !param) {
            best_level = 1;
            want = 1;
            rt_call(c, com_level_set_addr, 2, ctrl, 1);
            param = *(uint64_t *)(uintptr_t)(ctrl + 0x18);
        }
        for (int i = 0; i < 4; i++) c->x[i] = saved[i];
    }
    static uint32_t race_logged;
    if (__atomic_load_n(&race_t0, __ATOMIC_RELAXED) && (racer_id == 0 || racer_id == 1) &&
        !(race_logged & (1u << racer_id))) {
        race_logged |= (1u << racer_id);
        uint64_t kart = ai_kart(ai);
        uint32_t flags = kart ? *(uint32_t *)(uintptr_t)(kart + 0x110) : 0;
        float f0 = param ? *(float *)(uintptr_t)(param + 0x30) : -1.f;
        float f1 = param ? *(float *)(uintptr_t)(param + 0x34) : -1.f;
        float f2 = param ? *(float *)(uintptr_t)(param + 0x38) : -1.f;
        rt_log("bench: race ai racer=%d flags=%#x level %d -> %d param=%#llx (%.2f, %.2f, %.2f)",
               racer_id, flags, cur, want, (unsigned long long)param, f0, f1, f2);
    }
}

static int32_t ai_prepare(Cpu *c, uint64_t ai) {
    uint64_t kart = ai_kart(ai);
    if (!kart) return -1;
    int32_t racer_id = *(int32_t *)(uintptr_t)(kart + 0x10);
    // Bit 5 (0x20) of [kart + 0x110] selects CPU driving (1.8.0 main+0xb48c68 / main+0x449060, 1.0.0 main+0xd67398)
    // and bit 2 (0x04) selects human pad input, checked before bit 5 at 1.8.0 main+0x656e18: clearing bit 2 and
    // setting bit 5 hands steering/throttle/items to the AI buffer [kart + 0x78] while [racer_entry + 0xd0] & 3
    // stays 3 so camera, HUD and race results still treat racer 0 as player 1.
    if (racer_id == 0) {
        uint32_t old = *(uint32_t *)(uintptr_t)(kart + 0x110);
        uint32_t nw = (old & ~0x04u) | 0x20u;
        if (nw != old) {
            static int logged;
            *(uint32_t *)(uintptr_t)(kart + 0x110) = nw;
            if (!logged++) rt_log("bench: CPU driver took over player 1 (kart flags %#x -> %#x)", old, nw);
        }
    }
    ai_enforce_level(c, ai, racer_id);
    return racer_id;
}

static void host_ai_update(Cpu *c) {
    if (!orig_ai_update) orig_ai_update = rt_dispatch_original(ai_update_addr);
    uint64_t ai = c->x[0];
    int32_t racer_id = ai_prepare(c, ai);
    orig_ai_update(c);
    if (racer_id >= 0) ai_enforce_level(c, ai, racer_id);
}

static void host_ai_thunk(Cpu *c) {
    if (!orig_ai_thunk) orig_ai_thunk = rt_dispatch_original(ai_thunk_addr);
    // Delegate invoker (1.8.0 main+0x448970, 1.0.0 main+0x287a04) inlines ai_driver_update when [del+0x40] matches.
    uint64_t del = c->x[0];
    uint64_t obj = *(uint64_t *)(uintptr_t)(del + 0x38);
    uint64_t fn = *(uint64_t *)(uintptr_t)(del + 0x40);
    int64_t adj = *(int64_t *)(uintptr_t)(del + 0x48);
    uint64_t ai = (!(adj & 1) && fn == ai_update_addr) ? (obj + (uint64_t)(adj >> 1)) : 0;
    int32_t racer_id = ai ? ai_prepare(c, ai) : -1;
    orig_ai_thunk(c);
    if (ai && racer_id >= 0) ai_enforce_level(c, ai, racer_id);
}

static void record_ko_leg(int leg1, int place, int total) {
    ko_legs++;
    ko_last_place = place;
    if (place > 0 && (!ko_best_place || place < ko_best_place)) ko_best_place = place;
    rt_log("bench: leg %d place %d of %d", leg1, place, total);
}

static void host_survival_checkpoint(Cpu *c) {
    if (!orig_survival_cp) orig_survival_cp = rt_dispatch_original(survival_cp_addr);
    // AIRaceSurvivalRankChecker vtable +c8 (1.8.0 main+0xb1cb70, 1.0.0 main+0x1aa9bd4):
    //   +0x38  legs[6] (stride 0x18, passed count at +0x10)
    //   +0x338 n_passed, +0x340 passed_mask[] (bit L = racer passed leg L)
    //   +0x348 n_ranks,  +0x350 ranks[] (stride 8, 0-based place at +0)
    //   +0x378 total_racers
    //   +0x3f0 dropout_slots[] (stride 0x80, state at +0x00, leg at +0x78)
    uint64_t chk = c->x[0];
    int32_t n_passed = *(int32_t *)(uintptr_t)(chk + 0x338);
    uint32_t *passed_mask = *(uint32_t **)(uintptr_t)(chk + 0x340);
    uint32_t mask0_before = (n_passed > 0 && passed_mask) ? passed_mask[0] : 0;
    uint32_t slot0_before = *(uint32_t *)(uintptr_t)(chk + 0x3f0);

    orig_survival_cp(c);

    n_passed = *(int32_t *)(uintptr_t)(chk + 0x338);
    passed_mask = *(uint32_t **)(uintptr_t)(chk + 0x340);
    uint32_t mask0_after = (n_passed > 0 && passed_mask) ? passed_mask[0] : 0;
    uint32_t slot0_after = *(uint32_t *)(uintptr_t)(chk + 0x3f0);
    int32_t total0 = *(int32_t *)(uintptr_t)(chk + 0x378);

    uint32_t newly_passed = mask0_after & ~mask0_before;
    for (int l = 0; l < 6; l++) {
        if (!(newly_passed & (1u << l))) continue;
        int32_t place = *(int32_t *)(uintptr_t)(chk + 0x38 + (uint64_t)l * 0x18 + 0x10);
        int32_t of = l == 0 ? total0 : *(int32_t *)(uintptr_t)(chk + 0x38 + (uint64_t)(l - 1) * 0x18 + 0x10);
        record_ko_leg(l + 1, place, of);
    }

    if (slot0_before == 0 && slot0_after != 0) {
        int32_t l = *(int32_t *)(uintptr_t)(chk + 0x3f0 + 0x78);
        int32_t n_ranks = *(int32_t *)(uintptr_t)(chk + 0x348);
        int32_t *ranks = *(int32_t **)(uintptr_t)(chk + 0x350);
        int32_t place = (n_ranks > 0 && ranks && ranks[0] >= 0) ? ranks[0] + 1 : total0;
        int32_t of = l <= 0 ? total0 : *(int32_t *)(uintptr_t)(chk + 0x38 + (uint64_t)(l - 1) * 0x18 + 0x10);
        if (l >= 0 && l < 6 && !(newly_passed & (1u << l))) record_ko_leg(l + 1, place, of);
        if (bench_no_dropout()) {
            // Cancel player 1's queued dropout (same clear main+0xb1cc54 uses) and mark leg L passed so leg L+1 accepts player 1.
            *(uint32_t *)(uintptr_t)(chk + 0x3f0) = 0;
            if (l >= 0 && l < 6 && n_passed > 0 && passed_mask) passed_mask[0] |= (1u << l);
            ko_dropouts_prevented++;
            rt_log("bench: dropout prevented at leg %d", l + 1);
        }
    }
}

void bench_patch_hosts(void) {
    if (!bench_on()) return;
    const RtVersion *v = rt_version();
    com_level_set_addr = rt_main_addr(v->com_level_set);
    race_mgr_got_addr = rt_main_addr(v->race_mgr_got);
    if (v->kart_calc && v->race_mgr_got) {
        kart_calc_addr = rt_main_addr(v->kart_calc);
        rt_dispatch_override(kart_calc_addr, host_kart_calc);
    }
    if (v->ai_driver_update && v->ai_driver_thunk) {
        ai_update_addr = rt_main_addr(v->ai_driver_update);
        ai_thunk_addr = rt_main_addr(v->ai_driver_thunk);
        rt_dispatch_override(ai_update_addr, host_ai_update);
        rt_dispatch_override(ai_thunk_addr, host_ai_thunk);
    }
    if (bench_mode() == MODE_KO && v->survival_checkpoint) {
        survival_cp_addr = rt_main_addr(v->survival_checkpoint);
        rt_dispatch_override(survival_cp_addr, host_survival_checkpoint);
    }
}
