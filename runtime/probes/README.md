# runtime/probes

Self-tests of each subsystem, one command-line option each (`build/mkw --probe-…`). They come from totk-pc-recomp
except `texpre_probe.c`, `fg_probe.c`, `fps_probe.c` and `discord_probe.c`.

| File | Option | What it checks |
|---|---|---|
| `astc_probe.c` | `--probe-astc` | The compute ASTC decoder (`vk_astc.c`) against the CPU one, bit for bit, with synthetic blocks; BC7/BC1 re-encoding; a level over the 32 MiB chunk recorded in bands, bit-exact and its BC blocks the same as recorded whole |
| `audio_probe.c` | `--probe-audio` | MKW's own: the SDK's translated `nn::audio` client against the objects of `audren_sf.c`: parameter, open, 200 updates at 5 ms, device and close (formerly `--probe-ipc`) |
| `cfg_probe.c` | `--probe-cfg` | Reading the `.ini`, environment variables and a missing file (`cfg.c`) |
| `discord_probe.c` | `--probe-discord` | MKW's own: Discord Rich Presence (`core/presence`) against the Discord running on this machine: connects, shows each phase a few seconds, clears it |
| `fps_probe.c` | `--probe-fps` | MKW's own: `MKW_FPS=auto`'s controller (`fps_auto.c`) on synthetic frame times: the interval each steady frame time settles on with no changes after, a loading hitch, a return to 60, stalls slower than any interval |
| `gfx_abi_probe.c` | `--probe-gfx-abi` | Our objects fit in NVN's and the fields the game reads are where it expects them |
| `gfx_window_probe.c` | `--probe-window` | Window present, vsync, focus, cursor and resize |
| `kernel_probe.c` | `--probe-kernel` | Waits, wakeups, virtual cores and their arguments (cores 0-5), fibers (also one moved between threads) and translated code on four threads |
| `nvn_probe.c` | `--probe-nvn` | The NVN front end over the null back end, called as the game calls it |
| `nvn_tex_probe.c` | `--probe-nvn-tex` | Size, alignment and mip offsets of textures (`nvn_tex.c`) |
| `ov_probe.c`, `ov_probe.h` | `--probe-services` | Each replacement of `runtime/sdk/` called through its synthetic address; all 187 must be bound by some MKW module |
| `ov_mkw_probe.c`, `ov_mkw_list.h` | `--probe-services` | MKW's own: the 96 replacements only MKW imports (offline network, account, `oe`, clock, touch and the VP9 decoder with frames the test encodes itself) |
| `tex_probe.c` | `--probe-vkmem` | Texture memory: block-linear copies (and BC6H region reads against the whole level), ASTC, format table, imported pools; a BC6H region copied from storage against upload + image copy; under forced staging failures, an idle texture surviving the trim of its own re-upload, a deferred first upload bound as the zero stand-in, and a CPU rewrite after a GPU write uploaded; an ASTC copy decoded into a mirrored destination the CPU rewrote since its check, kept (not undone by its re-upload), and refused from a source the GPU wrote; a copy into a sparse tile mapped since the last part, in a batch sent mid-recording, landing in it; ASTC WriteTexels off the 4x4 blocks of a BC7 image writing whole blocks only |
| `texpre_probe.c` | `--probe-texpre` | MKW's own: quality of the GPU BC7 against `bc7enc` on the game's textures, listed in `MKW_TEXPRE_PROBE_LIST` |
| `vk_exec_probe.c` | `--probe-vkexec` | Vulkan replay: fence values, completion order, empty submits; `MKW_RTSTATS`'s rules on cleared targets; a pitch texture the GPU writes, in its pool bytes after the fence wait; which render targets count as rendered recently (one-shot draws); unwritten sparse small mips start at zero across allocation reuse and retain later writes |
