# Mario Kart World: Pit Stop (PS5)

**Mario Kart World: Pit Stop** (`PPSA99193`) is the native PS5 homebrew port of [mkw-recomp](https://github.com/adriadam10/mkw-recomp), built on a PC from an extracted Mario Kart World 1.0.0 copy. The
output is a title folder with `eboot.bin`, `sce_sys/`, and the local data the runtime reads (`exefs/`, `romfs/`, and
`spirv/`). Use the built-in game module path in `tools/build/build.py` to link the lifted C into the title. The PC
builder (`tools/ps5/prepare_ps5.py`) handles game preparation from a release bundle without a source checkout. On the PS5, start in the game runtime and keep saves in the writable profile path.

Mario Kart World is © Nintendo. This project is not affiliated with or endorsed by Nintendo, Sony Interactive Entertainment, or any game publisher, contains no game code or assets, and does not help to obtain them.

[Hookshot's PS5 beta](https://github.com/mihawk-99/PS5_Hookshot/blob/ps5-v0.1.0-beta.1/docs/PS5.md) is the reference for
the build and title layout, linked RADV, Vulkan display, and DualSense and audio integration. Implement against MKW's
interfaces. MKW's guest mappings and Vulkan replay are different enough to need their own checks. Use Hookshot's
GPL code as a design reference and follow the repository's source rules for any implementation reused elsewhere.

The [native game target](../../tools/ps5/README.md#native-game-target) builds the real runtime and local lift as one
title. Its first startup milestone is verified ExeFS loading and relocation before guest constructors.

## Build and test order

1. **Start with a game-free title.** Build the [Vulkan smoke title](../../tools/ps5/README.md) with the pinned PS5 SDK and
   RADV toolchain. Confirm the blue frame on the console and capture the selected device's Vulkan capabilities,
   queue support and virtual address reservation results. Record the console model and driver revision.
2. **Bring up the runtime's platform layer.** Implement the PS5 parts of `runtime/platform/platform.h`. Check that the
   fixed module, kernel and host-function ranges, the 14 GiB stack and heap reservation, and the 64 GiB alias region
   fit alongside the driver. The desktop module base (`0x7000000000`, 448 GiB, in `recomp/layout.py`) and the runtime's
   kernel and host-function bases fall inside the 256–512 GiB RADV reservation reported by Hookshot. Verify the PS5
   address map against `ps5-high144`. The smoke title held the entire layout and backed samples at both stack and
   heap ends, at the alias base, and at the module and kernel bases.
   The [PS5 backing probe](../../tools/ps5/README.md) tests the native page and backing granularity. Verify the guest's
   4 KiB permission and write-tracking semantics before wiring this module into `plat_mem_*`. Once the game loader
   is available, use `--probe-kernel` before attempting a gameplay boot.
   The smoke title also tests the dispatcher with synthetic callbacks at `ps5-high144` addresses and native
   read-only protection of its host-owned table.
3. **Connect presentation, input and audio.** Implement a PS5 backend for the window interface in
   `runtime/gfx/nvn/host/` and connect DualSense and audio output to MKW's existing input and mixer. Cross-build the
   needed libraries such as Opus, libvpx and zstd. Use `--probe-nvn`, `--probe-vkmem`, `--probe-vkexec`, `--probe-astc`
   and `--probe-window` to check the renderer with synthetic work before the first game boot.
4. **Boot and race.** Generate the C and SPIR-V on the PC with the existing tools. Build and stage the title locally,
   then check the title screen, a menu input, movie playback, and one race in that order. If mesh shaders are absent,
   measure MKW's existing compute mesh path on the console. Record peak memory during a race before tuning performance.

## Page table memory

`amm.pagetableMemorySizeInMib = 256` in `sce_sys/param.json` was tried (run 5, 13/11/2026): the kernel printed
`[Amm config] Page table memory : 256 MiB` for the title, and the title's direct memory total dropped from 11712 to
11520 MiB, the extra 192 MiB of pool. However, `sceKernelGetPageTableStats` still reported 16384 CPU units in total,
and run 8 still hit `[KERNEL] page table exhausted` once those 16384 units were used up. Because it costs 192 MiB of
direct memory without expanding the 16384-unit CPU page-table pool, the setting was removed.

`libSceAmpr` (`sceAmprGetMemoryRange` / AMM range query, `klog-probe-pt8.txt`) reports the AMM range as
`[0x80000000000, 0x88000000000)` (512 GiB, `multimap = 0`). The function's return register held garbage
(`0x8f5f2f08`), so it likely returns `void`.

Every ordinary mapping costs one CPU unit per 2 MiB of VA, whatever its memory type, protection, aliasing or
`sceKernelMapDirectMemory2` (`klog-probe-pt6`), and a GPU read of reserved, unmapped VA ends the title
(`GPU_FAULT_PAGE_FAULT_ASYNC`). RADV maps its 2 MiB zero block over all unbound sparse VA, so one 8192x8192x68 array
costs 2,903 units. In run 8 the game held four of them (11,612 units), asked for two more and died with the pool at
106 units free. Two leads remain, both measured by `tools/ps5/page_table_probe.c`:

- **AMM** (libSceAmpr). The title's AMM VA range (512 GiB, `[Amm config]`) comes from
  `sceAmprAmmGetVirtualAddressRanges`; `amm.multimapVaRangeInGib` sizes the MultiMap range. Retail titles map their
  heaps there through command buffers (`sceAmprAmmCommandBufferMap`, submitted and waited on). Whether those mappings
  draw on the 16,384-unit pool is not known yet. The probe reads the ranges; it writes no AMM command, because the
  public sources disagree on `sceAmprCommandBufferSetBuffer` ([research](../project/research.md#sources)). A title cannot look a
  symbol up at run time (`sceKernelDlsym` answers ESRCH in every form), so the probe links libSceAmpr as an import
  (`tools/ps5/ampr_stub.c`).
- **PRT apertures** (`sceKernelSetPrtAperture`). On the PS4 an aperture makes unmapped pages inside it partially
  resident. There are three (id 3 is EINVAL); a free one reads back `1, 0`. Run 7's attempt at 0x20b800000 was EINVAL,
  below the PS4's PRT area (0x10_0000_0000 + 0xec_0000_0000, the only range a public source checks). On the PS5, a
  128 GiB aperture at `0x8000000000` succeeds (`rc=0`, 0 CPU units, 64 GPU units): unmapped reads return zero,
  unmapped writes are dropped, and mapped 64 KiB pages work normally (1 CPU unit per 2 MiB touched). Clearing an
  aperture with `sceKernelSetPrtAperture(id, (void *)1, 0)` returns `0x80020016` (EINVAL), so an aperture is set once
  per process and never cleared.

ShadowMountPlus keeps its own copy of `sce_sys` once a title is registered (`/user/app/<title>/sce_sys`), and the
launcher reads that copy. `tools/ps5/deploy.py` refreshes its `param.json` and `icon0.png` (the home screen icon)
after each publish when it exists.

## Hangs

Run 5 stopped after `vk: sparse bind 2` with no crash and no `radv/ps5: the GPU has not reached submission`. The last
line, a `PS5 memory before-sparse-bind` census, is printed by the replay thread while it holds the queue lock and
before it takes the sparse lock (`vk_sparse_order`). The runtime's locks had a cycle there: queue lock, then sparse lock
(`vk_sparse_order`); sparse lock, then upload lock (`vk_sparse_remap` -> `slot_get` -> `vk_upload_begin`); upload lock,
then queue lock (`vk_upload_flush`). The census makes several kernel calls with the queue lock held, which widened the
window on the console. `vk_upload_flush` now takes the queue lock before the upload lock.

RADV on the PS5 binds sparse memory on the CPU: `vkQueueBindSparse` waits on the calling thread until the GPU has run
what the bind waits for (379 ms for the second bind of run 5). The PS5 has no sparse-only queue family whose submit thread
would take that wait: RADV turns it off when timeline semaphores are emulated (`radv_dedicated_sparse_queue_enabled`).
This wait cannot deadlock by itself. Every value it waits for is already submitted when the queue lock is taken, and no
submission waits on a CPU signal. Still, the sparse lock stayed held for all of that time.
`MKW_SPARSE_CPU_WAIT=1` (the default on the PS5) waits before the sparse lock is taken, so the game threads mapping
pages and the completion thread do not wait for the GPU. `MKW_SPARSE_CPU_WAIT=2` waits where RADV does, for testing on Linux.

`MKW_HANG_WATCH=<s>` (default 10 on the PS5, 0 elsewhere) logs `hang watch:` lines once the game has not presented for
that many seconds. They give each runtime thread's last checkpoint (the queue, sparse and upload lock waits,
`vkQueueBindSparse`, `vkQueueSubmit2`, fence waits, the replay and present threads), the owners of those three locks,
and the sparse order timeline. A line follows when presents resume.

Run 25 hung with the early-read watch on (`MKW_EARLY_READ=3`, 16 KiB pages): the replay "in a submission" for 70 s
and no lock named. The same conditions on Linux (`MKW_PROT_WATCH=1 MKW_EARLY_16K=1 MKW_EARLY_READ=3 MKW_PEEK_PLAIN=1`)
hung the same way, and the hang watch's stacks named it. The game's submit thread held `frz_lock` in
`mem_early_watch`, which filled a merge base's page (`mem_base_fill_locked`, `lazy_hook`). That page's 16 KiB native
page had no access because of another 4 KiB page's watch, so the copy faulted, and `thaw_fault` waited for `frz_lock`
on the thread that held it. Every other thread then queued on `frz_lock`. Now:

- the base copy opens the native page first and watches it again after (`memory_btrap.c`);
- a fault on a thread that holds `frz_lock` stops with a named error, not a hang;
- the PS5's `plat_mem_read` copies with `vm_lock` held, and a fault there would have taken `frz_lock` against
  `mem_early_watch` holding `frz_lock` and waiting for `vm_lock` (`plat_mem_state`). That copy is guarded now
  (`fault_guard.c`): it fails, and the caller reads the plain way;
- only the game's own code waits for the GPU in the early-read handler (`kern_fault_in_game`): a guest thread in
  lifted code with no host call in progress, which holds no host lock. Runtime threads and host code on guest threads
  release the watch and go on. A wait for a fence the replay has not handed to the queue gives up after 50 ms, and the
  hang watch names a waiting thread with its fence;
- the wait line names the address, the watched page, the frame, the GPU position, the thread and the function.

## Write tracking

`runtime/platform/prot_watch.c` implements `plat_mem_written` on the PS5 (16 KiB native protection granularity,
reporting all 4 KiB subpages of each written 16 KiB page) and on Linux when `MKW_PROT_WATCH=1` (4 KiB granularity).
Only committed `RW` pages that have been queried with `reset` are armed read-only; untouched pages stay `RW` and take
no faults. `plat_mem_back` unprotects and marks pages written before kernel writes (`pread`) or GPU imports so they do
not fail with `EFAULT`. On the PS5, `plat_mem_populated` walks the chunk directory and reports committed 64 KiB units.

## Direct memory: GPU-only pools, shadow trim and staging

The PS5's device profile turns shadow storage off (`MKW_SHADOW=0`, 19/11/2026): every CPU-visible pool is imported, so
the GPU reads and writes guest memory directly and nothing is copied in before a submit or written back at a fence.
Races went from 22.0-22.4 to 17.6-17.8 ms. `MKW_SHADOW=1` in mkw.ini brings the shadows back; the shadow trim below
applies only then.

MKW writes a CPU_NO_ACCESS pool's contents (BNTX texture blocks, vertex and index data) into its storage and then
initializes the pool over it, so at `vk_pool_init` the bytes exist only in guest memory. The runtime reads them from
there three ways:

- packaged textures (`nvnTextureBuilderSetPackagedTextureLayout`, so `tex_mirrored`) at their upload, after any
  eviction, and in `vk_texture_astc_copy`;
- each buffer's shadow, copied at its first GPU use and again after a shadow trim;
- write-backs, which bring the GPU's writes into the pool (copies, clears, storage buffers) back to guest memory.

GPU-owned textures (compressible or display flags, depth or stencil formats, multisample) are never read from guest
memory: the GPU writes their images directly.

`MKW_GPU_ONLY_RELEASE` (default 1 on the PS5) now releases only the storage of GPU-owned textures in such pools, when
their image is made, and the shadow copy zero-fills those ranges instead of reading them back. Releasing whole pools at
init zeroed every packaged texture (run 28's magenta). A packaged texture over released storage is logged.
A range is recorded as released only for the units actually given back: with nothing freed (task 32's census, 0.0 of
47.8 MiB) the shadows copy guest memory as with the switch off.

`MKW_POOL_RELEASE` (default 1 on the PS5) gives back, when the game finalizes a pool, only the whole 64 KiB units of
its storage that hold zeros: a unit committed again is zero-filled, so the game reads what it left there. NVN hands a
finalized pool's storage back with its contents, and MKW keeps them: each character-select card has a CPU_NO_ACCESS
pool of its own over its 144 KiB, which the game finalizes and then samples the card from again. Releasing every whole
unit zeroed bytes 0xd000-0x1d000 of each card (task 32: magenta bands, gone with the switch off and with this fix).
That version gave back 281-328 MiB per census; this one 2.0-2.4 MiB, since the finalized storage holds the game's data
(548-678 MiB backed in task 32's fix run).

Issue 11 (game 1.0.0 out of direct memory in the third race of a Grand Prix) was the race shadow map. 1.0.0 initializes it (3072x3072, 3 layers of D16, 54 MiB) at a new
place of the render-buffer pool without finalizing the last one, so on Race_Coaster_A_00 the runtime made it 2,539
times and held up to 9 live copies, 501 MiB (`mem: gpu-owned by shape`, run issue11-shapes-20261123-210136, which
finished the Grand Prix by luck); with the guest near 5.3 GiB the next 54 MiB image or a 4 MiB transient buffer
failed. 1.8.0 finalizes its maps, and released entries written over by a later image were already dropped
(vk_tex_evict.c). Live GPU-owned images now go by the same rule when idle for `MKW_TEX_LIVE_DROP_IDLE` submissions (default 4; with 64 the drop lagged and 9 maps were live again). Verified on the console (issue11-livedrop4-20261123-215530): the Grand Prix finished, 304 MiB free at worst against 68, no out-of-memory retries against 32, GPU-owned images at 617 MiB at most against 1.1-1.3 GiB, 58.3 fps as before. A hook giving back the guest memory of blocks freed in the game's ExpHeaps was tried and returned under 256 MiB in a Grand Prix: the guest's growth (~5.2 GiB, 1.8.0 alike) is still open
(`drop_overwritten_live`, every 64 MiB of GPU-owned images made and before an image creation gives up).

`MKW_PACK=1` (on in the PS5 profile since 24/11/2026) packs cold guest memory instead of holding it: under memory
pressure the governor's thread compresses (zstd level 1) each 64 KiB unit frozen for `MKW_PACK_COLD_S` (default 30) and
not GPU-imported or early-watched, and gives back its direct memory when it shrinks to 75 % or less; a zero unit is
given back with no copy. The next access to a packed unit, a fault on any thread or `plat_mem_back*` before a system
call or an import, decompresses it into fresh backing before its protections apply, so it reads the same bytes. The
store is 1 MiB slabs of direct memory (`MKW_PACK_STORE_MIB`, default 512, 1024 in the PS5 profile; the C heap had
only ~24 MiB of flexible memory free), and
`MKW_PACK_VERIFY=1` keeps each unit's bytes too and compares them on unpack. The census gives `packed N MiB in M MiB`
and a `mem: pack` line (packs, unpacks by cause and thread, their time, skips). Run memgrow2's ~1.2 GiB of old data in
one pool is what it is for; on the romfs's 64 KiB units, models packed to 0.46 and large textures to 0.74 (42 % of
those units under 75 %), so how much that pool gives back depends on how much of it is texture data.
Units reach it through the cold feed: the governor's thread hashes the game's memory (20 ms a tick, `mem: cold
census` line per walk) and freezes the 2 MiB regions whose units all stayed unwritten for `MKW_PACK_FEED_S` (default
60); `MKW_COLD_CENSUS=1` counts without freezing. In a 1.8.0 Grand Prix (pack-gain-20261124) it packed 1,573 MiB into
453 MiB at the same frame time (17.50 ms against 17.52), hard governor passes 1,827 -> 87, the census's lowest free
direct memory 168 -> 304 MiB; 121k unpacks of 218 us on average (3.9 ms at most), and the verify run found no unit
changed in 3,447.
Save writes (`plat_file_pwrite`) back their guest buffer first: the kernel reads it and answers EFAULT on a unit with
no backing.

`MKW_SHADOW_TRIM` (default 1 on the PS5) drops cold shadows under memory pressure: no GPU use for 180 submissions,
their write-backs done, no released ranges, so guest memory holds every byte. A pool whose shadow cannot be made again
for want of memory now tries again at the next submission. Before, it stayed without GPU storage for the rest of the
session and read zeros. `MKW_SHADOW_TRIM_TEST=1` trims on every governor tick, to test it on Linux.
Until 15/11/2026 `shadow_trim` took `touch_lock` and then called `sdone_drain`, whose `spend_take` takes `touch_lock`
again whenever recording threads have noted new GPU writes: the governor deadlocked on itself holding `pool_lock`, the
replay waited for `pool_lock` in `resolve_locked`, and the game for the replay's fence. It ran on every governor tick,
so the hang came at random, within a minute or near a race's end; the drain now comes before `touch_lock`. Two races
with trimming on ran without a hang (task 33's runs, 412 governor ticks freed shadows).

### Page protection costs

A page-protection change costs ~44 us on the PS5, one 16 KiB page or many. A race spent 12.5-15.6 ms a frame in them
(15/11/2026, `TRACE=perf` logs `page protection ... by caller` every 600 frames; `exe+` callers name with
`tools/run/sym.py --ps5`), most of it on the game's threads, which take the write watch's faults. Three changes:

- `MKW_PW_HOT` (default 1 on the PS5, `ab` to compare): a page that faults in two arm cycles in a row stays writable
  and is reported written for the next 13 queries instead of being armed again (`prot_watch.c`). Half the faults were
  such repeats. A/B in one race: 33.9 -> 32.2 ms a frame, faults 145 -> 64 a frame; texture re-uploads rose from 10 to
  163 per 60 frames (~0.4 ms a frame: textures that share a page with buffers the game writes every frame).
- `change()` (`ps5_guest_memory.c`) makes one `sceKernelMprotect` per run of native pages left in one mode
  (`ps5_mem_batch_begin`), the units held locked until the flush. Thawing a frozen 2 MiB region was ~128 calls, 9-10 ms
  on the game thread that wrote it; it is ~0.2 ms, and `mem_freeze` 2.4 -> 0.2 ms.
- The early-read watch's arms (`early_protect`) merge a run across 64 KiB units: 85 -> 70 us a call.

Page protection then took 4.3-5.4 ms a frame; the race ran at a median of 26-28 ms and 0.98-0.99x game speed (it was
0.85-0.93x). `MKW_ADDR_SPIN` (20 us spin before a guest address wait blocks, skipping the OS wake-up of 10-15 us) gave
-0.45 ms a frame in one A/B, within the scene's noise: it stays off.

Out of staging memory after relieving it, a texture upload is deferred (it streams in a later frame) instead of ending
the title (run 31). The same goes for `vk_texture_astc_copy` and ReadTexels/WriteTexels.

### The title pause

Every boot paused 3-16 s on the world map behind the title, the render thread waiting on textures (15-16/11/2026).
MKW fills its 8192x8192x68 sparse terrain arrays a level of a layer at a time from 8192x8192 staging textures it writes
in its CPU_CACHED pool, and the replay is about 2 submits behind it: each copy found the staging texture's pages written
since the last one and uploaded all of it first. The ASTC staging texture was already read region by region
(`astc_into_sparse`); the BC6H one (`0x2481bbdc00`, 87 MiB) went up 242 times in one frame (`titleup`), and in other boots
~900 re-uploads of 53.6 GB took 13.8 s (`titlepause-20261116-193942`). A copy out of a mirrored texture the CPU wrote
into a sparse texture of the same VkFormat now reads only the copied region from the source's storage
(`vk_texture_region_copy`, task 42), at the same point of the replay; the perf line `texture re-uploads` adds `region
copies from storage`, and `MKW_TEX_COPY_LOG=N` logs each copy out of such a texture with the path it took.
`MKW_TEX_REGION_COPY=0` restores the old path, to compare.

## Controls and language

The device profile also shows PlayStation button icons (`MKW_BUTTON_ICONS=playstation`) and sets the DualSense's
vibration to 100 % (`MKW_RUMBLE_STRENGTH=100`; 130 elsewhere); both stay in Pit Stop > Controls. With `MKW_LANG` unset
the game follows the console's system language (`plat_user_locale`, libSceSystemService), as the Switch does; a
language the game does not ship falls back as on a PC (Turkish to the rebuilt texts over British English, others to
American English). Until 24/11/2026 it was American English whatever the console's language.

## Frame rate

The PS5 build runs races at 60 fps without frame generation: its device profile (`runtime/core/base/cfg.c`,
`MKW_DEVICE_PROFILE` auto or `ps5`) sets `MKW_FPS` to 60, below `mkw.ini` and Pit Stop, so `FPS = 30` or `auto` and
`FRAMEGEN` still apply. A Grand Prix race at 60 (20/11/2026, 1.8.0, `MKW_TRACE=perf`): 17.0-17.4 ms a frame on
average, window medians at the 16.7 ms cap, 1% lows 35-41 fps, game speed 0.95-0.98x, since frames over 16.7 ms slow the
race down (about 2% take over 20 ms, where a course streams in new textures: roadmap.md). Not yet played at
this default outside that benchmark (menus, time trials, split screen, long sessions). At 30 the game advances two
ticks a frame and the race keeps full speed; that was the default until 20/11/2026, when a race frame's work was ~26-27
ms, and with `auto` the game then swapped its 60 and 30 Hz present interval ~1,400 times a minute (36-41 fps, uneven).
Measured at 30 on a busier course (16/11/2026,
`fps30-20261116-063832`): every 600-frame window at 33.3 ms, speed 1.00x, 27 slow frames in 5,300 (single hitches up to
236 ms, some blocked on texture uploads), replay 15-18 ms a frame against ~14 on the first course. The slack under
33.3 ms is not in that log: `FRAME_TRACE = 1` logs the game's own frame time. Local split screen multiplies the draws:
at 60 (22/11/2026, the same benchmark with every player scripted) 2 players average 21.7 ms (0.77x), 3 and 4 players
41.9 ms (0.40x), against 17.1 ms for 1 player on the same build (docs/project/performance.md#ps5-split-screen). So the
profile sets `MKW_SPLIT_FPS=30`: two or more players on the console (the controller applet's count, reset when the
title menu starts) go at 30 at most, outside the movies; 2 players then hold 33.3 ms at 1.00x, 3 and 4 players average 43.0 and 44.1 ms
(0.78x and 0.75x; 24/11/2026, `ps5-0.2.1-31-g23b2024`).

### Multiplayer

Split screen: each player signs in to the PS5 (a guest user works) with their own DualSense, up to 4; the light bar
shows the player's controller number (blue, red, green, pink). Local wireless with another PS5 or a PC on the LAN:
`local_wireless = lan` in `mkw.ini` on both, the same build on both (another one is refused), UDP 11460 open between
them. Over the internet through a lobby: `local_wireless = online`, `lobby = a.b.c.d:11461` (an address: the PS5 build
does not resolve names). The PS5 cannot paste an invite code: put it in `mkw.ini` as `net_join`. Details:
[multiplayer.md](multiplayer.md).

### Benchmark

`uv run tools/ps5/bench.py run <PS5 address>` runs one unattended: it sends the `titlectl` payload to the ELF loader if
it is not running (`tools/ps5/titlectl.c`: starts, stops and reports the title on port 9031), appends `BENCH = gp` to
`mkw.ini`, kills any running copy, launches the title, follows `mkw.log` over FTP, kills it if the log stops or the run
times out, saves `build/run/ps5/<name>-<time>-mkw.log` and puts `mkw.ini` back. `--mode tt` for a time trial,
`--runs N` to repeat, `--set KEY=VALUE` for more keys. Launching works on this console through
`sceSystemServiceLaunchApp` with the logged-in user (16/11/2026): the payload checks the shell's focus flag and says
`focused=1` when the game has the screen. A second launch request while the game starts sends it to the background,
so the payload makes one and waits a few seconds after a kill.

`BENCH = tt | gp` (Hacks tab, or `mkw.ini` `[hacks]`; `runtime/core/diag/bench.c`) runs races with nobody holding the
controller and ends the title at the finish. Unless set otherwise, it uses `QUICK_RACE = <mode>`, `SEED = 1` and
`SKIP_MOVIES = 1`. The game's own Smart Steering and Auto-Accelerate drive (switched on in `Play/account.sav` when the
game opens it). `tt`: the same time trial, from its ReadyRace to its save at the finish. `gp`: the first cup with every
default choice, A pressed every 1.5 s through the menus, the races and the results, from the first race to the cup's
ending movie (`Movie/EdEmblem_*`). `BENCH_MAX` seconds (0: 300 for tt, 1800 for gp) end a run that never gets there.
Each save write and movie in the window is a `bench: mark` line, and save writes split the window into segments with
their own numbers (whether they fall between a Grand Prix's races is for the first run to show). The log then gets:

```
bench: Grand Prix finished, 537.5 s, 16001 frames
bench: average 33.59 ms (29.8 fps), 1% low 30.3 fps, 0.1% low 12.5 fps, worst 236.0 ms
bench: frames over 33.3 ms 33, over 50 ms 33
bench: result mode=gp avg_ms=33.59 fps=29.8 low1=30.3 low01=12.5 worst_ms=236.0 frames=16001 secs=537.5 finished=1
bench: segment 1 (to account.sav): 132.4 s, 4000 frames, average 33.09 ms (30.2 fps), 1% low 30.3 fps, ...
```

(example numbers). The run exits before the game commits its last save, so the result and the assists stay out of the
save unless the game committed it earlier. The log's first `bench:` line says what the assists were.

## Threads, priorities and profiling

A title's threads run on CPUs 0-12 (mask 0x1fff: six full cores and half of the seventh, whose sibling, CPU 13, is
the system's). The console's `pthread_getaffinity_np` and `pthread_setaffinity_np` refuse FreeBSD's 32-byte
`cpuset_t` with `ERANGE`, so the runtime passes 16-byte sets (`ps5_threads.c`). Before 14/11/2026 every affinity call
failed: run 16 logged `mask=0` for every thread and `MKW_REPLAY_CORE` never applied.

Two placement switches, both off by default on the PS5 until a console A/B shows a gain:

- `MKW_REPLAY_CORE=1`: the replay thread alone on CPU 10 and every other runtime thread off CPUs 10 and 11.
- `MKW_THREAD_PRIO=1`: the runtime's threads at the priorities they ask for (present and vsync highest, replay above
  normal, shadow, headroom and pipeline builds below normal). Off, every thread keeps the title's priority (700).
  Run 16, the first build with them, had the replay at 31-33 ms a race frame against 22 ms in run 15.

Each takes `ab` to switch with the A/B blocks of one run (`MKW_AB=<seconds>`, which needs the phase watch's world
clock). In `mkw.ini`, `[diagnostics]`:

```ini
TRACE = perf
SEQ_WATCH = 1
SEQ_UNTIL = -WaitIntroMovie,IsSequenceFlag
AB = 10
THREAD_PRIO = ab
```

then `uv run tools/run/ab.py build/run/ps5/<log>` on the log. Test one switch per run; `AB_INVERT = 1` in a second run
separates the change from the scene.

The console's `pthread_getcpuclockid` answers every thread with the caller's clock and its `ITIMER_PROF` signals the
process rather than the running threads (run 16: 12 samples in 600 frames). `platform/sigsample.c` reads each thread's
CPU time on the thread itself through SIGPROF (`sched: host threads`, and the `cpu` column of `MKW_TRACE=sched`), and
the profiler (`MKW_PROFILE=1`, `MKW_PROFILE_US`, `core/diag/prof_posix.c`) sends SIGPROF to each runtime thread from a
sampler thread. It names lifted functions as `main+0x...` itself and host functions from `mkw.sym`, which
`build_game.py` writes next to the eboot: copy it to `/data/homebrew/PPSA99193/mkw.sym`. Without it, host addresses show
as `exe+0x...`, offsets that `uv run tools/run/sym.py --ps5` names (the logged `exe+` offsets are `llvm-pie.elf`'s
addresses since the same date).

On the PS5 use `MKW_PROFILE_US=10000` or `MKW_PROFILE_ONLY=guest` (only the game's threads get signals), and
`MKW_PROFILE_FROM=<frame>` to start past the loads. Each sample weighs its thread's CPU time since the previous one but
lands where the thread is when signalled: the game's job threads block ~75 times a frame each, so most of their samples
fall in waits and the CPU before them goes to `wait_cond`. Code sampled while running (system calls, page protection)
is reliable. `MKW_TRACE=sched` gives each game thread's CPU and where it waits. Its `render busy` in the perf line is
the interval less pacing, not the render thread's CPU (2.6 ms a frame in a race; the job workers 8-11 ms each).

Signals reach a PS5 thread only when it enters the kernel, so the running samples above land at system calls (~17 of
~70 ms a frame). `MKW_PROFILE_FN=1` (with `MKW_PROFILE_FROM`, `MKW_PROFILE_US`) profiles without signals: the lifted
code's function-entry hook notes each game thread's last function and its return address, and a sampler thread counts
those of the threads holding a virtual core; every 600 frames the log lists the top functions and call sites
(`prof: ... by last lifted function`, `by call site`). A short function is also credited with its caller's code after
it returns, until the next call. It costs a call per guest call while on.

The hang watch's stacks (`plat_sample_stacks`) come from a stack scan on the PS5: `backtrace` stops at the signal
trampoline there, so the handler lists the values on the interrupted stack that point into the executable, between the
stack pointer and the thread's registration frame (stale values among the callers).

## Test the guest layout on Linux

The opt-in `ps5-high144` layout is available for statically linked host builds. It places the modules at 160 GiB,
the stack and heap at 144–158 GiB, and the alias at 64–128 GiB. The guest still has 39-bit addresses. `GetInfo`
reports an ASLR base of 64 GiB, and `QueryMemory` describes the intervening 128–144 GiB gap as host memory. The
earlier profiles keep their original placements and generated artifacts.
Set `MKW_GAME` to the directory containing `exefs/` and `romfs/`. Use the same profile for generation and the runtime
build. These commands leave the desktop output in its own folder.

```sh
uv run python recomp/gen.py --guest-layout ps5-high144 --exefs "$MKW_GAME/exefs" \
    --out build/layout-ps5-high144/generated --patches recomp/patches-mkw.txt
uv run python tools/build/build.py --guest-layout ps5-high144
build/layout-ps5-high144/mkw --selftest
```

Linux can verify address placement and dispatch using this layout. Range availability and guest memory protection
need separate checks on the console.

The CPU-only PS5 backing module is a first building block for the runtime. It allocates 64 KiB direct-memory units
and accepts protection changes at 16 KiB boundaries. Guest 4 KiB permissions, CPU fault hooks, and GPU-visible
imports still need implementation and testing. For future integration without write-watch, `plat_mem_written`
can return its documented `UINT64_MAX` sentinel. `vk_mem_changed` and `vk_mem_changed_snap` treat the range as
untracked, and `make_fresh` and `make_fresh_snap` in `vk_tex_upload.c` upload mirrored CPU-owned textures again.
`vk_mem_mirror.c` refuses device-local pool mirrors for untracked ranges. The smoke test does not exercise these
renderer paths.

The repository's game-free Linux baseline is `uv run tools/build/build.py --dist --runtime-only`. Keep game inputs and
generated title contents outside version control. The current runtime initializes guest memory before dispatching
probe commands, so it needs linked game module metadata to reach those probes.

## Profile-guided build

**Measured: no real gain** (19/11/2026, [roadmap](roadmap.md)), so releases do not use it. This is how to repeat it.

The PS5 SDK has no compiler-rt profile runtime, so the profile is taken on a PC and used by the PS5 build. The PC build
compiles the PS5's own generated tree (same generator, `ps5-high144`, same patches and chunk file names, by which the
profile names `static` helpers), so the two builds see the same functions. Keep the generated tree at the same absolute
path in both builds. The instrumentation is clang's front end (`-fprofile-instr-generate`): its function hashes come
from the C's syntax tree, so the target, `-march` and `-fPIC` do not change them, but **the clang version does** (23
against the PS5's 21: 22 % of the lifted functions differ). The generated C must be compiled by clang 21 on the PC too.

On Windows, with the real GPU (what the 19/11 test used, from WSL). llvm-mingw's clang is newer, so `--pgo-cc` compiles
the generated C with the host's clang 21 and `--pgo-rt` links a profile runtime of that version: `libprofile21.a`, built
from LLVM 21.1.8's `compiler-rt/lib/profile` for `x86_64-w64-mingw32` with `-DCOMPILER_RT_HAS_ATOMICS=1`.

```sh
uv run tools/build/build.py --target windows --guest-layout ps5-high144 --gen "$PWD/build/ps5-gen" \
  --exefs "$GAME/exefs" --romfs "$GAME/romfs" --pgo-gen /mnt/c/mkw-pgo/raw \
  --pgo-cc "clang --target=x86_64-w64-mingw32 --sysroot=$LLVM_MINGW/x86_64-w64-mingw32" --pgo-rt libprofile21.a
```

Run `mkw-pgo-gen.exe` with this in `mkw.ini`:

```ini
[diagnostics]
INPUT_SCRIPT = race-drive.txt
SEQ_WATCH = 1
SEQ_UNTIL = -WaitIntroMovie,IsSequenceFlag
SEQ_EXIT = 210
SEED = 1
SKIP_MOVIES = 1
```

Then, with clang 21's tools:

```sh
llvm-profdata-21 merge -o build/pgo/mkw.profdata /mnt/c/mkw-pgo/raw/*-exit.profraw
uv run tools/ps5/build_game.py ... --generated "$PWD/build/ps5-gen" --gen-opt O2 --pgo-use build/pgo/mkw.profdata
```

On Linux the host's clang is already 21, so `build.py --guest-layout ps5-high144 --gen ... --pgo-gen build/pgo/raw` is
enough, and `MKW_GPU=null` runs the game's CPU side without the software GPU's stalls.

`race-drive.txt` is an input script of your own: `tools/run/press-race.txt`'s menu presses, then `ReadyRace+N` lines
that drive the race. Those lines need `SEQ_WATCH`, which turns on the function-entry hook (`cpu.h` `FN_ENTER`). The hook
must stay out of the profile: its branch would count as taken in every function and be laid out hot in a build where it
is always off. So `pgo.c` drops everything counted while the hook is on, when `SEQ_UNTIL` turns it off at the race
start, and the log says so. A run that ends with the hook on writes no usable profile: its only file is `*-hook.profraw`,
which the merge above leaves out. `MKW_QUICK_RACE` and `MKW_BENCH` keep the hook on, so they cannot take a profile.

Only a clean end (`plat_exit(0)`) writes the `-exit` stretch; a crash writes nothing. `MKW_PGO_AT` cuts the profile into
stretches (`runtime/core/diag/pgo.c`), which `llvm-profdata merge --weighted-input=W,FILE` can weight. A profile goes
stale when the generator or `runtime/cpu`'s headers change: `build_game.py --pgo-use` prints how many functions clang
found out of date or without data, and those are compiled without the profile. Retake the profile then. `--gen-opt O2`
compiles the generated C at `-O2`; `-O1` stays the default.

## Pit Stop placeholder and PC builder

A redistributable PS5 package cannot ship the full `eboot.bin` (which statically links the recompiled game C),
`spirv/`, `exefs/`, or `romfs/`. Instead:

- **Placeholder title (`PPSA99193`)**: linking `loader-inventory.o` with weak `rt_game_builtin() == NULL` produces a
  game-free `eboot.bin`. On boot, before `mem_init()` or `rt_boot()`, if `rt_game_builtin()` is NULL or
  `ps5_game_files_ready("/app0", RT_GAME_ID)` reports missing/outdated files or `pitstop-game.id` mismatch,
  `ps5_placeholder_run()` displays the Pit Stop missing-files screen (console IP from `sceNetCtlGetInfo`, expected
  version, transfer progress) and starts the `PIT1` TCP receiver on port `19913`.
- **What the builder tells the player first** (`prepare_ps5.py` 'info' lines, kept on the launcher's build page): the
  dump's version, build id and folder, and the versions it holds but does not build; what the PS5 runs and which game
  copies it holds (PIT1I `games`: each folder with its `exefs/main` build id), so it says **First install** or
  **Upgrade** before the build; and once the files are compared, how many changed and how much is sent and kept.
- **Install by FTP without the receiver** (`send_ps5.py send_directory_ftp`): when nothing answers on 19913, the
  builder uses the console's FTP server (2121, then 1337) on the title folder (`mount.lnk`'s, else
  `/data/homebrew/PPSA99193`, made for a first install). ftpsrv 1.16 (23/11/2026) makes files 0777, overwrites on
  `STOR`, replaces on `RNTO`, and `AVBL` gives the drive's real free space. Each file goes to `<rel>.pitstop-ftp`, its
  `SIZE` is checked and it is renamed over the old one, `eboot.bin` last; a file is skipped when the builder's
  `ftp-sent.tsv` or the receiver's manifest has its SHA-256 and `MLSD` shows it unchanged. No SHA-256 on the console
  and nothing on the TV; only named files are deleted (an `exefs/` module the new version lacks).
- **Updating a working install**: with the game files ready, **L1 + R1** held at start (`ps5_update_requested`,
  read from the DualSense's raw reading: held through the launch, the buttons never reach the title as a pad reading; the check takes 11-14 ms, at most 3 s without a reading) opens the same screen as **Ready to receive an
  update**, so a new build can be sent without uploading the placeholder's `eboot.bin` first. `cfg_init()` runs
  before it, as the pad reads `mkw.ini` once when it opens. Cross / Circle closes the screen while no send is under way.
- **Receiver security**: `ps5_recv.c` runs only while the placeholder screen is visible, never starts once the game
  launches, rejects any path leaving the title root (`..`, leading `/`, `\`, symlinks), streams into `.pitstop.part`
  with SHA-256 verification before atomic rename, skips matching files via `.pitstop-manifest.tsv` so runtime updates
  resend only changed files (`eboot.bin`, `mkw.sym`, `pitstop-game.id`), closes the listening socket on `CMD_DONE`, and
  refreshes `/user/app/PPSA99193/sce_sys` when writable. While the PC builds, `PIT1T` reports its step, progress
  or failure for the screen only (`send_ps5.py` `StatusNotifier`, at most once a second, 2 s timeout); it goes only
  to a receiver whose `PIT1I` reply has `"status"`, since an older one answers an unknown command with an error.
- **The RomFS as one file**: the builder writes `romfs.nup` (`tools/ps5/nup_writer.py`, raw NUP v1) in place of
  the ~206,000 loose files, and the title reads it in place (`ps5_game_romfs`: `romfs.nup` before `romfs/`). A
  broken send of it goes on from the receiver's `.part` (`PIT1P` flag 2, the part's `.want` naming size and SHA-256;
  only to a receiver with `"resume"`), and `PIT1X` then removes the loose `romfs/` beside it (`"remove"`). The
  manifest is hash-indexed: a lookup that walked the table cost some 2x10^10 string compares over one loose send.
- **Large files by the console's FTP server**: the title's own writes run at ~20 MiB/s for its first ~2 GB, then
  at ~2 MiB/s for the rest of the process's life (measured 23/11/2026: 3 GiB in 554 s by `PIT1P`, in `/app0` and in
  `/data` alike, one file or two, with or without `fsync`; 151 s by FTP to the same console), so a 23 GB `romfs.nup`
  took some three hours. When an FTP server answers (port 2121, else 1337; `PS5_FTP_PORT`, 0 for none) and the
  receiver has `"ftp"`, files of 256 MiB and up go by FTP into the title folder named by
  `/user/app/PPSA99193/mount.lnk`. `PIT1A` names that folder; the receiver checks it is its own (`eboot.bin` alike
  through it and `/app0`), makes `<path>.pitstop-ftp` there empty, and the builder appends to it (`APPE`). The
  receiver follows the upload (the screen's progress), hashes it as it arrives, then checks it, renames it into place
  and records it in the manifest; a 64-byte probe goes first, and anything that fails sends the file by `PIT1P`
  instead. Measured 23/11/2026: 3 GiB in 161 s (19.1 MiB/s), the check after the upload ~2 s; the next send skipped
  it. The send log gives each FTP file's rate and, for the rest, where the time went (`PIT1I` `"timing"`, also on the
  placeholder screen).
- **`lstat` inside the title**: it fails where `stat` answers (as `ps5_sys.c` already knew), so the receiver uses
  `lstat`, else `stat` (`path_stat`), except for its symbolic-link checks and `remove_tree`. Before, no received file
  was recorded: the console's `.pitstop-manifest.tsv` was 0 bytes and every send sent everything again.
- **The shaders as one file**: `spirv.nup` holds the `.spv`, `.cs.spv` and `.res.json` the title reads
  (`gfx_shader.c` `prepared_read`; the translator's `.done` and `.result` stay on the PC), 23,000 files in place of
  46,000 that went at ~50 a second.
- **A send that breaks leaves a title that starts**: large files go first and `sce_module/`, `sce_sys/` and
  `eboot.bin` last, so the placeholder's own files stay until everything else has arrived. Received files and folders
  are `0777`, as an FTP upload leaves them.
- **The copy already on the console**: `pitstop-game-dirs.txt` in the title folder names more folders that hold the
  game (each with its `updates/<version>/`); `ps5_game_dir` searches them after its own and picks the one whose
  `exefs/main` matches the build. The builder writes it for `--console-game` and sends no game files.
- **The dump in a subfolder**: the builder keeps the dump's folders below the one the player picked (up to two levels:
  `find_games`), so picking `Games/` that holds `MKW/exefs` sends `PPSA99193/MKW/exefs`. `ps5_game_dir` searched only
  the title folder and its `updates/<version>/`, and the placeholder came back after a send that succeeded (two
  players, 25/11/2026). It now searches its subfolders two levels down too, not inside `exefs/`, `romfs/`, `spirv/`
  or `sce_*`; such a player sends only the new `eboot.bin`.
- **SDK-free PC builder**: `tools/ps5/release_ps5.py` packages `ps5-bundle/` (`loader-inventory.o`, prebuilt `app_crt.o`,
  `libSceAgc*.so`, RADV/deps/SDK archives & stubs, linker scripts, and Linux/Windows `ps5-native-tool[.exe]`). The
  desktop launcher (`runtime/ui/launcher/`) and `tools/ps5/prepare_ps5.py` drive the build with pinned `zig` (`zig cc`,
  `zig ld.lld`) on both Linux and Windows from either a dump folder or a `.nup` package (`tools/prepare/nup.py`).
