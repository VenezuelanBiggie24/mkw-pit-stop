# tests

Tests that need neither the game nor a GPU, run by the CI on Linux and Windows (`.github/workflows/ci.yml`). The back
end's regression test runs over captures instead (`tools/test/`).

| File | What it checks |
|---|---|
| `test_warp32.py` | Executes the translator's actual shuffle/ballot helpers and emitted signed/unsigned reductions as C++ on synthetic 32/64/128-lane subgroups, with distinct values per logical warp and partially active masks |
| `test_spirv_images.py` | Integer image variants of synthetic compute shaders through the production patcher (`c/t_spirv_images.c`): signed and unsigned storage images, separate image/sampler construction, input preservation and actionable failure reasons; optional SPIR-V validation |
| `test_setup.py` | The setup's command lines and files from folders with spaces and accents: the compiler's path on its way to the shader step (issue #29), the resume without `--source` after an update, the update's version order and its clean-up of `*.old` files, the update that regenerates the code cleanup removed, NVIDIA's downloads fetched again when they arrive damaged (issue #207), a broken SM86 spec built again instead of reused (issue #325), and setup tools, unpacking and warm-up when the installation is separate from the launcher, including symlink/junction aliases of long installation paths and response-file paths with spaces, quotes and backslashes for Clang, Zig and lld-link |
| `test_steam.py` | The Tools page's Add to Steam (`tools/prepare/steam.py`): the binary `shortcuts.vdf` read back as it was written (strings, ints, a 64-bit field, tags), an entry with the same executable replaced instead of doubled with the other shortcuts kept whole, a missing file created, and a pid file that is not there or is not a number reading as Steam closed |
| `test_game_dir.py` | Installation selection and persistence, relative paths, UTF-8 length boundaries, trailing whitespace and quotes, environment overrides, settings saved in the chosen folder, library readiness before memory initialization, recovery from failed or invalid saved selections, and log-folder discovery with launcher fallbacks and active log destinations (`c/t_game_dir.c`, POSIX harness) |
| `test_game_dir_windows.py` | The actual Windows selector compiled with Clang/MinGW, run natively or under Wine: UTF-16 root limits with room for derived filenames, ASCII/BMP/supplementary characters, prepared DLL discovery, rejection before persistence, relative-prefix counting and recovery from invalid saved paths (`c/t_game_dir.c`; skipped when the toolchain or runner is absent) |
| `test_issues.py` | The issue moderation's reading of a report (`tools/github/classify.py`): the release from the form, RecompLauncher's report or the prose, the GPU without the CPU's vendor, empty reports, logs, known errors and their regressions, which maintainer replies close an issue and which player replies reopen it |
| `test_flow.py` | Shapes of control flow ptxas leaves that the translator must structure as the branch graph runs them (`sm86/translate/flow.py`, checked with `flow_sim`): a jump into a tail that an if / else shares |
| `test_ui_text.py` | The menu's text catalogs (`i18n/`): the English template has every id of `ui_text.h`, each `.po` is up to date with it, and each translation keeps the English's printf conversions in order (a swapped `%s`/`%d` would make printf read the wrong argument) |
| `test_switch2.py` | The Switch 2 controllers' Bluetooth LE protocol (`runtime/input/switch2_proto.h`): mkw-neo's test (`c/t_switch2.c`, reports recorded from real Joy-Con 2s) built against our copy |
| `test_aspect.py` | Window aspect, horizontal FOV, UI fitting and unchanged 16:9/wide/version defaults |
| `test_vertplus.py` | The actual late race-FOV hook over repeated requests and tall render-target dimensions |
| `test_vertplus_gate.py` | Old game tables without the separate FOV set fail closed to 16:9 |
| `test_ui_capture.py` | Actual draw hook, full-canvas backdrops, character/kart masks, cup backdrop, ordinary controls and movies |
| `test_mods.py` | Code patches shipped by mods (`recomp/patches.py` `load_mods`): a mod with spans only, two mods on one address stop the generate, no `mods/` folder leaves the table as it was |
| `test_cfg.py` | Every setting the in-game menu writes to `mkw.ini` is a key `runtime/core/base/cfg.c` reads back (#168: `HDR` was dropped as unknown) |
| `test_defaults.py` | The memory caps a player gets with nothing set (`runtime/gfx/vk/device/vk_defaults.c`), built alone and checked on the GPUs players reported: the Radeon 660M keeps its measured caps, a large discrete GPU is not held to them (issue #31) |
| `test_adpcm.py` | DSP-ADPCM voices (`runtime/audio/audio_mixer.c`, its voice decoding cut out and built alone) against an exact decode of random buffers (`c/t_adpcm.c`): with a context on every buffer, loops included, the output is the exact decode; without contexts at most a few samples differ (a slot refilled before the walk decoded its last samples); high pitch and sample rate cross more than 128 input samples at once; each sample is decoded about once (#331) |
| `test_nup.py` | The PS5 builder's clean-room Switch 2 `.nup` reader (`tools/ps5/nup_reader.py`): raw and zstd entries, base + update layering, path traversal rejection, TOC SHA-256 verification, and MKW 1.0.0 build ID check |
| `test_ps5_recv.py` | PS5 placeholder TCP receiver (`runtime/platform/ps5_recv.c`) + `tools/ps5/send_ps5.py` client: path traversal rejection, bad SHA-256 rejection, atomic staging under title folder, incremental `STAT_BATCH` manifest deltas, and listener close on completion |
| `test_ps5_builder.py` | PS5 release game-content scan (`release_ps5.py` including placeholder `eboot.bin` symbol check) and pure-Python ELF64/`ar` symbol readers (`prepare_ps5.py`) |
| `test_layout.py` | Legacy desktop layouts, interrupted or mismatched generation, synthetic module placement, and stack/alias overlap in either order |
| `test_ps5_mixed.py`, `ps5_mixed_host.c` | Mixed guest permissions and partial decommit over a Linux model of PS5 backing, with synthetic AArch64 loads, stores and atomics lowered to native x86 |
| `test_ps5_loader_platform.py`, `ps5_loader_platform_host.c` | Linux SDK model of high144 loader reservations, exact page state, checked reads, ownership loss and shutdown, plus early-watch batch rollback and restore failure, `MKW_PACK`'s pack and unpack (bytes read back, zero and GPU units, a page decommitted while packed), file I/O, and locks |
| `test_ps5_guest_span.py`, `ps5_guest_span_host.c` | Checked cross-block host copies, in-flight pin quiescence and stale copy-out after backing reuse |
| `ps5_guest_span_backing.c` | Linux mapping model shared by the guest-span and QueryMemory tests |
| `test_ps5_query_memory.py`, `ps5_query_memory.c` | Real SVC 0x06 handler and memory-info layout, checked copy-out across guest boundaries, a racing protection change, and desktop null-output compatibility |
| `test_net.py`, `net_host.c` | Local wireless play over UDP (`runtime/net`) and the lobby/relay (`server/lobby`) on loopback, under AddressSanitizer: LAN discovery, join, routing, broadcast, kick, lock, leave, host gone; wrong password and its block; another build and another local communication version refused; 20 000 fuzzed datagrams and the replay window; invite codes (every forged letter refused), the name filter; a private session by invite, the introduction, the relay with the largest datagram, a stranger through the relay; every lobby message at every length (no answer longer than its request without a cookie) and a flood |
| `test_ps5_input.py`, `ps5_input_host.c` | DualSense physical buttons and gameplay bindings, axis and trigger thresholds, and stale-input clearing |

Usage: `uv run pytest tests`

Run the mixed-page test alone with `uv run pytest tests/test_ps5_mixed.py` on Linux x86-64 with Clang.
Run the QueryMemory fixture with `uv run pytest tests/test_ps5_query_memory.py` on the same host.
