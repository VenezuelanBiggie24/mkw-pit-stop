// Between perf.c (frame timing, always built) and the OS's sampling profiler (prof_win.c on Windows).
#pragma once
#include <stdint.h>

extern volatile uint32_t prof_render_tid;   // the thread that presents (perf_present)
extern volatile uint32_t prof_main_tid;     // the guest main thread (perf_main_thread)
extern volatile uint32_t prof_replay_tid;   // the asynchronous replay (perf_replay_thread): its waits hold up the frame too

// The sampler; perf.c's weak versions do nothing.
int prof_sampler_start(void);               // MKW_PROFILE: starts sampling; 1 when it runs
void prof_sampler_write(void);              // writes the profile once (prof_write)
void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d);   // every present; d: the interval, 0 on the first
// Names of host code addresses (prof_posix.c; perf.c's weak versions print exe+0x...): a lifted function as the guest's
// main+0x... (symbol+off), a host function by its symbol. prof_names_init builds the tables once (not in a signal
// handler); prof_pc_describe only reads them.
void prof_names_init(void);
const char *prof_pc_describe(uint64_t pc, char *buf, int n);
// The counters of instrumented game code (pgo.c, build.py --pgo-gen): MKW_PGO_AT's frames and plat_exit. Linux and
// Windows have the profile runtime; on the PS5 these do nothing and its objects stay as they were.
#if (defined(__linux__) || defined(_WIN32)) && !defined(MKW_PS5_NATIVE)
void pgo_frame(uint64_t frame);
void pgo_exit(void);
#else
static inline void pgo_frame(uint64_t frame) { (void)frame; }
static inline void pgo_exit(void) {}
#endif
