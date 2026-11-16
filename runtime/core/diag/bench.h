// Benchmark mode (bench.c): MKW_BENCH=tt|gp boots into a time trial or a Grand Prix, the game's own assists drive it,
// and the run ends at the finish with frame-time statistics of the races in the log.
#pragma once
#include <stdint.h>

int bench_on(void);                            // MKW_BENCH is set
int bench_save(uint8_t *save, uint64_t size);  // fs_host.c: Play/account.sav's working copy at open; 1 when changed
void bench_save_write(const char *host_path);  // fs_host.c: a save file opened for writing (the finish's save)
void bench_romfs_opened(const char *path);     // fs_host.c: a romfs file opened (the cup's ending movie)
void bench_frame(uint64_t frame);              // perf.c: each present
