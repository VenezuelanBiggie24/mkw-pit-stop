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

- **Now: the synchronization calls.** ~16-20 ms a frame of the game threads' time goes to `nn::os::YieldThread`
  (`svc 0x0B`, from the job system's spin loops at `main+0x4520` and `main+0x5660`), `WaitForAddress` and
  `SignalToAddress` (light events and message queues), and the SDK's critical sections. Cheaper SVC paths first.
- **To do: the CPU translator's loops.** The generated C keeps only some guest registers in locals (the rest in
  `c->x[]`) and puts a compiler barrier on every loop's back edge, so a loop reloads its state from memory each turn;
  the barrier is needed only where a loop waits on memory another thread writes. Broadest lever (all game code),
  largest change: the whole game regenerated, `tools/cpu/lift_diff.py` before any run.
- **To do: `main+0x154e14`.** A list walk under a mutex (nodes 0x140 bytes apart, flags at +0x138, removal through a
  virtual call), 6.3-6.95 ms a frame through one race and 1.65 ms in another, steady within a race: why the list is
  that long, and whether it should be.

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
