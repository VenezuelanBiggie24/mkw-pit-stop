// Profile-guided optimization of the generated code: writes the counters of a build with instrumented game code
// (tools/build/build.py --pgo-gen DIR) for tools/ps5/build_game.py --pgo-use.
// Why here: a run rarely ends through atexit, where the profile runtime writes by itself; it ends by plat_exit
// (_exit: MKW_SEQ_EXIT, --seconds) or by a timeout's kill. So the counters are written at known frames and at
// plat_exit. MKW_PGO_AT=<frame>[,<frame>...]: at each listed present the counters so far go to
// <stem>-f<frame>.profraw and are reset, so each file holds one stretch of the run (the menus apart from the race);
// what is left goes to <stem>-exit.profraw at plat_exit (clean ends only: plat_exit(0)). Without MKW_PGO_AT one file,
// <stem>-exit.profraw. <stem> is the build's -fprofile-instr-generate=DIR/mkw-%p.profraw (or LLVM_PROFILE_FILE)
// without its .profraw, %p expanded. After each write the runtime's file name is <stem>-exit.profraw again, so its own
// atexit write (a run that ends by exit(), e.g. the window closed) holds the rest of the run instead of overwriting the
// last stretch.
// The function-entry hook (cpu.h FN_ENTER, g_fnprof: MKW_SEQ_WATCH, MKW_QUICK_RACE, MKW_BENCH...) must not reach a
// profile: its branch would count as taken in every function and be laid out hot in the PGO build, where it is always
// off (the 17/11/2026 profile: 44,564 of 44,565 executed functions, so its "no gain" says nothing). What is counted
// while the hook is on is dropped when it goes off (MKW_SEQ_UNTIL: the race start), and a run that ends with it on
// writes nothing but <stem>-hook.profraw from the runtime's atexit, which a merge must leave out.
// The profile runtime's symbols are weak: a build without instrumented code links them as null and this does nothing.
// Linux and Windows have the runtime (the PS5 SDK ships no libclang_rt.profile): prof.h makes the calls no-ops on the PS5.
#if (defined(__linux__) || defined(_WIN32)) && !defined(MKW_PS5_NATIVE)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gamelib.h"
#include "host.h"
#include "prof.h"

int __llvm_profile_write_file(void) __attribute__((weak));
void __llvm_profile_reset_counters(void) __attribute__((weak));
void __llvm_profile_set_filename(const char *name) __attribute__((weak));
const char *__llvm_profile_get_filename(void) __attribute__((weak));

#define PGO_MAX_AT 16
static uint64_t at[PGO_MAX_AT];
static int at_n, at_next, inited, hook_counted;
static char stem[512];

static void pgo_name(const char *part);

static void pgo_init(void) {
    inited = 1;
    if (!__llvm_profile_write_file || !__llvm_profile_get_filename) return;
    const char *name = __llvm_profile_get_filename();   // expanded: %p is this process's id
    size_t n = name ? strlen(name) : 0;
    if (n > 8 && !strcmp(name + n - 8, ".profraw")) n -= 8;
    if (!n || n >= sizeof stem - 32) return;
    char s[sizeof stem];
    memcpy(s, name, n);
    s[n] = 0;
    const char *e = cfg_get("MKW_PGO_AT");
    for (const char *p = e; p && *p && at_n < PGO_MAX_AT;) {
        char *end;
        uint64_t f = strtoull(p, &end, 10);
        if (end == p) {   // not a number: stop here (strtoull would not move p, and the loop would never end)
            rt_log("pgo: MKW_PGO_AT: \"%s\" is not a frame number; frames after it ignored", p);
            break;
        }
        p = end;
        if (f && (!at_n || f > at[at_n - 1])) at[at_n++] = f;   // ascending frames only
        while (*p == ',' || *p == ' ') p++;
    }
    snprintf(stem, sizeof stem, "%s", s);   // last: pgo_exit tests stem[0]
    pgo_name("exit");   // the runtime's atexit write goes here too
    rt_log("pgo: instrumented game code, profile %s-*.profraw, %d frame(s) in MKW_PGO_AT", stem, at_n);
}

static void pgo_name(const char *part) {
    char path[sizeof stem + 32];
    snprintf(path, sizeof path, "%s-%s.profraw", stem, part);
    __llvm_profile_set_filename(path);   // copied by the runtime
}

static void pgo_write(const char *part) {
    char path[sizeof stem + 32];
    snprintf(path, sizeof path, "%s-%s.profraw", stem, part);
    __llvm_profile_set_filename(path);
    int r = __llvm_profile_write_file();
    rt_log("pgo: %s %s", r ? "could not write" : "wrote", path);
    pgo_name("exit");   // the runtime's atexit write must not overwrite this stretch
}

// perf_present, every present (the render thread)
void pgo_frame(uint64_t frame) {
    if (!inited) pgo_init();
    if (!stem[0]) return;
    if (*rt_game()->fnprof) {
        if (!hook_counted) pgo_name("hook");
        hook_counted = 1;
        while (at_next < at_n && at[at_next] <= frame) {
            rt_log("pgo: frame %llu of MKW_PGO_AT skipped: the function-entry hook is on", (unsigned long long)at[at_next]);
            at_next++;
        }
        return;
    }
    if (hook_counted) {
        hook_counted = 0;
        __llvm_profile_reset_counters();
        pgo_name("exit");
        rt_log("pgo: function-entry hook off at frame %llu: the counters so far are dropped", (unsigned long long)frame);
    }
    if (at_next >= at_n || frame < at[at_next]) return;
    char part[32];
    snprintf(part, sizeof part, "f%llu", (unsigned long long)frame);
    while (at_next < at_n && at[at_next] <= frame) at_next++;
    pgo_write(part);
    // Other threads keep counting meanwhile: a few counts of this frame land on either side, which PGO does not mind.
    __llvm_profile_reset_counters();
}

// plat_exit(0) (platform/linux_sys.c, win32_sys.c), before the process ends
void pgo_exit(void) {
    if (!inited) pgo_init();
    if (!stem[0]) return;
    if (hook_counted || *rt_game()->fnprof) rt_log("pgo: the function-entry hook was still on at exit: no profile written");
    else pgo_write("exit");
}
#endif
