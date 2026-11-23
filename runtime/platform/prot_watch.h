#ifndef MKW_PROT_WATCH_H
#define MKW_PROT_WATCH_H

#include <stddef.h>
#include <stdint.h>

// rw: 0 read-only, otherwise writable; 3 says the call is a hold's (prot_watch_hold), for the platform's statistics.
typedef int (*ProtWatchProtectFn)(uint64_t addr, size_t size, int rw);
typedef int (*ProtWatchStateFn)(uint64_t addr);

void prot_watch_init(size_t page_size, ProtWatchProtectFn protect_fn, ProtWatchStateFn state_fn);
int prot_watch_reserve(uint64_t addr, uint64_t n);
void prot_watch_touch(uint64_t addr, uint64_t n);
void prot_watch_mark_written(uint64_t addr, uint64_t n);
void prot_watch_unprotect_and_mark(uint64_t addr, uint64_t n);
void prot_watch_hold(uint64_t addr, uint64_t n);
void prot_watch_release(uint64_t addr, uint64_t n);
int prot_watch_page_info(uint64_t addr);
int prot_watch_is_armed(uint64_t addr);
void prot_watch_set_early(uint64_t addr, int on);
int prot_watch_is_early(uint64_t addr);
int prot_watch_fault(uint64_t addr);
int prot_watch_dirty(uint64_t addr, uint64_t n);   // 1 a page may hold an unreported write, 0 none, -1 not watched
uint64_t prot_watch_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset, int *did_protect);
void prot_watch_gpu_import(uint64_t addr, uint64_t n, int on);
int prot_watch_has_gpu(uint64_t addr, uint64_t n);
uint64_t prot_watch_faults(int reset);
uint64_t prot_watch_repeat_faults(int reset);
uint64_t prot_watch_hot_skips(int reset);   // arms MKW_PW_HOT skipped   // of them, on a page that faulted in its previous arm cycle too
void prot_watch_shutdown(void);

#endif
