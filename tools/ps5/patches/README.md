# PS5_Mesa patches

Applied in name order by `tools/ps5/build_radv.py` to the pinned `PS5_Mesa` revision (`tools/ps5/build_smoke.py`
`MESA`) before RADV is built; their SHA-256 goes into the archive's `PROVENANCE.txt`, so a changed patch rebuilds it.
Both applied unchanged, with line offsets only, on upstream main `0f86253` (15/11/2026).

| File | What it changes |
| --- | --- |
| `ps5_mesa-linear-alloc-null.patch` | `linear_alloc_child` (`src/util/ralloc.c`) and the threaded layer return NULL for a missing context instead of asserting |
| `ps5_mesa-prt-sparse-arena.patch` | A dedicated 128 GiB PRT aperture above the 256 GiB direct-memory region (`radv_ps5_platform.c`), so unbound sparse tiles read zero without backing a shared zero block |
| `ps5_mesa-heap-usage-once.patch` | `radv_query_heap_info` reports the driver's allocations as visible VRAM only: the APU budget added them as VRAM and visible VRAM, twice (heapUsage 10,966 of 7,808 MiB with ~4.5 GiB in VMA), and our headroom thread stopped allocating ahead |
| `ps5_mesa-sync-wait-monotonic.patch` | The queue's sync condition variable runs on `CLOCK_MONOTONIC` (Mesa's `u_cnd_monotonic`), the clock of `os_time_get_nano()` and of `radv_ps5_sync_wait_many`'s deadlines: as a C11 `cnd_t` (`CLOCK_REALTIME`) its 1 ms poll returned at once and spun; a first fix giving it a realtime deadline (`timespec_get`) made the poll last up to ~1 s on the PS5 and the game stalled in steps of ~960 ms (run regioncopy, 16/11/2026) |
| `ps5_mesa-videoout-acquire-monotonic.patch` | VideoOut's condition variable (`wsi_common_videoout.c`) runs on `CLOCK_MONOTONIC` too: the acquire's 20 ms wait for a free buffer got a monotonic deadline on a `CLOCK_REALTIME` C11 `cnd_t`, returned at once and spun on the lock the flip thread needs; frames waited out the 1 s acquire timeout and were skipped (stalls in ~960 ms steps, runs memgrow2 to mono, 16/11/2026) |
