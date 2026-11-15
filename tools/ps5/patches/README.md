# PS5_Mesa patches

Applied in name order by `tools/ps5/build_radv.py` to the pinned `PS5_Mesa` revision (`tools/ps5/build_smoke.py`
`MESA`) before RADV is built; their SHA-256 goes into the archive's `PROVENANCE.txt`, so a changed patch rebuilds it.
Both applied unchanged, with line offsets only, on upstream main `0f86253` (15/11/2026).

| File | What it changes |
| --- | --- |
| `ps5_mesa-linear-alloc-null.patch` | `linear_alloc_child` (`src/util/ralloc.c`) and the threaded layer return NULL for a missing context instead of asserting |
| `ps5_mesa-prt-sparse-arena.patch` | A dedicated 128 GiB PRT aperture above the 256 GiB direct-memory region (`radv_ps5_platform.c`), so unbound sparse tiles read zero without backing a shared zero block |
