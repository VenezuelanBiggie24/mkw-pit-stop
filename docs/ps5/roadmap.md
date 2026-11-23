# PS5 roadmap

The PS5 port's roadmap: upstream's list as of its 0.1.13 merge, with the console's measurements and open items
added (On the PS5, stability and memory, multiplayer). Upstream's own list now lives in
[roadmap.toml](../project/roadmap.toml), generated into [roadmap.md](../project/roadmap.md).

What is being worked on, in order of priority: nothing may hang the GPU, then the picture as on the console (menus
before races), then the frame rate, then the player's side. Updated 17/11/2026.

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
(−0.8 ms), the replay thread on its own core (−1.1 ms), dynamic state recorded only when it changes (idea from
totk-pc-recomp, MIT; −10.6 % recorder instructions on Steam Deck; no measurable frame-time change). Next:

- **AMD GPU clocks on Linux** (done, `MKW_GPU_CLOCKS`): amdgpu judges a GPU's clock by how busy it is, and this
  chain keeps it half idle, so it stayed at its lowest clock (500 of 2855 MHz on an RX 6700 XT, 2560x1440: races at
  23-42 ms a frame). Held at the peak stable pstate: 16.7-17.2 ms, -15.1 ms a frame (medians, in-run A/B and its
  inverted twin, 17/11/2026). Integrated GPUs (Steam Deck, the 660M) are left to amdgpu: on the 660M the peak made the
  race 1.9 ms a frame slower, its GPU faster but the CPU, which shares the chip's power, slower (in-run A/B, 17/11/2026).
  It also explains #496 (`MKW_QUEUE_WAITS` costing 60 → 50 fps on a 7900 XT): +15.3 ms a frame with amdgpu's clock,
  +0.01 ms with it held.
- **Less work per draw**: descriptor sets kept across frames by their contents (`MKW_DRAW_CACHE`). On a fast CPU
  (i7-14700F, RADV) it changes nothing measurable: a race capture's recording thread ran the same instructions with
  it on and off, and its binds made ~20 % more descriptor work (sets are allocated) (16/11/2026); left for slower CPUs.
- **Recording in parallel**: chunks of a submit on several threads; today they fight over the texture lock, which has
  to be split first.
