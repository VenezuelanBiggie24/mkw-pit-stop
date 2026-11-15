// Hang watch (hangwatch.c): each runtime thread names where it is (a checkpoint), and a watchdog thread logs every
// thread's checkpoint once the game's presents stop for HANG_S seconds. For hangs that leave no line behind, as the
// PS5's run 5 did after "sparse bind 2" (13/11/2026).
#pragma once
#include <stdint.h>

// This thread's checkpoint is now `where` (a string literal or other static string); returns the previous one, for a
// caller to put back on exit. A thread-local load, a clock read and two relaxed stores, whether the watch is on or off.
const char *hang_mark(const char *where);
const char *hang_mark_val(const char *where, uint64_t val);

// This thread's id (plat_thread_id), kept per thread: lock owners for the report cost no system call.
uint32_t hang_tid(void);

// A value worth logging with the checkpoints (a semaphore's or a counter's), read with no lock: fn writes it into buf.
void hang_reporter(void (*fn)(char *buf, int n));

// perf_present, each present: the frame the watch compares; starts the watch thread on the first call (MKW_HANG_WATCH).
void hang_frame(uint64_t frame);
