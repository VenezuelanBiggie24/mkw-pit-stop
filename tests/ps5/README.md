# tests/ps5

The PS5 port's pieces built or run alone on Linux: the builder and its `.nup` reader, the placeholder's receiver, the
DualSense mapping and models of the PS5's memory backing and loader.

| File | What it checks |
|---|---|
| `test_ps5_nup.py` | The PS5 builder's clean-room Switch 2 `.nup` reader (`tools/ps5/nup_reader.py`): raw and zstd entries, base + update layering, path traversal rejection, TOC SHA-256 verification, and MKW 1.0.0 build ID check |
| `test_ps5_recv.py` | PS5 placeholder TCP receiver (`runtime/platform/ps5_recv.c`) + `tools/ps5/send_ps5.py` client: path traversal rejection, bad SHA-256 rejection, atomic staging under title folder, incremental `STAT_BATCH` manifest deltas, listener close on completion, and the move back of game files and saves after a clean ProsperoStore update |
| `test_ps5_ftp_install.py` | The builder's install over the console's FTP server without the receiver (`send_ps5.py send_directory_ftp`), against a fake FTP server: first install, unchanged files skipped by its record or the receiver's manifest, stale exefs modules removed by name and nothing else, a short upload leaving the old file |
| `test_ps5_builder.py` | PS5 release game-content scan (`release_ps5.py` including placeholder `eboot.bin` symbol check) and pure-Python ELF64/`ar` symbol readers (`prepare_ps5.py`) |
| `test_ps5_mixed.py`, `ps5_mixed_host.c` | Mixed guest permissions and partial decommit over a Linux model of PS5 backing, with synthetic AArch64 loads, stores and atomics lowered to native x86 |
| `test_ps5_loader_platform.py`, `ps5_loader_platform_host.c` | Linux SDK model of high144 loader reservations, exact page state, checked reads, ownership loss and shutdown, plus early-watch batch rollback and restore failure, `MKW_PACK`'s pack and unpack (bytes read back, zero and GPU units, a page decommitted while packed), file I/O, and locks |
| `test_ps5_guest_span.py`, `ps5_guest_span_host.c` | Checked cross-block host copies, in-flight pin quiescence and stale copy-out after backing reuse |
| `test_ps5_query_memory.py`, `ps5_query_memory.c` | Real SVC 0x06 handler and memory-info layout, checked copy-out across guest boundaries, a racing protection change, and desktop null-output compatibility |
| `test_ps5_input.py`, `ps5_input_host.c` | DualSense physical buttons and gameplay bindings, axis and trigger thresholds, and stale-input clearing |

C sources the tests here build with the compiler at hand and run: each `*_host.c` (and `ps5_query_memory.c`) with the
test named beside it in the table above, and:

| File | What it is |
|---|---|
| `ps5_guest_span_backing.c` | Linux mapping model shared by the guest-span and QueryMemory tests |

Usage: `uv run pytest tests/ps5` (Linux x86-64 with Clang for the memory models)