- **The kernel's global lock**: waits by address are done (−1.3 ms\*); the rest of the lock remains.
- **Hybrid CPUs** (`MKW_FAST_CORES`, #490 by @AngeloAscheri): on Linux the frame's threads stay on the fast cores
  and background work (pipeline builds, prewarming, video) on all of them; on an i7-14700F at a fixed 60 fps, races
  went from 12 of 34 windows over 17.5 ms to none (in-run A/B and its inverted twin, 17/11/2026). Still to do:
  - **Windows**: off by default, never measured there. `SetProcessAffinityMask` bounds every thread, so the
    background threads would lose the efficiency cores too (`plat_thread_background` does nothing there); it needs a
    per-thread placement (CPU Sets, or each frame thread's own mask) and then the same A/B on a hybrid CPU.
  - **Few P-cores**: on by default only with 8 fast processors left besides the replay core, a hypothesis; no CPU
    with 2-4 P-cores (i5-1235U, Lunar Lake, Ryzen AI 9 HX 370) has been measured either way.
  - `vk submit` and `vk present` start on the replay thread after it pins itself and share its processor: their own
    A/B.

  A warning for whoever measures this (16/11/2026): every race up to 17:31 took 38-41 ms a frame and every one from
  17:33 on 19-22 ms, with no change to the code; that was the GPU clock (the item above), and comparing a `taskset`
  run of one period with an unpinned run of the other first gave a 2x effect that was not there. Compare inside one
  run (`MKW_AB`).
- **The game thread**: the generated C at `-O1`. Profile-guided optimization, 12-15 % in totk-pc-recomp, gave no
  measurable gain here (section 1, 19/11/2026).

\* Our numbers (not the ones said to come from totk-pc-recomp) were measured on 07-09/11/2026 on our test machine:
Radeon 660M APU (Ryzen 5 7535HS), Linux, 720p internal resolution.

### On the PS5

A race frame takes ~24 ms on the console (15/11/2026): GPU ~7 ms and replay ~14 ms run beside the game, so the game's
own threads are the critical path (~72 ms a frame of CPU among them, measured with `MKW_PROFILE_FN`; docs/ps5/ps5.md).

For now the PS5 runs races at 30 fps by default (its device profile; docs/ps5/ps5.md, "Frame rate"): it holds 33.3 ms with
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
  functions, so the generated code stays at -O1. Measured on the console (15/11/2026): no hang, every
  hot function within noise, the game threads' total 74.7 -> 74.0 ms a frame. Next on this line: `frinta` lifted to a
  libm `round()` call (1.3 ms a frame, from `sinf`), and the `--reg-loop-weight 8` build.
- **Tried and dropped: a gentler memory governor.** A race sits at ~836 MiB of direct memory, so the governor runs its
  hard level every ~3 frames (textures evicted, shadows trimmed, ~1 GB scanned for zero pages) without raising free
  memory. Lower levels (soft 768 / hard 512 MiB, an A/B on 15/11/2026) let memory fall to 470-560 MiB: textures came
  out corrupted and the frame was slower (27.9 against 23.9 ms). The hard level holds memory where it is; the lever
  is using less (finalized pool storage kept backed since task 32, ~550-680 MiB; textures; shadows), not doing less.
- **Measured: the race frame's critical path** (19/11/2026, PS5 1.8.0 at 60 fps, `MKW_TRACE=sched`,
  `MKW_PROFILE_FN_MAINWAIT`). Main runs ~4.6 ms of a 22-23 ms frame; it waits ~6.5 ms in nvnSyncWait for the last
  presented frame's fence and ~9.5 ms on an address the job chain signals. The fence wait is a real data dependency:
  `MKW_PS5_OVERLAP` (the fence done once recorded) made the game's reads of GPU-written storage buffers wait instead
  (~270 early-read waits per 600 frames against 3), faulted the shadow copies on the watched pages (~105,000 against
  ~1,500), and the frame went to 41-44 ms. During the job wait ~3.7 other game threads run (all six cores a third of
  the time): the chain works rather than waits. The profile names WaitForAddress, CriticalSection Enter/Leave and
  GetTlsValue in it, but it charges the code after a call to the function last entered: Enter's share is the locked
  code, GetTlsValue (four loads) its callers'. The runtime's own cost, from `MKW_SVC_STATS`: yields 677 a frame at
  5.9 us (4.0 ms, all threads; spin loops, cheaper ones only spin more, 15/11), SignalToAddress 387 at 4.3 us
  (1.7 ms), and a wait's handoff to running again 12-25 us, ~10 of it the OS wake (`MKW_ADDR_SPIN` A/B: -0.09 and
  +0.15 ms, none).
- **Done: the uniform snapshot off the recording thread** (19/11/2026). Before the first chunk the replay thread
  spent, a race frame: the walk 3.3 ms, the texture slots checked ahead 1.7-3.4 ms (about 2,400; write-watch queries
  ~0.6 ms, gen_lock ~0.3 ms, the rest per slot) and the uniform snapshot ~1.1 ms (0.53 ms a submission). The snapshot
  is now taken on a thread of its own during the walk (`MKW_USNAP_ASYNC`, default 1; its readers wait for it). In-race
  A/B: frame -0.20 / -0.62 ms, median 16.70-16.75 ms with it, replay -0.9 to -1.5 ms. Next: the slots checked ahead
  in parallel with the walk too. `MKW_TRACE=perf` now splits that check's time.
- **Done: the recording threads' thread-locals in one block** (19/11/2026). The PS5 target's TLS is emulated: every
  `_Thread_local` access is a call to `__emutls_get_address`, one per variable per function, ~15 per draw and ~6 per
  replayed op. Native TLS does not link (the title's layout has no TLS segment). `VkRecTls vk_rt` (vk_exec.h) holds
  them, so a function makes one. Four alternating races: 17.53 / 17.57 ms before, 17.36 / 17.33 after; 1 % low
  35.5-35.8 -> 39.2-39.5 fps. Left as they were: trace-only counters, the fault handler's and the kernel's (`tcur`).
- **Done: no shadow copies on the PS5** (19/11/2026). Races of one build ran in one of two states, ~22 ms or
  18.8-19.7 ms: the fast ones were those where the governor's first hard pass had shed shadows
  (`vk_mem_shadow_shed`) before the race's pools were made, so they were imported (fence write-backs ~5 MiB per log
  window against ~285). Shadows answer the cost of importing on Linux (amdgpu userptrs), which the console's unified
  memory does not have, and their copies and write-backs sat in main's wait for the last frame's fence. The PS5
  device profile sets `MKW_SHADOW=0`: races 17.6-17.8 ms (three, against 22.0-22.4), GPU 7.6 -> 6.2 ms, free direct
  memory median 375 -> 642 MiB (largest area down to 68 MiB at times, all areas >= 1.06 GiB); screenshots and the
  user's menu check clean.
