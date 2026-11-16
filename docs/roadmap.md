# Roadmap

What is being worked on, in order of priority: nothing may hang the GPU, then the picture as on the console (menus
before races), then the frame rate, then the player's side. Updated 09/11/2026.

## 1. 60 fps in races: the serial frame

A race frame takes 31-33 ms\* on the Radeon 660M at 720p, against the 16.7 ms the game asks for. The time does not go
to one slow piece but to a chain where each step waits for the previous one (measured with `MKW_TRACE_TL`):

| Step | Time\* |
|---|---|
| The game runs its frame, from the release of the previous frame's fence to its last submit | ~3.4 ms |
| The Vulkan back end records that submit into command buffers, on the replay thread | ~19 ms |
| The GPU runs what was submitted last | ~9.7 ms |
| Completion work before the game is released for the next frame | ~1.5 ms |

The game's big submit only reaches the back end at the end of its frame, and the GPU cannot start until it is
recorded, so recording is on the critical path: every millisecond saved there is a millisecond off the frame, down to
~21 ms, where the GPU (busy ~20 ms) takes over. Spreading the same CPU work over more threads does not help by
itself; recording with less CPU does. Already done: recordings reused across frames (−4.3 ms), recording overlapped
with the game (−2.4 ms), submits on their own thread (−1.2 to −1.6 ms), the shadow copies off the replay thread
(−0.8 ms), the replay thread on its own core (−1.1 ms). Next:

- **Dynamic state recorded only when it changes** (idea and code from totk-pc-recomp, MIT): there it took the replay
  from 7.77 to 6.67 ms.
- **Less work per draw**: descriptor sets kept across frames by their contents (`MKW_DRAW_CACHE`).
- **Recording in parallel**: chunks of a submit on several threads; today they fight over the texture lock, which has
  to be split first.
- **The kernel's global lock**: waits by address are done (−1.3 ms\*); the rest of the lock remains.
- **The game thread**: the generated C at `-O1`, with profile-guided optimization next (12-15 % in totk-pc-recomp).

\* Our numbers (not the ones said to come from totk-pc-recomp) were measured on 07-09/11/2026 on our test machine:
Radeon 660M APU (Ryzen 5 7535HS), Linux, 720p internal resolution.

### On the PS5

A race frame takes ~24 ms on the console (15/11/2026): GPU ~7 ms and replay ~14 ms run beside the game, so the game's
own threads are the critical path (~72 ms a frame of CPU among them, measured with `MKW_PROFILE_FN`; docs/ps5.md).

For now the PS5 runs races at 30 fps by default (its device profile; docs/ps5.md, "Frame rate"): it holds 33.3 ms with
the race at full speed on a busier course. The items below are what 60, heavier courses and split screen still need.

