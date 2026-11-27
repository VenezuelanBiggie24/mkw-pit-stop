// Guest heap census (heap_census.c): MKW_HEAP_CENSUS=1 reads the game's heap tree (1.8.0) every 120 s on its own thread
// and logs how much of the backed guest memory is free inside the game's ExpHeaps ("census: ...").
#pragma once
#include <stdint.h>

void heap_census_start(void);
// The texture heap's free blocks as [lo, hi) pairs, at most max; -1 when it is not known (vk_mem_upload.c pack_step)
int texheap_free_ranges(uint64_t (*out)[2], int max);   // bench_mem.c at the window's start: starts the thread when the switch is on