- **Done: pool storage no longer rebuilt over and over before the race, and the governor rate-limited** (19/11/2026,
  PS5). A pool backed again on use (`lazy_back`) kept its old last-use submit, so the next shadow trim freed it at once:
  ~1,600 rebuilds (~60 GiB) of the same 32 MiB pools before a race, with 1-6 s frames. `lazy_back` now marks the pool
  used: ~330 MiB rebuilt, no frame over 1 s. `MKW_GOV_RATE_LIMIT` (default 1) runs the governor from the headroom thread
  at most every 250 ms (18-38 passes per 60 frames before): in-race A/B ~5x fewer passes, -0.43 ms mean (-0.92 / +0.07
  by race). The log now adds up every free area of direct memory: 1.0-1.1 GiB against 0.6-0.73 GiB in the largest one,
  still near the hard level's 1 GiB, so the governor's levels stay on the largest area.
- **Done, memory gain only: the governor on all free memory** (20/11/2026, PS5 1.8.0). `MKW_GOV_TOTAL` (default 1)
  sets the levels on every free area added up, keeping a 128 MiB floor on the largest one, and its hard trim evicts
  the textures unseen longest first (`MKW_GOV_EVICT_IDLE`, 600 frames) in batches of 16 under a try-lock, not the
  whole table in hash order under `tex_lock` (6-16 ms). In-race A/B (two races, inverted): hard passes 267-288 ->
  46-62 and textures evicted 5.8-6.4 -> 1.6 GiB per race; frame -0.20 / +0.14 ms, median -0.05 / 0.00, within
  noise. All free memory still dips to 924-1,174 MiB, under the 1 GiB hard level. Re-uploads stay at ~60 per 60
  frames either way: the game rewrites those textures, so eviction is not their source.
- **Measured: what the 1% lows are, and a first fix** (20/11/2026, PS5 1.8.0). About 2% of race frames take over 20 ms:
  the replay finishes the last submission of the frame just presented after the game starts waiting on its fence. A
  probe of the replay thread's time by part showed those frames bring a course's streaming in: ~30 new textures,
  created and uploaded serially in the slot precheck (7.4-9.1 ms against 2.3), and chunk recording slowed by contention
  on tex_lock (waits 3.8 ms a frame while the holds came to 0.9: ~1 us holds, each contended acquire sleeping ~15-20 us).
  Not the cause: the walk, pipelines, image views, descriptor sets, the present thread (its drain waits on the blit, not
  on pacing), held submissions, sparse binds. Faster on average but no fewer slow frames: smaller or even chunks, more
  chunk threads, the precheck on a helper, its uploads in parallel. `MKW_TEX_LOCK_SPIN` (default 1) spins up to 30 us
  before sleeping on tex_lock: its waits -70%, the frame slightly better in three races, the slow frames as many.
  "Not yet recorded" in the SyncWait line means the fence's first part was not sent (vk_sync.c part_sent), not that the
  submission was not recorded.
- **Done, no frame gain: textures made faster on the replay thread** (20/11/2026, PS5 1.8.0). Timing the texture
  lock's holds showed the replay's slot precheck as its longest holder (~1.1 ms twice a frame, up to 36 ms); ~2% of
  frames have a pass over 5 ms (median 7 ms), half of it making new textures (~100 per 60 frames as the course
  streams) and half uploads and snapshot answers. Of making a texture, ~60% was the ASTC alpha check that picks BC1
  (996 of 1,646 ms in a race), and every one made a probe image only an out-of-memory retry uses.
  `MKW_TEX_MAKE_FAST` (default 1) spreads that check over the ASTC decode pool (same answers: 6,144 checks compared in
  a race) and makes the probe only when out of memory. In-race A/B (two races): making textures 9.5-10.3 -> 6.1-6.9 ms
  per 60 frames, the frame -0.21 / +0.21 ms (medians -0.01 / -0.02). Letting waiters into the precheck's hold
  (tried, not kept) did not move the frame either: the texture lock's waiters are not on the critical path.
