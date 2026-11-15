#ifndef MKW_PS5_GUEST_MEMORY_H
#define MKW_PS5_GUEST_MEMORY_H

#include <stdbool.h>

// Final loader cleanup after all users of the mapped addresses have stopped.
// A false result reports a cleanup failure. No new mappings may follow.
bool ps5_guest_memory_shutdown(void);

// Optional callback registered by the Vulkan layer to append VMA / driver /
// sparse / texture memory totals to the PS5 memory census. When in_signal is
// non-zero, the callback must not take locks (use the last atomic snapshot).
void ps5_mem_set_vk_census(void (*fn)(char *out, size_t cap, int in_signal));

// Logs a tagged memory + page-table census line to stderr immediately.
void ps5_mem_log_census(const char *tag);

#endif
