// Guest heap census (heap_census.c): MKW_HEAP_CENSUS=1 reads the game's heap tree (1.8.0) every 120 s on its own thread
// and logs how much of the backed guest memory is free inside the game's ExpHeaps ("census: ...").
#pragma once

void heap_census_start(void);   // bench_mem.c at the window's start: starts the thread when the switch is on