- **Done, off until checked on screen: texture checks without the 2 MiB snapshot** (24/11/2026, PS5 1.0.0). The
  precheck's spikes (8-14 ms, ~1 % of race frames) were its write-watch snapshot re-arming whole 2 MiB regions with
  `sceKernelMprotect` (~2 us per written page found), and 98 % of those pages (65 % in spikes) lay in no texture it
  looked up. `MKW_SNAP_SCOPED` (default 0) checks each texture's own pages: their generations, then a peek of the
  write watch (`prot_watch_dirty`, nothing protected); only a span that may hold an unreported write is queried and
  re-armed, rounded out to whole native pages. In-race A/B, two races: arms 0.77 -> 0.24-0.28 ms a frame, the game's
  write faults 13-14 -> 2.7-4.2, uploads unchanged, precheck spikes 11.0 -> 4.8 % and 6.9 -> 2.9 % of frames, the
  frame -0.28 / -0.14 ms, busy -0.55 / -0.84. Next: a race watched on screen with it on (a missed write shows only as a
  stale texture), then default 1.
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
  translated.
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
- **Done: the queues ordered by their event waits at the pre-merge frame time** (17/11/2026, A/Bs in one race, FPS=60,
  busy = the work). Upstream's `MKW_QUEUE_WAITS=1` (#430) cost +6 ms on the PS5 (25.2 against 31.2): the first held
  submission took the whole episode's stream, so its fence covered the frame's draws and the job threads' early-read
  waits grew from ~2.3 to ~20 ms a frame. Each held submission now gets its own part (`MKW_QORDER_PARTS`, -5.0 ms) and
  a submission's ops before its first unmet wait go at once (`MKW_QORDER_PRE`, -1.2 ms). The rest were false sibling
  waits (another 4 KiB page of a watched 16 KiB native page): the fault handler now does that one load itself
  (`MKW_EARLY_SIBLING_EMU`, -5.6 ms: 29.4 -> 23.9 ms; the six reader sites are plain `mov`/`movzx` loads; the code
  bytes come from `/app0/eboot.bin`, as the PS5 maps text execute-only). Single-stepping the load with the trap flag
  ended the title at its first step (`MKW_EARLY_SIBLING_STEP`, off). Race frame now ~24 ms of work with the ordering
  on; 2-player split at FPS=30: 34.35 ms average (29.1 fps), half the windows at 33.3 ms, recording 17-21 ms.
- **Done: chunks recorded while the walk goes on** (`MKW_REPLAY_PAR_STREAM`, 17/11/2026). The walk and the precheck
  took 3.1-4.4 ms a frame before any chunk was recorded; the chunks now go out as the walk cuts them. 2-player split
  at 30 fps, A/B in one race: the render side's work 22.5 -> 20.1 ms a frame, every submission streamed; frames the same
  as serial (Linux capture, pixel for pixel) and split pictures clean on the console. The frame interval did not move
  (34.1 against 34.5 ms): at 30 fps the render side has ~12 ms to spare, and the windows over 33.3 ms are occasional
  missed vertical blanks (a 50 ms frame), not work.
  Upstream did the same on 16/11/2026 (`MKW_REPLAY_PAR_PIPE`, with a serial head, `MKW_REPLAY_PAR_HEAD`); the merge of
  0.1.13 keeps upstream's and drops `MKW_REPLAY_PAR_STREAM`. Measured on the PS5 (17-18/11/2026): the 0.1.13 build ran
  a race frame 25.6-26.3 ms against 22.4-22.8 for the 17/11 one (single player, FPS=60). Upstream's serial head records
  ~600 draws on the replay thread alone (~6 ms), and the prescan prechecks every slot before the walk (2,960 slots,
  3.1 ms). The PS5 profile now turns the head off (`MKW_REPLAY_PAR_HEAD=0`), checks slots by chunk
  (`MKW_REPLAY_PAR_CHECK_BY_CHUNK`) and has the replay thread record chunks after its walk (`MKW_REPLAY_PAR_JOIN`):
  replay 20.1 -> 16.6 ms a frame, race 24.7 ms. `MKW_REPLAY_PAR_PIPE=ab` in split: 0.19 ms (none).