- **Done: the synchronization calls' own cost.** `MKW_SVC_STATS` (15/11/2026, a race): ~590 yields a frame
  (`nn::os::YieldThread`, the job system's spin loops at `main+0x4520` and `main+0x5660`) at 8.3 us each, 4.9 ms a frame;
  SignalToAddress ~510 a frame at 3.95 us. `MKW_YIELD_NOLOCK` skips the kernel lock when no waiter may take the core
  (6.3-7 us a yield), but the frame did not move (A/B -0.12 ms): the loops spin waiting for other jobs, and a cheaper
  spin only spins more (yields 595 -> 650 a frame). The time is the game's own job chain: the translator's code next.
- **Done: the CPU translator's loops.** The compiler barrier on loop back edges now stays only where a loop
  may read or write a fixed address (`recomp/loops.py`: 73 452 of 137 111 dropped, half of them backward jumps that
  were no loop). More locals, or locals chosen by loop use, cost more than they saved on random functions;
  `--reg-loop-weight 8` helps `main+0x154e14`'s list walk (`x11` no longer stored and reloaded per node) and is
  for a second build to measure. -O2 gave more instructions and memory operands than -O1 on the race's 24 hottest
  functions, so the generated code stays at -O1; PGO is next. Measured on the console (15/11/2026): no hang, every
  hot function within noise, the game threads' total 74.7 -> 74.0 ms a frame. Next on this line: `frinta` lifted to a
  libm `round()` call (1.3 ms a frame, from `sinf`), and the `--reg-loop-weight 8` build.
- **Tried and dropped: a gentler memory governor.** A race sits at ~836 MiB of direct memory, so the governor runs its
  hard level every ~3 frames (textures evicted, shadows trimmed, ~1 GB scanned for zero pages) without raising free
  memory. Lower levels (soft 768 / hard 512 MiB, an A/B on 15/11/2026) let memory fall to 470-560 MiB: textures came
  out corrupted and the frame was slower (27.9 against 23.9 ms). The hard level holds memory where it is; the lever
  is using less (finalized pool storage kept backed since task 32, ~550-680 MiB; textures; shadows), not doing less.
- **Done, no frame gain: the zero scan skips what it already read.** Each hard pass read every frozen unit for zero
  pages (~1.5 GB, 9.3 ms a pass on the governor's thread, freeing almost nothing). `MKW_ZERO_SCAN_CACHE` (default 1)
  skips a unit found not all zero earlier in the same freeze (`mem_frozen` now returns the freeze's number). A/B on
  16/11/2026 (`zscan-ab`): 0.1 MiB scanned and 1.7 ms a pass, the same free memory (610 against 616 MiB) and zero
  pages freed, busy +0.5 ms (noise): the scan was off the critical path. The pass still walks every unit (~1.2 ms).
- **Measured and set aside (16/11/2026; `tools/run/ab.py` now also gives "busy", the frame less the pacing wait):**
  the early-read watch's waits for another 4 KiB page of a watched 16 KiB page (~1 a frame of ~8 ms on job threads,
  every one such a sibling, no real early read): skipping them (`MKW_EARLY_SIBLING_WAIT=0`) did not shorten the work
  (+0.6 ms busy), they overlap other work. Slow wakes (`MKW_SVC_STATS`): ~11 a frame take 200 us or more, 98 % of it
  waiting for the virtual core, held by an equal-priority worker of the other job pool (two pools of six, one per
  core, priority 44, as main on core 0); main loses ~0.3 ms a frame to them. What is left is the game's own work as
  translated: profile-guided optimization of the generated C next (task 35).
- **Found, to measure: `main+0x154e14` is a wait, not a walk** (task 36, 16/11/2026). Its 6.3-7 ms a frame
  (`MKW_PROFILE_FN`) are an early-read wait for the GPU at its start: a read at 0x25a052c0e8 of a 16 KiB native page
  whose other 4 KiB page 0x25a052f000 the GPU writes (a sibling wait). In the sibling A/B run 58 of the 60 logged waits
  were there, ~1 a frame of 9.7 ms; the race's waits total ~7.8 ms a frame at 60 fps and ~1.9 at 30 (2.2 ms each). The
  waiting thread keeps its virtual core, so the profiler counted the wait as running; the walk itself counts for the
  mutex's `Enter` (the last function entered). The list is the game's own: registrations by id (+0x134) with a mask of
  slots (+0x130), added by `main+0x847680` up to a capacity (+0x100), killed by id by `main+0x1b16850` (one walk a
  kill) and freed two updates later; nothing reads a fence, a counter or a timestamp. The profile now gives the wait
  apart, the early-read line names its readers, and `MKW_LIST_WATCH=1` logs the list's length and calls; the console
  run decides between `MKW_EARLY_WAIT_AWAY` (the core back while waiting; skipping the wait gave nothing, see above)
  and the per-node cost (`--reg-loop-weight 8`).

## 2. The picture as on the console

- Shader translator bugs found by comparing with another Switch 2 port: three of six fixed, three to go, each
  reapplied alone and checked on a race capture.
- The kart's shadow misses a frame now and then; two fixes from Ryujinx (MIT) to try.
- Push constants of constant bank 24 are not passed yet: five programs read zeros.
- Zero Vulkan validation errors (25 left, 9 kinds), which may be rendering bugs or hangs on other GPUs.
- Task/mesh shaders as compute: a race on the first thread in the scene lists (bushes and grass).

## 3. Menus

From the title to the race everything is right; the mode, character and vehicle select screens still need checking
screen by screen against the console.

## 4. Stability and memory

- GPU hangs on AMD: upload batches already split; next, breadcrumbs and `VK_EXT_device_fault` to find the cause of the
  remaining ones.
- GPU memory: races run in 4 GiB with every texture; the sparse tiles above 768 MiB (the asphalt) are next.
- **PS5: free memory runs out across races** (run `nocache`, 16/11/2026, three races in one session, yesterday's
  eboot `14752dcc`): free direct memory fell 1104 -> 568 -> 468 -> 405 -> 316 -> 285 -> 105 MiB by frame ~22 400
  while the VMA heap rose and fell with each race (2.5-3.9 GiB), so what grows is outside VMA. At 105 MiB the ASTC
  scratch buffers (278-285 MB) and five images failed (`-2`, out of device memory): textures left undecoded or
  missing. The player saw the texture corruption grow with each race. Run `memgrow` (16/11/2026, the census now
  logs the guest's backed memory and the pools that grew): the guest's backed memory rose 3282 -> 5178 MiB (heap
  ranges 2336 -> 3624, alias 875 -> 1483) out of 11712 MiB of direct memory, beside VMA ~3.7-4.5 GiB and sparse
  1.6 GiB, until 90-136 MiB were free. Almost half of it is one pool: 0x248195b000, 2427 MiB CPU_CACHED|GPU_CACHED,
  never resolved by the GPU (the staging texture of the terrain arrays, 0x2481a17a00, is in it), backed 901 -> 1635
  MiB a little each race; the rest is new small pools per course. Pages the CPU wrote stay backed; on the console
  the whole pool is memory anyway. Run `memgrow2` (the census also gives the textures in each big pool): the live
  textures' storage is 642 of the pool's 1003 backed MiB at the menus but only 183-220 of 1377-1434 MiB in later
  courses, so ~1.2 GiB of it is earlier courses' data (textures finalized since, or not textures), kept because it
  is not zero; giving it back is not safe while the game may read it again (the character cards of task 32). Our
  copies of the textures there: ASTC to BC 1347-1380 MiB from 461-476 MiB stored (2.9x), ASTC to RGBA8 (arrays,
  ~200 MiB) 7x their storage; the runtime's own texture memory is ~1.6 GiB, sparse 1.6 GiB. The levers: the arrays
  to BC like the other ASTC textures, and the governor counting the guest's growth. The same run ended in a guest
  read fault at 0x25659903d8 (main+0x7419e8 / sdk+0x776a60, frame 31201, 463 MiB free), in a 0.5 MiB pool at
  0x2565954000: first seen, cause not known. Task 37: `MKW_PACK=1` compresses frozen units cold for 30 s and gives
  their memory back, unpacking on the next access (docs/ps5.md); waiting for its console run (verify first).
- **PS5: sparse tiles left non-resident from boot.** Every run logs 12-13 "tile slots over `MKW_SPARSE_MIB`" (1536
  MiB), up to 2048 tiles (128 MiB) without memory, clean runs too; a lead for the corruption near the start line
  that has been there in every run, and for the black-and-white race intro (not yet looked at).
- **PS5: magenta textures, intermittent, new on 16/11/2026** (character select near Mario's eyes, scenery, karts
  outlined): seen on several eboots including yesterday's, clean on others with the same eboot. Only the ASTC
  decoders write magenta (error blocks; void extents may legitimately be magenta, so `MKW_ASTC_ERRORS` must skip
  them before its counts mean anything). Unclear after the run with the pipeline and shader caches set aside
  (renamed `*.bak-20261116` on the console). If it comes back: the ASTC error counter, and whether it follows low
  free memory (a scratch buffer or texture allocation failing).
- Five kernel and platform bugs inherited from totk-pc-recomp (the fiber stack alignment can crash a run).

## 5. Loading and the player's side

- The stutter right after a race starts: block-linear deswizzle on the GPU (totk-pc-recomp, MIT: texture waits from
  900 to 270 ms in that game), VMA reservations on Windows.
- A Linux package that starts on non-rolling distributions (glibc 2.31 through zig), and the launcher tested on real
  Windows machines.
- Windows with a dedicated GPU: memory the GPU wrote is not uploaded again to its mirror.
- "Detect" in the graphics settings: a preset picked from the GPU.
- Audio: two known SDK bugs (splitters, voice 0) and cuts in the movies' audio.

## 6. Later game versions

- Updates (1.8.0 and others, issue #19): planned once 1.0.0 plays as on the console. Each version is a different
  executable, so the recompilation (function list, patches, the runtime's addresses) is redone for it; the game cannot
  be shared, so this needs someone who owns that version to run the tools on their own dump and contribute the result.
