# tests

Tests that need neither the game nor a GPU, run by the CI on Linux and Windows (`.github/workflows/ci.yml`). The back
end's regression test runs over captures instead (`tools/test/`).

| File | What it checks |
|---|---|
| `test_setup.py` | The setup's command lines and files from folders with spaces and accents: the compiler's path on its way to the shader step (issue #29), the resume without `--source` after an update, the update's version order and its clean-up of `*.old` files |
| `test_nup.py` | Clean-room Switch 2 `.nup` reader (`tools/prepare/nup.py`): raw and zstd entries, base + update layering, path traversal rejection, TOC SHA-256 verification, and MKW 1.0.0 build ID check |
| `test_ps5_recv.py` | PS5 placeholder TCP receiver (`runtime/platform/ps5_recv.c`) + `tools/ps5/send_ps5.py` client: path traversal rejection, bad SHA-256 rejection, atomic staging under title folder, incremental `STAT_BATCH` manifest deltas, and listener close on completion |
| `test_ps5_builder.py` | PS5 release game-content scan (`release_ps5.py` including placeholder `eboot.bin` symbol check) and pure-Python ELF64/`ar` symbol readers (`prepare_ps5.py`) |
| `test_defaults.py` | The memory caps a player gets with nothing set (`runtime/gfx/vk/device/vk_defaults.c`), built alone and checked on the GPUs players reported: the Radeon 660M keeps its measured caps, a large discrete GPU is not held to them (issue #31) |
| `test_layout.py` | Legacy desktop layouts, interrupted or mismatched generation, synthetic module placement, and stack/alias overlap in either order |
| `test_ps5_mixed.py`, `ps5_mixed_host.c` | Mixed guest permissions and partial decommit over a Linux model of PS5 backing, with synthetic AArch64 loads, stores and atomics lowered to native x86 |
| `test_ps5_loader_platform.py`, `ps5_loader_platform_host.c` | Linux SDK model of high144 loader reservations, exact page state, checked reads, ownership loss and shutdown, plus early-watch batch rollback and restore failure, `MKW_PACK`'s pack and unpack (bytes read back, zero and GPU units, a page decommitted while packed), file I/O, and locks |
| `test_ps5_guest_span.py`, `ps5_guest_span_host.c` | Checked cross-block host copies, in-flight pin quiescence and stale copy-out after backing reuse |
| `ps5_guest_span_backing.c` | Linux mapping model shared by the guest-span and QueryMemory tests |
| `test_ps5_query_memory.py`, `ps5_query_memory.c` | Real SVC 0x06 handler and memory-info layout, checked copy-out across guest boundaries, a racing protection change, and desktop null-output compatibility |
| `test_ps5_input.py`, `ps5_input_host.c` | DualSense physical buttons and gameplay bindings, axis and trigger thresholds, and stale-input clearing |

Usage: `uv run pytest tests`

Run the mixed-page test alone with `uv run pytest tests/test_ps5_mixed.py` on Linux x86-64 with Clang.
Run the QueryMemory fixture with `uv run pytest tests/test_ps5_query_memory.py` on the same host.