- **PS5 2-player split: the water height maps' write-back (#410) on the game thread.** `MKW_COMPUTE_WRITTEN=ab` in
  split: 30.0 fps (76 % of windows at 33.3 ms) without the marks, 28.3 (30 %) with. Two 512 KiB pitch textures go back
  at every fence; the write watch arms their pages in between, and each page cost a fault or an unprotect (~70 a
  frame, ~34-40 us each). The write-back now holds the storage and holds unprotect a run of pages in one call
  (prot_watch_hold): 1.9 -> 0.8 ms a frame (ab-split-batch, 18/11/2026). The image is no longer read back with its own
  submission and wait at the fence: the copy into a host-visible buffer is recorded after the dispatch in the replay's
  command buffer (`MKW_TEX_READBACK`, vk_tex_texel.c). `MKW_TEX_READBACK=ab` in split: frame median the same (33.95
  ms), render work -3.7 ms; both height maps go back from the copy and the kart rides the waves (Koopa Troopa Beach,
  18/11/2026). ~27 more write-watch faults a frame with the marks remain, spread over pages, cause not found.
- **Next: what misses the vertical blank in 2-player split** (the game's own frame, two ticks at 30 fps, is the
  suspect: `FRAME_TRACE = 1`). Heavy windows carry ~16 % more draws. Set aside in split: `MKW_REPLAY_PAR_PRECHECK`
  (0), `MKW_REPLAY_PAR_MORE` (6 recorders: 35.0 against 34.3 ms, the cores are taken). `MKW_REPLAY_PAR_MORE`/`SMALL`
  drew whole-scene vertical streaks in their B blocks; MORE alone renders clean, so SMALL or the switching is the bug.
  Both were removed in the merge of 0.1.13.
- **Done, small gain: sibling reads by `cmp`** (19/11/2026). Two `cmp` sites held 64 % of the early-read wait time left
  on 1.8.0, so the fault handler now emulates `cmp` with a memory operand (`MKW_EARLY_SIBLING_CMP`, default 1). A/B
  inside two Grand Prix races at FPS=60: -0.38 and -0.12 ms (inverted), -0.25 ms by means and -0.46 by medians. The wait
  time was mostly not on the frame's critical path. Of the reads still not emulated, 54 % were at `EMU_MAX`.
- **Done: more sibling reads emulated** (19/11/2026). `test` with a memory operand (`MKW_EARLY_SIBLING_TEST`, default 1:
  one `testb` site held 59 % of the wait time left) and `EMU_MAX` 128 instead of 32 (`MKW_EARLY_SIBLING_MAX`). Four
  Grand Prix races at FPS=60 with `test` on: limit 32 23.21 and 24.87 ms, limit 128 22.90 and 22.40 ms; no read was left
  undecoded. An A/B of `test` at limit 128: -0.51 ms by means, -0.83 by medians, but -1.13 and +0.11 in its two races.
  Races differ: some run 56-79 emulated reads a frame against 12-16 (`main+0x1043f0` on one record), and one such race
  took 25.62 ms even at 128. Those reads cost two protection changes each.
- **Done: sibling reads from a copy of the page** (`MKW_EARLY_SIBLING_SNAP`, default 1, 19/11/2026). A/B at FPS=60: the
  race with 141 emulated reads a frame 24.93 -> 21.99 ms, the race with 21 a frame 22.33 -> 22.14 ms. A check race
  (`MKW_EARLY_SIBLING_SNAP_CHECK=1`) compared 868,731 reads with memory: none differed. With the default on, a confirming
  race took 23.18 ms (worst frame 85.3 ms, no hang).
- **Measured and set aside: longer write-watch backoff for hot pages** (19/11/2026). Pages hot again right after an arm
  stayed writable 4 and 12 times longer: repeat faults 8.6 -> 3.2 a frame, all faults 64 -> 58, frame +0.01 ms by means
  and -0.24 by medians over two A/B races. About 87 % of the faults are on pages that did not fault the cycle before.
