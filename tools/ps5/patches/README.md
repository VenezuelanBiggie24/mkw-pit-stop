# PS5_Mesa patches

Applied in name order by `tools/ps5/build_radv.py` to the pinned `PS5_Mesa` revision (`tools/ps5/build_smoke.py`
`MESA`) before RADV is built; their SHA-256 goes into the archive's `PROVENANCE.txt`, so a changed patch rebuilds it.
Both applied unchanged, with line offsets only, on upstream main `0f86253` (15/11/2026).

| File | What it changes |
| --- | --- |
| `ps5_mesa-linear-alloc-null.patch` | `linear_alloc_child` (`src/util/ralloc.c`) and the threaded layer return NULL for a missing context instead of asserting |
| `ps5_mesa-prt-sparse-arena.patch` | A dedicated 128 GiB PRT aperture above the 256 GiB direct-memory region (`radv_ps5_platform.c`), so unbound sparse tiles read zero without backing a shared zero block |
| `ps5_mesa-heap-usage-once.patch` | `radv_query_heap_info` reports the driver's allocations as visible VRAM only: the APU budget added them as VRAM and visible VRAM, twice (heapUsage 10,966 of 7,808 MiB with ~4.5 GiB in VMA), and our headroom thread stopped allocating ahead |
| `ps5_mesa-sync-wait-realtime.patch` | `radv_ps5_sync_wait_many`'s 1 ms sleep gives `cnd_timedwait` a realtime deadline: the C11 condition variable runs on `CLOCK_REALTIME`, and the monotonic deadline it was given had always passed, so the wait spun on the lock |
