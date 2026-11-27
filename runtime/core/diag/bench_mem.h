// Memory report of the benchmark (bench_mem.c): memory sampled once a second on its own thread through a MKW_BENCH run
// (MKW_BENCH=ko is made for it), a "bench: mem" line every BENCH_MEM_LOG_S, statistics per window and segment at the end.
#pragma once
#include <stdint.h>

void bench_mem_start(uint64_t t0_ns);   // bench.c: the window starts (kern_now_ns); starts the sampler
void bench_mem_stop(void);              // bench.c: before the report; the sampler takes no more samples
// bench.c: the statistics of the samples taken in [a_ns, b_ns) as "bench: mem <label>..." lines; result: also the
// machine-readable "bench: memresult ..." line (the whole window).
void bench_mem_report(const char *label, uint64_t a_ns, uint64_t b_ns, int result);