- **Measured and set aside: the fast PS5 lock** (`MKW_PS5_FAST_LOCK`, default 0, 19/11/2026). Six alternating Grand Prix
  races at FPS=60: off 22.69, 22.40 and 21.31 ms (22.13), on 21.88, 22.05 and 22.48 ms (22.14); texture-lock waits about
  20 % lower, no hang. Its lost wakeup and a reader spin were fixed first, and it now reads `mkw.ini`.
- **Measured and set aside: profile-guided optimization** (19/11/2026). 1.8.0 on the PS5, Grand Prix at FPS=60, frame
  generation off, 210 s of race per run, in the order A B A B: `-O1` 23.87 and 24.85 ms, `-O2` with PGO 24.12 and
  23.97 ms. One run of each build had a single 4-6 s stall; without it `-O1` is 23.87 and 24.14 ms, PGO 23.64 and
  23.97 ms, so about 0.2 ms against 0.27 ms between two runs of the same build. The profile: one race on Windows with
  the real GPU, 210 s at about half speed, 13,217 functions run, the function-entry hook in none, and no function
  mismatched or missing in the PS5 build. It does not cover the menus, other courses or split screen. No gain fits the
  frame waiting on the game's job chain rather than on the speed of its code, which this test does not prove. The 4-6 s
  stalls were not looked into.
  **The 17/11/2026 test (22.84 against 22.76 ms) says nothing:** `MKW_SEQ_WATCH` kept the hook on during that profile,
  marking its branch hot in 44,564 of 44,565 functions run. Commit 38a9398 says that profile avoided the hook; it did
  not. clang crashed now and then compiling the lift 16 jobs wide (lower-expect, then two objects of the PS5 build);
  each file compiled alone.
- A null read in the game's code at `sdk+0x776a60` from `main+0x70f154` ended a run at the title (17/11/2026,
  stream-vis), the place of the 16/11 fault below: intermittent, cause not known. Again 17/11 at frame 405 in a split
  run (fix-split2), the same stack.
- **Measured and set aside (17/11/2026):** `MKW_EARLY_WAIT_AWAY` (0.00 ms), host `memset`/`round` (within noise).
  `MKW_SHOTS` stalls the PS5 6-8 s a picture on slow storage: never in a timing run.
Two defaults of the test runs mislead on another machine (16/11/2026): `run_perf.sh` caps the GPU heap at 4096 MiB
(`MKW_GPU_MEM_LIMIT`, set for the 660M), and on a 12 GB RX 6700 XT the race then took 59 ms a frame against 39-43 ms
without the cap (sl-base, sl-nocap: two separate runs, both with the GPU at its lowest clock, before `MKW_GPU_CLOCKS`;
not measured with the clock held); and `race.sh`'s 720p needs the RomFS overlay `build/romfs-res`
(`tools/romfs/resolution.py`): without it the log says "RomFS overlay: none" and a 1.8.0 race renders at 2560x1440
with a 3840x2160 output. Players have neither: no cap, the game's resolution.

## 2. The picture as on the console

- Shader translator bugs found by comparing with another Switch 2 port: three of six fixed, three to go, each
  reapplied alone and checked on a race capture.
- The kart's shadow misses a frame now and then; two fixes from Ryujinx (MIT) to try.
- Zero Vulkan validation errors, which may be rendering bugs or hangs on other GPUs: whole runs from boot to a race,
  one player (Grand Prix) and two (split screen), give none since the depth target left in the feedback-loop layout
  for the first recording chunk was fixed (RX 6700 XT, RADV, 17/11/2026); photo mode and the other modes not checked.
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
  their memory back, unpacking on the next access (docs/ps5/ps5.md); waiting for its console run (verify first).
  Task 40 (Linux, capture t40 at the title, 16/11/2026): arrays and cube maps with slices of 64x64 or more now count
  their layers for the 256x256 rule, so they go to BC1/BC7 too: ASTC copies 678 -> 585 MiB, RGBA8 146 -> 26 MiB
  (arrays 134 -> 13); their BC error 30.7 dB at worst (`MKW_ASTC_BC_CHECK`; single images 26.2), the 24x24-45x45 ones
  (27-28 dB, 6.5 MiB) kept as RGBA8. A region over the 32 MiB chunk is decoded in bands of block rows through the kept
  64 MiB scratch (an 8192x8192 level: 336 MiB -> 42 MiB, bit-exact, `--probe-astc`), so the 278-285 MB scratch buffers
  that failed are gone. Waiting for a console run.
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
  900 to 270 ms in that game), VMA reservations on Windows. Measured on an i7-14700F (16/11/2026), the CPU deswizzle
  (`tex_bl_copy`) runs at memcpy speed (16-25 GB/s, 9.6 for a 16 MiB BC7 level), so on the GPU it only pays if the
  GPU reads the storage from its mirror without a CPU copy. In the course load's peak (1.5 s, user call graphs of the
  replay thread, tl-fp1) the replay spent ~66 ms in the BC1/BC7 choice for ASTC textures (`astc_alpha_one`, at image
  creation) and ~44 ms in the deswizzle, both serial on the critical path; spreading them over worker threads is the
  cheaper step.
