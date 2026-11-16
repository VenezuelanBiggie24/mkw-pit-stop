// Profile-guided optimization of the generated code: writes the counters of a build with instrumented game code
// (tools/build/build.py --pgo-gen DIR) for tools/ps5/build_game.py --pgo-use.
// Why here: a run rarely ends through atexit, where the profile runtime writes by itself; it ends by plat_exit
// (_exit: MKW_SEQ_EXIT, --seconds) or by a timeout's kill. So the counters are written at known frames and at
// plat_exit. MKW_PGO_AT=<frame>[,<frame>...]: at each listed present the counters so far go to
// <stem>-f<frame>.profraw and are reset, so each file holds one stretch of the run (the menus apart from the race);
// what is left goes to <stem>-exit.profraw at plat_exit. Without MKW_PGO_AT one file, <stem>-exit.profraw. <stem> is the
// build's -fprofile-instr-generate=DIR/mkw-%p.profraw (or LLVM_PROFILE_FILE) without its .profraw, %p expanded.
// The profile runtime's symbols are weak: a build without instrumented code links them as null and this does nothing.
// Only Linux has the runtime (the PS5 SDK ships no libclang_rt.profile): prof.h makes the calls no-ops elsewhere.
#if defined(__linux__) && !defined(MKW_PS5_NATIVE)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "prof.h"

int __llvm_profile_write_file(void) __attribute__((weak));
void __llvm_profile_reset_counters(void) __attribute__((weak));
void __llvm_profile_set_filename(const char *name) __attribute__((weak));
const char *__llvm_profile_get_filename(void) __attribute__((weak));

#define PGO_MAX_AT 16
static uint64_t at[PGO_MAX_AT];
static int at_n, at_next, inited;
static char stem[512];

static void pgo_init(void) {
    inited = 1;
    if (!__llvm_profile_write_file || !__llvm_profile_get_filename) return;
    const char *name = __llvm_profile_get_filename();   // expanded: %p is this process's id
    size_t n = name ? strlen(name) : 0;
    if (n > 8 && !strcmp(name + n - 8, ".profraw")) n -= 8;
    if (!n || n >= sizeof stem - 32) return;
    memcpy(stem, name, n);
    stem[n] = 0;
    const char *e = cfg_get("MKW_PGO_AT");
    for (char *p = (char *)e; p && *p && at_n < PGO_MAX_AT;) {
        uint64_t f = strtoull(p, &p, 10);
        if (f && (!at_n || f > at[at_n - 1])) at[at_n++] = f;   // ascending frames only
        while (*p == ',' || *p == ' ') p++;
    }
    rt_log("pgo: instrumented game code, profile %s-*.profraw, %d frame(s) in MKW_PGO_AT", stem, at_n);
}

static void pgo_write(const char *part) {
    char path[sizeof stem + 32];
    snprintf(path, sizeof path, "%s-%s.profraw", stem, part);
    __llvm_profile_set_filename(path);
    int r = __llvm_profile_write_file();
    rt_log("pgo: %s %s", r ? "could not write" : "wrote", path);
}

// perf_present, every present (the render thread)
void pgo_frame(uint64_t frame) {
    if (!inited) pgo_init();
    if (!stem[0] || at_next >= at_n || frame < at[at_next]) return;
    char part[32];
    snprintf(part, sizeof part, "f%llu", (unsigned long long)frame);
    while (at_next < at_n && at[at_next] <= frame) at_next++;
    pgo_write(part);
    // Other threads keep counting meanwhile: a few counts of this frame land on either side, which PGO does not mind.
    __llvm_profile_reset_counters();
}

// plat_exit (platform/linux_sys.c), before _exit
void pgo_exit(void) {
    if (!inited) pgo_init();
    if (stem[0]) pgo_write("exit");
}
#endif
