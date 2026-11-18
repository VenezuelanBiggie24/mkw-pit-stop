# PS5 Vulkan smoke title

The game-free `MKW PS5 Smoke` title clears the screen blue for 240 frames at 60 Hz. Its kernel log reports the Vulkan
device and queue capabilities used by MKW and virtual address reservations attempted after the swapchain initializes.
The title tries each layout as a group, holding every exact reservation until the rest of that layout has been tried.
The desktop trial uses its first runtime addresses and requests each range whole. The candidate uses the shared
experimental `ps5-smoke` profile from `recomp/layout.py` with a stack and heap block at 16 GiB and an alias region at
32 GiB. An alternate uses the same fixed windows, with the stack and heap at 32 GiB and the alias at 64 GiB. Both
layouts are tried whole and in pieces at the same addresses. The segmented trial tries the 14 GiB stack and heap
as 4, 4, 4, and 2 GiB and the 64 GiB alias as four 16 GiB pieces. The alias survey uses the range and piece size
documented by Hookshot. [Hookshot's PS5 reservation design](https://github.com/mihawk-99/PS5_Hookshot/blob/a3551c60e425f72461ab771ba39d987ec968ef8d/host/ps5/win32_ps5.c#L76-L94)

The `low36 whole` and `low36 segmented` trials use the `ps5-low36` profile. Both succeeded on the console with
stack and heap at 36–50 GiB and the alias at 64–128 GiB while holding the fixed ranges at 160 GiB.

On the PS5 the probe uses the SDK reservation call with flags zero. The previous console trial's exclusive
`mmap` call replaced the probe's own test mapping. The exclusive comparison runs only in the Linux host reference
build and only after a collision returns `EEXIST` and preserves the probe's marker.

| File | Purpose |
| --- | --- |
| `build_smoke.py` | Generates the standalone title and builds it through the pinned Vulkan Template |
| `build_radv.py` | Applies `tools/ps5/patches/*.patch` to `PS5_Mesa`, rebuilds `libvulkan_radeon.ps5.a` when missing or stale, and records the archive and patch SHA-256 in `PROVENANCE.txt` |
| `build_game.py` | Cross-builds the real runtime for the **Mario Kart World: Pit Stop** (`PPSA99193`) title and reports unresolved imports. `--pgo-use FILE`: the generated C with a profile from a Linux `build.py --pgo-gen` run (objects in `game-objects/lift-pgo`; sums up clang's warnings about functions out of date or without data) |
| `deploy.py` | Uploads the built smoke title (or the game with `--game`), verifies it by SHA-256, publishes it over FTP, and refreshes ShadowMountPlus's registered copy of `param.json` and `icon0.png` in `/user/app/<title>/sce_sys` when there is one |
| `release_ps5.py` | Packs the game-free PS5 placeholder title zip (`mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip`), Linux and Windows self-contained builder packages (`setup/ps5-bundle/` with `zig`, precompiled `app_crt.o`/stubs/`ps5-native-tool[.exe]`/RADV/SDK `.so`s), `SHA256SUMS` and release notes; refuses any asset that holds generated game code, `.spv`, NSOs, RomFS files or lifted symbol names |
| `release_notices.py` | The release's licence notices (`LEGAL.txt`, `licenses/` with each part's licence, copyright, texts and source revision) and `source/*.tar.gz`, the source of every part at the revision built: the PS5 parts link GPL-3.0-or-later code (the payload SDK fork's platform layer, PS5_Vulkan's start-up), so the placeholder and `ps5-bundle/` are GPL-3.0-or-later as a whole |
| `licenses/` | Licence texts the release carries that no checkout on disk holds (LLVM's) |
| `prepare_ps5.py` | Player-side cross-platform (Linux & Windows) PC builder: checks a Mario Kart World dump folder or `.nup` file (`nup_reader.py`) and builds its version (1.8.0 preferred, 1.0.0 too; a console dump's `updates/917504` is found), generates and compiles the lifted C (`ps5-high144`) with `zig cc`, links and fake-signs `eboot.bin` without the PS5 SDK or bash, translates the SM86 shaders into `spirv/` (reusing desktop caches when present), stages `PPSA99193/`, the RomFS as one `romfs.nup` by default (`--romfs nup|copy|skip`), and streams deltas to the PS5 placeholder receiver (`--send <PS5_IP>`), writes the title to a USB drive (`--usb DRIVE`: `homebrew/PPSA99193`, exFAT for `romfs.nup`), or with the copy already on the console sends no game files and names its folder in `pitstop-game-dirs.txt` (`--console-game /data/...`) |
| `nup_reader.py` | The PS5 builder's clean-room `.nup` reader (NUP v1 spec, MIT, a55uka): build id check and unpack of `exefs/` and `romfs/`; the desktop setup uses upstream's `tools/prepare/nup.py` |
| `nup_writer.py` | Writes the staged RomFS as one `romfs.nup` (NUP v1, files stored raw, 16-byte aligned, each with its SHA-256) that the title reads in place (`runtime/sdk/fs/nup.c`): one file to send or copy to a drive instead of ~206,000 |
| `ps5_prepare_common.py` | `prepare_ps5.py`'s dump check (versions from `recomp/versions.py`), copies (removing another version's leftover files), tool runs and SM86 spec step: its own copy of what it used from `tools/prepare/prepare.py` before upstream split that file for several game versions |
| `send_ps5.py` | Cross-platform `PIT1` TCP client (port 19913) that queries console state, checks batches against the console's `.pitstop-manifest.tsv` so updates only send changed files, and streams verified uploads; a large file goes on where a broken send left it (`"resume"`), the loose `romfs/` is removed once `romfs.nup` replaced it (`PIT1X`), and the build's step shows on the placeholder (`PIT1T`) |
| `titlectl.c` | Resident payload for the ELF loader (port 9021) that starts, stops and reports an installed title over one-line commands on port 9031 (`ping`, `status`, `launch`, `kill` <title>): Sony's launch calls tried in turn with a check that the process appeared, SIGKILL by title id to stop (ideas from phantomptr/ps5upload, GPL-3, no code) |
| `bench.py` | Unattended benchmark: builds and sends `titlectl`, appends `BENCH = 1` (and `--set` keys) to the console's `mkw.ini`, kills any running copy, launches, follows `mkw.log` over FTP until `bench: result`, kills on a silent log or timeout, saves `build/run/ps5/<name>-<time>-mkw.log`, restores `mkw.ini`; `--runs N` side by side. `uv run tools/ps5/bench.py run HOST [--runs N]`; `bench.py ctl build \| send HOST \| HOST ping\|status\|launch\|kill` |
| `smoke.cpp` | Clears and presents a frame through the Vulkan Template's sample interface |
| `probe.c` | Logs the selected device's features, queue support and candidate guest memory ranges |
| `memory_probe.c` | Exercises the runtime platform's PS5 backing code on small CPU-only blocks and checks layout coexistence |
| `runtime_probe.c` | Exercises MKW's dispatcher and checks sealing of its host-owned table |
| `query_probe.c` | Optional native smoke check of the real SVC 0x06 output handler and checked guest-span copy-out |
| `pad_probe.c` | Optional live DualSense check using the template's existing pad readings and MKW's real input bindings |
| `alias_probe.c` | Checks direct backing and exact re-reservation of subranges inside the whole PS5 alias reservation |
| `ampr_stub.c` | Link stub naming the libSceAmpr exports the page-table probe imports (`build_smoke.py --page-table` links it as `libSceAmpr.prx`): a title's `sceKernelDlsym` finds nothing on the console |
| `page_table_probe.c` | Measures `sceKernelGetPageTableStats` across sparse image creation/binding, unbound tile readback, 1 GiB GPU-only allocation, 64 KiB vs 2 MiB direct mappings and 2 MiB aliases; reads the AMM VA ranges (`sceAmprAmmGetVirtualAddressRanges`) and the cost of a PRT aperture. Switches of `build_smoke.py --page-table`: `--unmapped-gpu-read` and `--prt-gpu-read` (GPU reads that may end the title), `--amm-pagetable-mib`, `--amm-multimap-gib` (param.json `amm` keys; absent by default) |
| `patches/` | The patches `build_radv.py` applies to the pinned `PS5_Mesa` before it builds RADV ([patches/README.md](patches/README.md)) |

The `low48 coexist` and `high144 coexist` trials try 14 GiB stack and heap blocks at 48–62 GiB and 144–158 GiB.
Each trial holds the entire proposed layout, including the 64–128 GiB alias and the fixed windows, using separate
reservations for the sampled 64 KiB units and the complementary spans. The log reports full exact reservation
separately from backing at the stack's first and last units and at the module, kernel, and alias bases. Only those
five units are candidates for physical backing. The host-function window is only reserved. Both layouts are probe
inputs. `high144 coexist` held every span and backed all five samples on the console. The opt-in `ps5-high144`
profile uses those addresses. If neither full layout reaches its backing checks, the probe tests the module and
kernel samples in isolation after releasing the spans.

The `high144 lifecycle` probe uses the 144 GiB stack and 64 GiB alias bases. It checks zeroed memory on commit,
CPU read and write, queried protection of a 16 KiB page and
its writable neighbor, rejection of a 4 KiB protection request, and zeroing after decommit and recommit while the
alias block retains its contents. The log distinguishes raw native protection and query results from the wrapper's
intentional 4 KiB rejection. It logs `PS5 memory high144 lifecycle backing: passed` after successful cleanup.
The probe uses ordinary reads and writes on the owned blocks, with no deliberately faulting accesses.
Before the native-page lifecycle check, `raw-4KiB` calls the SDK directly on a 4 KiB slice between two 4 KiB
neighbors in one native page. It logs the raw protect result and the queried ranges and flags for all three slices.
The probe then restores and verifies read-write access across the owned 64 KiB block. The separate wrapper rejection
still tests the runtime's current policy, independently of what the SDK reports.
Each commit logs allocation and mapping separately, with the requested length, alignment, and raw SDK result.
For each block independently, `map-any` tries the same allocation at an OS-selected address with flags zero and
releases that mapping before `map-direct` tries the owned reservation. The log shows the physical offset and both
mapping results. A map-any failure still allows the fixed test. If temporary unmapping fails, cleanup runs before
the backing is released.
The backing path requests 64 KiB alignment for allocation and mapping, with protection checked in 16 KiB pages.
[Hookshot's PS5 guest mapping alignment](https://github.com/mihawk-99/PS5_Hookshot/blob/a3551c60e425f72461ab771ba39d987ec968ef8d/host/ps5/win32_ps5.c#L143-L149)

After those checks, the native title exercises the [mixed-page prototype](../../runtime/platform/ps5_mixed.c)
on one owned 64 KiB block at 144 GiB. It verifies guest RW, RO, NONE, and reserved slices in one native RW page,
zeroing after a partial recommit, and preservation of a sibling's data. It queries the native group after all four
slices become RO, then NONE, then RW again. Denied accesses are tested through checked pointers without
dereferencing them. The title requires `PS5 memory mixed-page: passed` before it presents a frame.

The runtime dispatch probe compiles [MKW's dispatcher](../../runtime/cpu/dispatch.c) into the same title. It uses
synthetic module and host callback addresses from `ps5-high144` and tests lookup, execution and register changes.
The native probe queries read-only protection over the dispatcher's 64 KiB host table and logs
`PS5 runtime dispatch: passed` on success. The host reference exercises the same callbacks and seals the table
with Linux `mprotect`. The dispatch table uses native page alignment and does not access guest memory.

The optional QueryMemory probe runs after the existing probes have released their mappings. It reserves two adjacent
64 KiB backing units at 144 GiB, then uses the real `mem_query` and SVC 0x06 output handler to copy a 40-byte result
across the unit boundary. It checks the result registers, null and overflowing pointers, and cross-page writes into
guest RO and NONE pages. Denied writes leave the entire destination unchanged. The title requires
`PS5 QueryMemory: passed` before presentation and releases the test span on either outcome.

The optional `--alias-subrange` build holds the exact `ps5-high144` windows with one 64 GiB alias reservation.
It backs two adjacent 64 KiB slots at 65 GiB and checks that their reserved neighbors remain inaccessible.
It unmaps and re-reserves the first slot while the second stays mapped, then verifies zeroing on recommit
and preservation of the second slot's data. The log records SDK statuses, returned addresses, and protection
ranges. If exact re-reservation fails, cleanup excludes the lost slot and the probe fails before any remap.
The native title requires `PS5 alias subrange: passed` before presentation.

## Build on Linux or WSL

### Native game target

Use the pinned SDK, Vulkan and Mesa checkouts below. The compiler needs the Vulkan, SDL3, Opus, libvpx, zstd and
xxHash headers plus the VMA header described in [the desktop build notes](../build/README.md). Pass portable
dependency header directories using `--include`. The SDK provides the target's system headers.

```sh
uv run tools/ps5/build_game.py --stack "$stack" --include "$headers" --inventory
uv run python recomp/gen.py --exefs "$MKW_GAME/exefs" --guest-layout ps5-high144 \
    --out build/ps5-game/generated --patches recomp/patches-mkw.txt
uv run tools/ps5/build_game.py --stack "$stack" --include "$headers" \
    --generated build/ps5-game/generated --exefs "$MKW_GAME/exefs"
```

`--inventory` compiles runtime units without game inputs and writes full and loader-reachable symbol inventories
in `build/ps5-game/inventory-objects/`. The private build compiles the local lift into the title with its built-in
module tables and writes the corresponding reports under `game-objects/`. Any extra native dependency archives
are passed with `--library`. The loader stage currently depends on unresolved runtime platform and library APIs.
If the link succeeds, the packager writes `build/ps5-game/title/dist/PPSA99193/` and stages the verified ExeFS
inputs there. The title's checkpoint is the `PS5 loader: verified, relocated and sealed` log before guest constructors.

### Smoke toolchain and package

Use Clang 18 or newer and the host dependencies required by [PS5_Vulkan's build](https://github.com/mihawk-99/PS5_Vulkan/tree/7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc).
The host `mesa_clc` build worked with LLVM and Clang 22.1.8, LLVM/SPIR-V Translator 22.1.8, SPIRV-Tools 2026.4.1,
and glslang 16.6.0. [Khronos translator release](https://github.com/KhronosGroup/SPIRV-LLVM-Translator/releases/tag/v22.1.8)

Use matching LLVM, Clang development headers and libraries, and `LLVMSPIRVLib` major and minor versions. Put that
LLVM's tools and `glslangValidator` on `PATH`. For a local install, add the translator and SPIRV-Tools pkg-config
directories to `PKG_CONFIG_PATH`. Set `LLVM_CONFIG` to the chosen `llvm-config` and `PS5_CLANG` to the matching
`clang` when multiple LLVM versions are installed.

Place these sibling checkouts in a local directory, using the names in the table. The revisions come from
[Hookshot's PS5 beta](https://github.com/mihawk-99/PS5_Hookshot/blob/ps5-v0.1.0-beta.1/ps5/build-title.sh) and the
template's own dependencies.

| Checkout | Revision |
| --- | --- |
| [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) | `91179df760df1e6c6f2d4d172bc435319d79ff3c` |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | `7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc` |
| [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) | `0f86253f674c425052948eb3cb116c81e646a734` |
| [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) | `611893fc25ef3e138f5bc2c90d829d8b58174cd2` checked out, with `b83202be73e930050e00de0f1b4d9ec46c0391bf` also available in its history |

From the MKW repository, with `stack` set to the absolute path of that directory, prepare the local dependencies and
build the smoke title.

```sh
git -C "$stack/PS5_VulkanTemplate" submodule update --init external/glm
git -C "$stack/PS5_PayloadSDK" fetch origin b83202be73e930050e00de0f1b4d9ec46c0391bf
(
    set -e
    cd "$stack/PS5_Vulkan"
    bash tools/setup-native-dependencies.sh
    make || { test -x build/host/ps5-native-tool && test -s runtime/libc.prx; }
    PS5_MESA_REVISION=0f86253f674c425052948eb3cb116c81e646a734 bash tools/build-radv.sh release
)
uv run python tools/ps5/build_smoke.py --stack "$stack"
```

`make` also builds PS5_Vulkan's demo. The smoke title uses the generated `ps5-native-tool` and `libc.prx` from that
step and can proceed once both exist.

The output is `build/ps5-smoke/title/dist/PPSA99192/`. The build stages the template's license notices with the title.
Add `--alias-subrange` to build the ownership experiment under `build/ps5-smoke-alias/title/dist/PPSA99192/`.
`--prepare-only` requires just the pinned template and GLM submodule and generates the sample sources without a console
toolchain. Generated files and the template's isolated Git project stay under `build/ps5-smoke/`.

Add `--query-memory` to build the optional native probe. It writes to
`build/ps5-smoke-query/title/dist/PPSA99192/` with the same title ID. The probe's C objects use Clang 22 against the
pinned title SDK. Set `LLVM_CONFIG` to the desired LLVM 22 `llvm-config` executable if `llvm-config-22` is outside
`PATH`. The probe uses the runtime's memory query code with an empty guest module and thread state.

Add `--dualsense` to build the optional input probe in `build/ps5-smoke-dualsense/title/dist/PPSA99192/`.
It uses the same Clang 22 toolchain. Once the frame appears, release Cross, then press and release it within
900 frames (about 15 seconds at 60 Hz). The frame turns green while Cross maps to MKW's A button and returns to blue
on release. The kernel log records the mapped A transitions, stick values, and `PS5 DualSense input: passed` or
`failed` at the end. An intercepted or disconnected reading clears held input and cannot count as a release.

## Upload to the console

Start the PS5's FTP server with anonymous access. Use the console's IP address and set `PS5_FTP_PORT` if the server
uses a port other than `2121`. The `app` argument is optional.

```sh
python3 tools/ps5/deploy.py 192.168.1.50 app --dry-run
python3 tools/ps5/deploy.py 192.168.1.50 app
```

Select one optional package using the same flag as the build command. All packages publish to the same console title
ID and retain the previous title in the backup folder.

```sh
python3 tools/ps5/deploy.py 192.168.1.50 app --query-memory --dry-run
python3 tools/ps5/deploy.py 192.168.1.50 app --dualsense --dry-run
python3 tools/ps5/deploy.py 192.168.1.50 app --alias-subrange --dry-run
```

Remove `--dry-run` to upload the selected package.

The dry run validates the local package and prints the plan without connecting. An upload stages the files under
`/data/homebrew/.mkw-smoke-PPSA99192-stage-<unique>` and reads each one back for SHA-256 verification before publishing
`/data/homebrew/PPSA99192`. An update retains the previous folder under the printed
`.mkw-smoke-PPSA99192-backup-<unique>` name. If the final rename is rejected, the uploader restores the prior folder
when the destination is free.

If a connection loss leaves publication unconfirmed, inspect the printed title, stage and backup paths over FTP.
If the title folder is absent and a backup exists, restore the backup to `PPSA99192`. Remove the empty
`.mkw-smoke-PPSA99192-publish` lock only after confirming no upload is publishing and the title folder is in the
intended state.

## Console result

Launch the title in an existing PS5 homebrew environment. Look for a blue frame followed by the title exiting, and save
the `MKW PS5 Smoke` kernel log. Record which PS5 model ran the test. The Vulkan report includes both the requested
instance API and the selected GPU's API and driver versions. The reservation report prints requested ranges, returned
addresses and error codes, then summarizes the first requested address and size that did not match. `relocated`
means the hint was not honored. The probe releases those reservations immediately and releases the exact reservations
after each trial. Compare `low36 whole` with `low36 segmented` for request-size effects. An exact result is scoped
to this title's process and load order. The subsequent `PS5 memory` lines report backing and protection separately.