- A Linux package that starts on non-rolling distributions (glibc 2.31 through zig), and the launcher tested on real
  Windows machines.
- Windows with a dedicated GPU: memory the GPU wrote is not uploaded again to its mirror.
- "Detect" in the graphics settings: a preset picked from the GPU.
- Audio: two known SDK bugs (splitters, voice 0) and cuts in the movies' audio.

## 6. Later game versions

- Updates (issue #19): each version is a different executable, so the recompilation (function list, patches, the
  runtime's addresses) is redone for it; the game cannot be shared, so this needs someone who owns that version to run
  the tools on their own dump and contribute the result.
- 1.8.0 (11/11/2026): recognised by the setup (`recomp/versions.py`), its own patch table
  (`recomp/patches-mkw-180.txt`: spin waits, zstd, output size and every set of 1.0.0 — aspect, vertplus (the tall
  Window's race FOV), shadow, seed and LAN play) and runtime row (`runtime/core/boot/version.c`: frame update and game
  clock, zstd, NVN2 proc table and `nvnDeviceGetInteger` answers, boot checkpoint, the new `subsdk1` module's
  constructors, the menu focus probe). Checked (16/11/2026): no game code calls the NVN2 procs new in its sdk (its
  proc loader stores them and nothing reads them back, `runtime/gfx/nvn/procs/nvn2.c`); its 11 604 shader stages,
  the 39 inside `exefs/main` included, translate to valid SPIR-V and none is missing in a title, race or photo mode
  capture; the button icon sheet is recognised by its hash (`runtime/ui/glyphs/glyphs.c`).

## 7. Multiplayer

docs/ps5/multiplayer.md. 17/11/2026: a PS5 hosting and a Windows PC joining raced whole races over the LAN (30 fps
against 60 fps works); `local_wireless = lan` is the default since. A 2-player split race ran on the PS5.

- **A player's first race on a course can drop them**: the 3.8 s pipeline stall at the race load made the PC's game
  leave the room (docs/ps5/multiplayer.md, "what the console runs showed"). Watch for it; building the race's pipelines
  before the start, or a shorter stall, would end it.
- **Measure split screen on the PS5** (`build/run/ps5/mkw.ini.task44-split`, or unattended with `MKW_VIRTUAL_PADS`
  and an input script: title menu "Multiplayer", then the player count): the first 2-player run gave 6-8 fps with
  multi-second waits for the replay (pipelines cold); frame, busy, GPU, replay, CPU with warm caches. 4 players: later.
- **More than two machines, a peer leaving and rejoining, online through a lobby**: not tried yet.
- **Through routers without the relay**: UPnP and PCP/NAT-PMP mappings of the host's port while hosting (lease
  renewed, removed at the end; whether the PS5 may), so fewer sessions need the relay.
- **Station to station directly**: today every datagram between two stations passes through the host (one more hop
  and the host's upload); a mesh needs each pair introduced, as the lobby introduces a station to its host.
- **The PS5's invite code**: entered with the system keyboard (`runtime/sdk/swkbd`) instead of `mkw.ini`.
- **Who runs the public lobby** and where (the owner's decision; resources in docs/ps5/multiplayer.md).
- A probe for the controller applet's id order with a player missing in the middle (needs three virtual pads).
