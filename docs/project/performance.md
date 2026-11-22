# Performance

Frame rates (and frame times) measured on real machines, each with the version it ran: the code changes every week, so an old row says
what that build did, not what today's does. MKW asks for 60 fps in races (16.7 ms per frame); on the console, when a frame takes
longer, the game itself drops to 30 fps (33.3 ms), and Settings > Screen > Frame rate on Automatic does the same here
since 0.1.5 (12/11/2026: no switch between 60 and 30 in nine races). A frame time over 33.3 ms means slow motion even
at 30: the game counts each frame as 1/30 s. The log's `game: speed` lines give the game speed of a run.

To add your machine, open a [performance report](https://github.com/adriadam10/mkw-recomp/issues/new?template=performance.yml);
it is added here with the version it reports and a link to it. The table is sorted by version (`pre-release`, the
numbered ones, then `source <commit>`: a build from source says the commit it was built from) and, inside a version, by internal resolution, lowest first; `tools/docs/perf_table.py`
does it and the pre-commit checks it.

| Version | GPU (driver) | CPU | OS | Internal resolution | Scene | FPS (frame time) | Source |
|---|---|---|---|---|---|---|---|
| pre-release | Radeon 660M (Mesa 26.2 RADV) | Ryzen 5 7535HS, 13 GB | Linux 7.2 (CachyOS) | 720p | Race | 30-32 fps (31-33 ms median) | maintainer |
| pre-release | GTX 1050 Ti 4 GB (NVIDIA 582.66) | Core i5-8400, 16 GB | Windows 11 | 1080p | Race | 22-24 fps (42-45 ms median) | maintainer |
| 0.1.6 | RTX 3060 (NVIDIA 621.14) | Ryzen 7 5700X | Windows 11 | 1080p | Race | ~48 fps (~21 ms) | [#49](https://github.com/adriadam10/mkw-recomp/issues/49) |
| 0.1.6 | RTX 4070 SUPER (NVIDIA 621.01) | Ryzen 5 7500F | Windows 11 | 1440p | Free roam | ~59 fps (~17 ms) | [#39](https://github.com/adriadam10/mkw-recomp/issues/39) |
| 0.1.7 | RTX 3050 Laptop (NVIDIA 621.14) | Core i5-12450H | Windows 11 | 720p | Race | ~34 fps (~29 ms) | [#108](https://github.com/adriadam10/mkw-recomp/issues/108) |
| 0.1.7 | GTX 1650 SUPER (NVIDIA 621.14) | Ryzen 7 7700X | Windows 11 | 1080p | Race | ~24 fps (~41 ms, a short run) | [#117](https://github.com/adriadam10/mkw-recomp/issues/117) |
| 0.1.8 | RTX 2070 SUPER (NVIDIA 621.01) | Ryzen 7 3700X | Windows 10 | 540p | Race, 2 players | ~29 fps (~35 ms) | [#169](https://github.com/adriadam10/mkw-recomp/issues/169) |
| 0.1.8 | RTX 3050 Laptop (NVIDIA 621.14) | Core i5-12450H | Windows 11 | 720p | Race | ~37 fps (26.7 ms median), p95 23 fps (42.7 ms) | [#126](https://github.com/adriadam10/mkw-recomp/issues/126) |
| 0.1.8 | RX 5500 XT (AMD 26.10.1) | Ryzen 5 5600G, 16 GB | Windows 11 | 1080p | Race | ~18 fps (~57 ms) | [#151](https://github.com/adriadam10/mkw-recomp/issues/151) |
| 0.1.8 | RX 7900 XTX (AMD 26.10.2) | Ryzen 5 7500F | Windows 11 | 2160p | Race | 60 fps (16.7 ms median), p95 45-50 fps (20-22 ms); freezes of 1-2 s | [#138](https://github.com/adriadam10/mkw-recomp/issues/138) |
| 0.1.10 | RX 6800M 12 GB (AMD 26.10.1) | Ryzen 9 5900HX, 16 GB | Windows 11 | 1080p (FSR 1 at 80%) | Race | 57 fps mean (17.6 ms), 1% low 29 fps; 104 frames over 25 ms in 49 s (the log's exit line) | [#280](https://github.com/adriadam10/mkw-recomp/issues/280) |
| 0.1.12 | RTX 5080 (NVIDIA 621.24) | Ryzen 9 9950X3D, 32 GB | Windows 11 | 900p | Race (Lightning Cup) | 60 fps (16.7 ms), worst frame 18.8 ms (the overlay; no log attached) | [#428](https://github.com/adriadam10/mkw-recomp/issues/428) |
| 0.1.12 | RX 9070 XT (AMD 26.10.1) | Ryzen 5 5600X, 32 GB | Windows 11 | 1440p | Race | 60 fps (16.8 ms), worst frame 24.2 ms; 11 frames over 33 ms in the last minute (the overlay; no log attached) | [#431](https://github.com/adriadam10/mkw-recomp/issues/431) |
| 0.1.12 | RTX 5080 (NVIDIA 621.24) | Ryzen 9 9950X3D, 32 GB | Windows 11 | 2160p | Race | 60 fps mean (16.7 ms), 1% low 50 fps; 6 frames over 25 ms in 47 s (the log's exit line) | [#479](https://github.com/adriadam10/mkw-recomp/issues/479) |
| 0.1.13 | RTX 3050 Ti Laptop 4 GB (NVIDIA 621.01) | Core i7-11800H, 16 GB | Windows 11 | 1080p | Race | 46 fps mean (21.9 ms), 1% low 36 fps; 403 frames over 25 ms in 55 s (the log's exit line; the reports say 60 fps) | [#549](https://github.com/adriadam10/mkw-recomp/issues/549), [#589](https://github.com/adriadam10/mkw-recomp/issues/589) |
| 0.1.16 | RTX 3060 | Ryzen 7 3700X | Windows | (not in the log) | Race | 56.4 fps mean (18.0 ms median of 811 windows), 1% low 21.6 fps; 97 frames over 25 ms in 51 s (the log's exit line) | [#712](https://github.com/adriadam10/mkw-recomp/issues/712) |
| source e693271 | RX 9070 XT (Mesa 26.2.5 RADV) | Ryzen 7 7800X3D | Linux (CachyOS) | 720p to 2160p | Race | 60 fps (16.7 ms; no log attached) | [#97](https://github.com/adriadam10/mkw-recomp/issues/97) |
| source 0081969 | RX 6700 XT (Mesa 26.2.6 RADV) | Core i7-14700F, 32 GB | Linux 7.2 (Bazzite) | 1440p | Race (1.8.0, Grand Prix) | 59 fps (16.9 ms median) with the GPU clock held (`MKW_GPU_CLOCKS`, gc-def); 47 fps (21.2 ms) before it, 24-26 fps (38-41 ms) when amdgpu left the clock at 500 MHz | contributor ([#521](https://github.com/adriadam10/mkw-recomp/pull/521)) |

Players' rows come from the log attached to each report, not from the times written in it (they were often better
than the log): the median of its 60-second frame-time windows, or of its race windows where the run had
`MKW_TRACE=perf`. Left out: reports without a log, runs with frame generation on, and crash reports (those through
RecompLauncher carry only the end of the log).

## PS5 split screen

Measured on a base PS5 (22/11/2026, Mario Kart World 1.8.0, 1080p, `ps5-0.1.0-12-g77aae4a`, the same build for every
run): the Grand Prix benchmark (`tools/ps5/bench.py`, `MKW_BENCH=gp`, 210 s from the first race's start, the 60 fps
default, frame generation off, `MKW_TRACE=perf`). For 2-4 players an input script takes the place of the benchmark's
own menu presses: it picks Multiplayer, raises the player count with RIGHT on that entry, and holds A for every player
(the extra players are `MKW_VIRTUAL_PADS`). The controller applet's log line confirms the count of each run.

| Players | Average frame time | Typical frame (window median) | 1% low | 0.1% low | Worst frame | Frames over 33.3 ms | Race speed |
|---|---|---|---|---|---|---|---|
| 1 | 17.06 ms (58.6 fps) | 16.7 ms | 39.8 fps | 20.5 fps | 74.0 ms | 34 of 12,312 | 0.98x |
| 2 | 21.66 ms (46.2 fps) | 21.3 ms | 32.2 fps | 18.6 fps | 70.1 ms | 64 of 9,697 | 0.77x |
| 3 | 41.92 ms (23.9 fps) | 42.2 ms | 19.5 fps | 14.3 fps | 99.1 ms | 4,983 of 5,009 | 0.40x |
| 4 | 41.92 ms (23.9 fps) | 42.7 ms | 19.2 fps | 14.2 fps | 99.0 ms | 4,890 of 5,009 | 0.40x |

The game kept its 60 Hz present interval in every run (no switch to 30), so it counted each frame as 1/60 s and the
race slowed by the ratio: 46 fps gives 0.77x, 24 fps gives 0.40x. Three players cost the same as four.

Since then the PS5 profile holds split screen at 30 fps (`MKW_SPLIT_FPS=30`, runtime/core/boot/game.c; `same` follows
`MKW_FPS`), where a frame has 33.3 ms at full speed. 2 players with it, same benchmark (22/11/2026,
`split2p-cap30`): 33.35 ms (30.0 fps), window medians 33.3 ms, 1% low 27.3 fps, 0.1% low 17.4 fps, worst 75.4 ms, race
speed 1.00x. Not measured with it yet: 3 and 4 players (at ~42 ms a frame, about 0.8x expected) and courses other than
the benchmark's first.
