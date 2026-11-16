# runtime/core/diag

Diagnostics: flight recorder, counters and profiling, RenderDoc, game phases and the census of SDK calls.

| File | What it is |
|---|---|
| `flight.c`, `flight.h`, `perf.c`, `prof*`, `perfmon.*`, `rdoc.c` | Diagnostics: flight recorder, counters, profiling, RenderDoc |

The sampling profiler (`MKW_PROFILE`) is `prof_win.c` on Windows and `prof_posix.c` on Linux and the PS5 (a sampler thread signals each runtime thread, `platform/sigsample.c`; the log names lifted functions as `main+0x...` and host functions from the executable's symbols, or the PS5 title's `mkw.sym`).
| `hangwatch.c`, `hangwatch.h` | `MKW_HANG_WATCH=<s>` (default 10 on the PS5): each runtime thread's last checkpoint (`hang_mark`) and the owners of the queue, sparse and upload locks, and every runtime thread's stack with names (`MKW_HANG_STACKS=0` turns that off; read on each thread through `platform/sigsample.c`), logged once the game stops presenting for that long |
| `seqwatch.c` | `MKW_SEQ_WATCH=1`: entries and exits of the game's phases (`seq: enter TitleMenuMain frame N`) through the function-entry hook of the translated code; the `seq` command of the control channel; `MKW_SEQ_HOLD` holds the button script while a phase lasts; `MKW_SEQ_UNTIL`/`MKW_SEQ_EXIT`: goes off at a phase and exits N s later (`tools/run/race.sh`) |
| `pgo.c` | Profile-guided optimization: writes the counters of a build with instrumented game code (`tools/build/build.py --pgo-gen DIR`) at `MKW_PGO_AT=<frame>[,<frame>...]` (each file one stretch of the run, `DIR/mkw-<pid>-f<frame>.profraw`, counters reset after it) and at `plat_exit` (`-exit.profraw`), since runs end by `_exit` and the profile runtime writes only at `atexit`. Linux only: the PS5 SDK has no profile runtime, so the PS5 build uses the Linux profile (`tools/ps5/build_game.py --pgo-use`) |
| `impcensus.c` | `MKW_IMPORT_CENSUS=<file>`: which SDK functions the game code calls during the run, how many times and from which frame, through the same entry hook as `seqwatch.c`; read by `tools/analisis/sdk_gaps.py --census` |
