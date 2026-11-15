# Initial research (30/10/2026)

Game copy: `exefs/` 40 MB + `romfs/` 23 GB, 205 781 files. Program id `0400C3F00006E000`.

## The executable

Measured with the `recomp/nso.py` loader (from totk-pc-recomp) on the decompressed NSOs.

| Module | Text | Image | Dynamic symbols | What it is |
|---|---|---|---|---|
| `main` | 42 MB | 54 MB | 1 289 imported, 151 exported | the game |
| `sdk` | 7.8 MB | 14 MB | 27 427 exported | SDK 19.3.3, `spec: Ounce` (Switch 2), NVN2 driver inside |
| `subsdk0` | 2.6 MB | 3.6 MB | 216 imported | Mono (.NET) runtime; `main` imports nothing from it except `nn::ecli::Allocate/Deallocate` |
| `rtld` | 8 KB | 64 KB | — | dynamic linker; not needed in a recomp |

- The same NSO0 (LZ4) and MOD0 format as Switch 1. `main.npdm`: Switch 2 ACID (signature area 0x300, not 0x200),
  cores 0-5 (mask 0x3f), new services `banana`, `audsmx:u`, `ngc:u`, `notif:a`, `ectx:aw`.
- `main`'s imports by namespace: `nn::audio` 234, `nn::os` 127, `nn::codec` 77, `nn::hid` 57, `nn::movie` 52,
  `nn::util` 48, `nn::async` 44, `nn::socket` 32, `nn::fs` 27… NVN2 comes in through `nvnBootstrapLoader` +
  `nv::InitializeGraphics` (as NVN1).
- NVN2: ~701 function names in `sdk` (`CommandBuffer*` 202, `Texture*` 97, `Device*` 61, `Dlss*` 54, ray tracing
  acceleration `CommandBufferBindAccelerationStructure*`). Approximate regex.
- Engine: Nintendo EPD's. In `romfs/`: `RSDB`, `Phive`, `Banc`, `AI` (`.ainb`), `GymlArchive`, `Lib/{agl,gsys,sead}`,
  shaders `*.Nin_Ounce_NVN2.bfsha.zs`, 68 % of the files as `.zs` (zstd). In `main`: `sead::` 16 strings, `agl::` 40,
  `gsys::` 23, `phive` 311, Havok `hknp` 1 100, `ainb` 85, `gyml` 119. No RTTI.

## Switch 2 hardware (T239)

- CPU: 8 × Cortex-A78C (6 for the game), ARMv8.2 + PAC (v8.3/v8.6). ~1 GHz.
- GPU: Ampere, 1 536 CUDA cores (12 SMs, own count), RT and tensor (DLSS). Shaders in SM86 SASS.
- 12 GB LPDDR5X, 102 GB/s.
- Sources: [Switchbrew T239](https://switchbrew.org/wiki/Switch_2:_Tegra_T239),
  [TechSpot](https://www.techspot.com/news/107922-nintendo-finally-clarifies-switch-2-custom-nvidia-processor.html).

Instructions in `main` that a Switch 1 lifter lacks (count from eden-nx2, `NX2_NOTES.md` row "Titles"): RETAA/B
114 767, LDAPR 37 560, LSE atomics 24 718, CAS 6 640, SWP 2 470, CASP 6, XPAC 7, PAC 2, STG 1. PAC is treated as
identity and MTE tags as 0 (eden-nx2, row 3).

## Sources

Every source used, with its license and what is taken from it. From MIT sources code may be taken, citing it; from
GPL ones and from those without a license, only ideas.

| Source | License | What is used |
|---|---|---|
| [Takoroka/eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2), branch `eden-nx2` (50fa86bb, 29/10/2026) | GPL-3.0 | **The first to run MKW** (an emulator): title, Mushroom Cup movie and race, ~17-25 ms/frame on an RTX 4070 laptop. `NX2_NOTES.md`: 149 blockers with cause and fix: protocol of the new services (`gsv`, `vi2`, `syncpt`, `nvsrv`, `gpu` 101/211/212/214/215), memory (12 GB, sparse regions of ~20 GB), Ampere GPU (classes C797/C7C0/C7B5, Turing MME64 macros, QMD 3.0), lowering Ampere SASS to Maxwell (`shader_recompiler/frontend/ampere/lower_to_maxwell.h`, 2 249 lines), ASTC with Switch 2's swizzle, VP9 video. Taken as ideas only: Maxwell's `InstanceId` is `InstanceIndex - BaseInstance` in Vulkan → `sm86/translate/glsl/mem.py`; an `LDG`/`STG` whose base is not tracked searches its buffer among the program's at run time → `global_search` in `sm86/translate/glsl/mem.py`; "read traps" (what the GPU writes reaches game memory when the game touches it) → `mem_trap` in `runtime/core/memory/memory.c`, our own code; the alpha test as a discard in the FS before writing, with function and reference fixed in the pipeline → `spirv_alpha_test` in `runtime/gfx/vk/pipe/vk_spirv_patch.c`, our own code |
| [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) 779bdcf (10/11/2026) | GPL-3.0 | Ideas only. Its update offer (`host/hos_update.c`): at start, GitHub's latest release (a few seconds at most, nothing offline); when it differs from the one the folder was built with, the player is asked, and yes rebuilds the folder from the game files it keeps → the launcher's update check, our own code |
| [tangerineturtle1/NX2navi](https://github.com/tangerineturtle1/NX2navi) f58ad1d (09/11/2026) | MIT for the imported base only (0478c13); later additions without a license (`LICENSE-ADDITIONS`, "All rights reserved"): ideas only | A Switch 2 Ocarina of Time recomp on totk-pc-recomp (NVN2 over Vulkan). Reviewed in full (09/11/2026): six bugs of our translator, five Linux kernel and fiber bugs inherited from totk, a Linux package with an old sysroot (MIT base `tools/make_release_linux.py`), differential tests of the lifter and `pick_jobs` (MIT base), a robust setup. The MIT equivalents are taken from up-to-date totk-pc-recomp |
| [STS78721/MKW-THWOMP](https://github.com/STS78721/MKW-THWOMP) (09/11/2026) | GPL-3.0 as a whole (Eglantine's translator); what it inherits from totk-pc-recomp, MIT | Another MKW recomp (1.8.0) for Windows: a fork of DKB Recomp, ported in a day; intro only. Reviewed (09/11/2026): push constant bank 24, compute c0 table (fixed here), audio: ideas only |
| [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) c3b9a4e | MIT | Same engine. AArch64→C lifter in Python (`recomp/`, from LAS: registers in locals, symbolic jump tables, LDXP/STXP), SDK replaced by `nn::*` function name, virtual cores with preemption (`runtime/kernel.c`), the game's zstd to libzstd (`ov_zstd.c`), NVN1→Vulkan (`vk_*.c`), spin loops (`tools/spin_waits.py`). See its `docs/architecture.md`. Taken (design, our code): the GPU breadcrumbs of `runtime/vk_crumbs.c` (710b7f5) in `runtime/gfx/vk/diag/vk_crumbs.c` |
| [totk-pc-recomp#60](https://github.com/valentderah/totk-pc-recomp/pull/60), branch `wdiasjunior/linux-port` @ eccf4dd | MIT | Linux port of totk-pc-recomp's runtime: `platform/linux.c` (futex, userfaultfd, signals, fibers), SDL3, AppImage. Tested on Arch + RADV at 60 fps. The base of this repo's runtime. `tools/crit_path.py` (critical path of `MKW_CRIT`) → `tools/run/crit_path.py` |
| totk-pc-recomp `tools/make_release.py` and `tools/build_game.py` (c3b9a4e / eccf4dd) | MIT | Windows build: llvm-mingw plus the MSYS2 UCRT64 packages with their sha256 (`PINS`) and the link libraries (`LLVM_LINK_LIBS`) → `tools/build/windeps.py` and `build.py --target windows`, cited |
| totk-pc-recomp `tools/make_release_linux.py` (ae30a61) | MIT | The Linux release: built against the Steam Runtime 3 "sniper" SDK sysroot (the same pinned snapshot), zstd built static, and packed as an AppImage with appimagetool 1.9.1 and the static type-2 runtime 20251108, each pinned by sha256 → `tools/build/sysroot.py` and `tools/build/release.py`, cited |
| [Steam Runtime 3 "sniper" SDK](https://gitlab.steamos.cloud/steamrt/sniper/sdk) sysroot 3.0.20261021.266025, [SDL](https://github.com/libsdl-org/SDL) 3.4.18, [AppImage](https://github.com/AppImage/appimagetool) tools | Debian packages' own licenses (Opus BSD-3, libvpx BSD-3, glibc LGPL, only linked against) / Zlib / MIT | The Linux release links against the sysroot (glibc 2.31) and bundles its Opus and libvpx, and SDL3 built from source against it; their licences go into the AppImage's `licenses/` |
| totk-pc-recomp `installer/` (c3b9a4e) | MIT | An installer over a CLI that emits one event per line, steps with a key to resume, a portable folder with rotated `logs/`: the model of `tools/prepare/prepare.py` and of how `runtime/ui/launcher/launcher_setup.cpp` reads it |
| [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261020 (Linux build) | Apache-2.0 with LLVM exception; permissive mingw-w64 parts | The toolchain that builds `build/mkw.exe` from Linux (clang 23, lld, UCRT, libc++); no code copied |
| MSYS2 UCRT64 (`opus`, `libvpx`, `zstd`, `xxhash`, `vulkan-headers`, `vulkan-loader`, `vulkan-memory-allocator`) and SDL 3.4.18 (`SDL3-devel-…-mingw`) | BSD-3 / BSD / BSD / BSD-2 / Apache-2.0 / Apache-2.0 / MIT / zlib | Windows headers, import libraries and DLLs of the same libraries linked on Linux (`build/win/deps`) |
| [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx) (copy in totk-pc-recomp `tools/vendor/ryujinx`, 1.3.2) | MIT | Semantics of NVIDIA shaders for the SM86 translator: SPH header (`Translation/ShaderHeader.cs`), `IPA` with perspective and `gl_FragCoord.w` (`Instructions/InstEmitAttribute.cs`), `LOP3`'s predicate (`InstEmitIntegerLogical.cs`), `TMML` (`InstEmitTexture.cs`), global memory as storage buffers (`GlobalMemoryToStorageBufferPass`). It is Maxwell: what is Ampere's is checked against Mesa NAK |
| Ryujinx `Ryujinx.Graphics.Gpu/Image/Texture.cs` (475615f) | MIT | Only the idea: without native ASTC, decode and recompress to BC7 (`EnableTextureRecompression`). The mode 6 BC7 encoder of `runtime/shaders/bc7_encode.comp` is our own |
| [richgel999/bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (b9438627) | MIT (`bc7e.ispc`: Apache-2.0, not used) | Code: `bc7enc.cpp` as is in `runtime/third_party/bc7enc`, the offline BC7 encoder of `vk_texpre.c` (modes 1, 5, 6, 7) |
| [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler) (`nak_nir_lower_tex.c`) | MIT | The free NVIDIA compiler for SM70+ (Volta-Ada, Ampere included): `TEX` operand order (group 0 = array index and coordinates, group 1 = LOD/bias, offset, comparison) and the scalar `.SCR` form; encoder and semantics of each instruction |
| smo-recomp (Coz-0, ssbu-recomp lineage) | GPL-3.0 | A sibling recomp by the same author: lifter with a differential test against Unicorn, NVN→Vulkan runtime with its own Maxwell translator, measured improvements. Ideas only |
| SavageWindRecomp (BotW, LAS runtime) @ 663cff16 | MIT | A sibling recomp by the same author. Taken: the structured control flow of its shader translator (`structure_flow`, in `sm86/translate/flow.py`); `FMNMX` and `.SAT` as GLSL.std.450 `NMin`/`NMax`/`NClamp` (`runtime/nvn/maxwell/translate.c`, in `sm86/translate/glsl/common.py` `NMINMAX_FN`); its Windows build docs as a comparison |
| [Revivedx/SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp) (`main` @ 5173d27) | MIT | Start screen and building the game on the player's machine: `runtime/ui/launcher.mm` (pages, game files, version check by the NSO build id and the RomFS file count), `runtime/gamelib.*` (game code as a library with a single entry by name and a header id) and the zig options of `runtime/gamebuild.c`. The idea and those options are taken (`runtime/ui/launcher/launcher.cpp`, `runtime/core/boot/gamelib.c`, `tools/prepare/prepare.py`) |
| SavageWindRecomp `runtime/nvn/glyphs.c` (branch `exp/recorder`) | MIT | Button icons matching the controller by redrawing the game's icon sheet on upload: the idea and the signed-distance drawing, text with stb_truetype and the Xbox, PlayStation, keyboard and mouse icons → `runtime/ui/glyphs/glyphs.c` and `glyph_draw.c`, adapted to MKW's RGBA sheet (PicFont) |
| [seedds/nxnative](https://github.com/seedds/nxnative) | MIT | Runs native ARM code on Apple hardware (no CPU translation). **Also runs MKW** (2a6f9b3, 09/11/2026: NVN2 over Metal on Mac, tested per its README); C#, MSL and Python, more ideas and test cases than code. Its spin loop search is taken (through totk-pc-recomp: `tools/cpu/spin_waits.py`). A private fork by Will Faust runs MKW on Mac ([XDA, 27/10/2026](https://www.xda-developers.com/switch-2-mario-kart-world-running-on-mac-nintendo-probably-isnt-thrilled/)): code not published |
| [0xNikilite/oboromi](https://github.com/0xNikilite/oboromi), fork [jopdorp/kokoromi](https://github.com/jopdorp/kokoromi) | GPL-3.0 | A Switch 2 emulator in Rust: SM86 decoder generated from DocumentSASS, SM86→SPIR-V translation. Ideas only |
| [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), [Wael3rd/Eglantine-MKW](https://github.com/Wael3rd/Eglantine-MKW) | GPL-3.0 | Derived from eden-nx2; the MKW one targets 1.8.0. Reviewed @ e96917cd0e (01/11/2026). Taken as ideas: Switch 2's tick counter runs at 31.25 MHz (checked in our `sdk`, `GetSystemTickFrequency` at `sdk+0x145d00`) → `RT_TICK_HZ`; measuring how much of the sparse tiles ever gets written → `vk_sparse_written` |
| [Ca1nM/mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor) (b12cbe6, 08/11/2026) | MIT | Editor of MKW 1.0.0's `account.sav` (Product.100): lists of drivers, vehicles and stickers, their states and the Special Cup. Its offsets and id list are taken (`runtime/core/hacks/unlock.c`, `unlock_ids.c`) |
| [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) (a598b9f, 08/11/2026) | GPL-3.0 | A Switch 2 Ocarina of Time recomp (Windows and Linux), **a fork of another, unpublished MKW recomp** whose `nx2/`, `host/` and `gpu/vk/` it carries whole: races at 45-55 fps on an RTX 4060 Ti. Its MKW is another version (build id `d00e4f39…`, SDK 21.4.0; ours `9e244438…`, 1.0.0). Ideas only |
| [mihawk-99/PS5_Hookshot](https://github.com/mihawk-99/PS5_Hookshot/tree/ps5-v0.1.0-beta.1) (a3551c6) | GPL-3.0 | The PS5 beta provides the Vulkan and SDK pins and documents the driver's virtual address layout. Its four 16 GiB reservations in `host/ps5/win32_ps5.c` inform the smoke test's alias trial at 64 GiB. Ideas only |
| [Atmosphère QueryMemory](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/source/svc/kern_svc_query_memory.cpp#L35-L42) (6b0c077) | GPL-2.0 | Ideas only. The output pointer check and [copy-fault result](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libmesosphere/include/mesosphere/svc/kern_svc_k_user_pointer.hpp#L79-L87) inform the PS5 fixture's `InvalidPointer` result. [Result definitions](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libvapours/include/vapours/results/svc_results.hpp#L18-L47) |
| OcarinaPS5 local checkout (9c97f28) | Design reference | The PS5 no-replace `mmap` fallback in `third_party/recompia/runtime/src/platform/hostvm_posix.cpp` informs the smoke test's guarded comparison of reservation APIs. The `app` workflow in `tools/deploy.sh` informs the smoke title's FTP command. Ideas only |
| [mihawk-99/PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate/tree/91179df760df1e6c6f2d4d172bc435319d79ff3c) (91179df) | MIT foundation | The PS5 smoke sample uses the template's base class and its generator and build scripts. `ps5/tools/new-title.py` configures `sce_sys/param.json` (`kernel.flexibleMemorySize`). The generated foundation and its linked dependencies stay under `build/` |
| [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (`7f492b4c`), [PS5_PayloadSDK](https://github.com/john-tornblom/ps5-payload-sdk) | MIT / GPL-3.0 (reference only) | `PS5_Vulkan/tooling/native/sce_module_writer.cpp` (`build_process_parameters`, `build_parameter_blocks`) and `PS5_PayloadSDK/samples/install_app/make_fself.py` (`PT_SCE_PROCPARAM = 0x61000001`) document the `ORBI` process-parameter segment (`0x38`-byte `SceKernelMemParam` block, zeroed in `ps5-native-tool`); `PS5_PayloadSDK/sce_stubs/libkernel.c` exports `sceKernelMapDirectMemory2`, `sceKernelWriteMultiMapCommand`, `sceKernelWriteMultiMapWithGpuMaskIdCommand`, and `sceKernelMapperSetPageTablePoolOccupancyNotificationThreshold`. `sce_sys/param.json` `amm.pagetableMemorySizeInMib = 256` was tried and removed because it did not change the 16,384-unit CPU pool: see [ps5.md](ps5.md#page-table-memory) |
| [0xD0GF00D/DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS), [reoLantern/nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler) ([arXiv 2609.18662](https://arxiv.org/abs/2609.18662)) | MIT | The SASS encoding specification taken from `nvdisasm` itself (sm_80/sm_86/sm_89): fields per instruction and a decoder validated against nvdisasm on 3.78 M instructions. The base of the SM86 decoder (`sm86/decode`); `intercept.c`, `funnel.py`, `parse_sidl.py` and `sidl_*.py` copied unchanged into `sm86/spec`, which builds the spec on the player's machine. Needs CUDA 12.8's `nvdisasm` |
| [cloudcores/CuAssembler](https://github.com/cloudcores/CuAssembler) | MIT | An SM86 SASS assembler that learns the encoding from nvdisasm/binary pairs |
| [AnyPS5 #6528](https://github.com/boykopovar/AnyPS5/issues/6528) (PR #9356) | GPL-2.0 | Only the idea, for task/mesh as compute: without `VK_EXT_mesh_shader`, the mesh shader is compiled as a compute shader that writes vertices and triangles (the extra ones degenerate) to buffers, drawn by index with a generated vertex shader. Measured there: 15 of 20 games on a Radeon 680M without mesh shaders |
| [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) (1d1ff111, 14/11/2026): `core/libs/prx/libSceAmpr/include/Api.hpp`, `core/libs/prx/libkernel_sys/include/Api.hpp`, `core/libs/prx/libkernel/include/Api.hpp`, `core/libs/prx/libkernel/Apr/include/AprCommandBuffer.hpp` | GPL-2.0 | Names and signatures only, for the PS5 page-table probe (`tools/ps5/page_table_probe.c`): libSceAmpr's AMM API (`sceAmprAmmGetVirtualAddressRanges`, `GiveDirectMemory`, `CommandBuffer{Map,MapDirect,MultiMap,MapAsPrt,Unmap,...}`, `SubmitCommandBuffer2`, `WaitCommandBufferCompletion`), libkernel's AMM command writers (`sceKernelWrite*Command`, `sceKernelCallIndirectBuffer*`), `sceKernelSet/GetPrtAperture`, `sceKernelQueryMemoryProtection`, `sceKernelLoadStartModule`/`sceKernelDlsym`. An emulator's reading: its own code calls nothing on a console, so a layout is used only where a second source agrees |
| [mattias800/prosper](https://github.com/mattias800/prosper) (e35a74a2), [PR #2875](https://github.com/mattias800/prosper/pull/2875), `prosper/src/hle/memory/hle_kernel_mem.cpp` | none (ideas only) | The second source for the AMM layouts, derived from a retail title's SDK wrappers and a live argument log (Yakuza Kiwami, PPSA31334): `sceAmprAmmGetVirtualAddressRanges` writes four 64-bit slots (window start and end; the last two unknown to it), `GiveDirectMemory(start, end, size, alignment, type, out)`, `AmmCommandBufferMap(cb, va, size, type, protection)` with type 0xb and protection 0xc3/0xf3, a one-argument AMM constructor, `SubmitCommandBuffer2(base, used bytes, flags, u32 *, u32 *id)`, `Wait(id)`. That title runs its whole heap through AMM (512 GiB of VA asked for, 10 GiB given). Its `sceAmprCommandBufferSetBuffer` (NID `N-FSPA4S3nI`) sees six registers and two "flavours" where AnyPS5 has three arguments: that disagreement is why the probe writes no AMM command |
| [DNNDHH/PS5-3.20_Libs](https://github.com/DNNDHH/PS5-3.20_Libs) (b2a77c09), `libSceAmpr.c` only | none | Read only the generated stub file: libSceAmpr's 91 export names with their NIDs (firmware 3.20). Every name matches its NID computed as SHA-1(name + the fixed suffix), first 8 bytes little-endian, base64 with `+`/`-`. The repository also holds decrypted firmware libraries: not downloaded or used |
| [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) (b97bafa3), `src/core/libraries/kernel/memory.cpp` | GPL-2.0 | Signatures only: `sceKernelSetPrtAperture(int id, address, size)`, `sceKernelGetPrtAperture(int id, address *, size *)`; three apertures inside 0x10_0000_0000 + 0xec_0000_0000 on the PS4 (NIDs `BohYr-F7-is`, `L0v2Go5jOuM`, which match the names) |
| [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (7f492b4), `docs/HARDWARE_FINDINGS.md` (2026-09-18, 2026-09-27) | GPL-3.0 | A finding, no code: a title's `sceKernelDlsym` answers `ESRCH` (0x80020003) in every form (name, NID, `NID#library#module`) on every module, and a title reaches a console module only through imports linked into it and bound at start; an import of a module the loader does not load for a PS5 title stays null. Our probe run 7 agrees (`sceAmprAmmGetVirtualAddressRanges` by name: 0x80020003). Hence `tools/ps5/ampr_stub.c`, linked as `libSceAmpr.prx` the way the template links its AGC stubs |
| [prosper PR #4153](https://github.com/mattias800/prosper/pull/4153) | none (ideas only) | A retail PS5 title (Assassin's Creed Black Flag Resynced) calls `sceKernelSetPrtAperture` at boot, so the call is live on the PS5; three slots, EINVAL outside 0-2. It found no public source for the PS5's aperture address range or alignment |
| shadPS4 (b97bafa3) `src/core/libraries/kernel/process.cpp`, `src/core/module.h` | GPL-2.0 | Signatures only: `sceKernelGetModuleList(int *, u64, u64 *)`, `sceKernelGetModuleInfo(int, info)` with the 0x160-byte info (size first, 256-byte name at +8), as AnyPS5's `core/libs/SceTypes.hpp`. And the checks of `sceKernelSetPrtAperture` in `memory.cpp`: id 0-2, address 4 KiB aligned, range inside 0x10_0000_0000 + 0xec_0000_0000 (the PS4's PRT area) — the probe tries apertures there |
| [psdevwiki: PS5 Param.json](https://www.psdevwiki.com/ps5/index.php/Param.json) | wiki | `amm.multimapVaRangeInGib` (optional integer, example 512): the key behind the kernel's "MultiMap VA range size" (`build_smoke.py --amm-multimap-gib`). Read through a search engine's copy: the page answers 403 to scripts |
| [Dolphin, hybrid ubershader mode](https://dolphin-emu.org/blog/2017/07/30/ubershaders/) ([PR #5702](https://github.com/dolphin-emu/dolphin/pull/5702)) | GPL-2.0+ | Only the idea, for specialized shaders: draw with the generic shader while the specialized variant compiles in the background, and switch to it when ready |
| [Mesa environment variables](https://docs.mesa3d.org/envvars.html) | MIT (docs) | RADV RGP capture: `MESA_VK_TRACE=rgp`, `MESA_VK_TRACE_TRIGGER`, `MESA_VK_TRACE_FRAME` |
| [Switchbrew](https://switchbrew.org/w/index.php?title=Category%3AServices) | CC BY-SA | The "Switch 2: GSV / VI2 / Host1x / Nvkernel services" pages and the `[S2]` commands of `appletOE`. Only needed if something of the translated SDK calls them |
| [Axiom4912/Axiom](https://github.com/Axiom4912/Axiom), [hffgzktbcc-blip/ounce-core](https://github.com/hffgzktbcc-blip/ounce-core) | — / MIT | Discarded: a README without code; a research runtime that only runs Switch 1 homebrew |

### Reference copies (`scratchpad/ext/`, cloned again when missing)

| Copy | Commit | Where to look |
|---|---|---|
| `eden-nx2` | 50fa86bb | `NX2_NOTES.md` (blockers with cause and fix) before the code; `src/shader_recompiler/frontend/ampere/`, `src/video_core/host1x/nx2_video_decoder.cpp` |
| `totk` (totk-pc-recomp) | ae30a61 | `runtime/` (kernel, `nvn_procs.inc`), `tools/` (`spin_waits.py`, `shader_census.py`, `xlate/`) and `tools/vendor/ryujinx/src/Ryujinx.Graphics.Shader` (Ryujinx 1.3.2) |
| `totk-linux` (PR #60) | c626cda | `runtime/platform/linux.c`, `tools/build_game.py`, `tools/romfs_index.py` |
| `ryujinx` | a61a426 | `src/Ryujinx.Graphics.{Shader,Gpu,Texture,Vulkan}` |
| `nvsass-disassembler` | 28f9064 | `DocumentSASS/`, `parse_sidl.py`, `sass_decode.py` |

No public NVN2 documentation, decompilation, symbol map or exefs patches of MKW were found (30/10/2026).

## NVN2 against NVN1 (measured)

- NVN2's name table in `sdk` (0xb74c26-0xb79b76): 699 functions. Against the NVN1 one of the Switch 1 game
  (`totk-pc-recomp/runtime/nvn_procs.inc`, 537): **529 with the same name**, 170 new, 8 of NVN1 gone (debug markers
  and `*NvRm*NVX`). That the signature did not change in those 529 was a hypothesis, checked as the calls were
  translated.
- New ones by group: DLSS 54, `ProgramBuilder` 20 (programs are created with a builder), mesh/task
  (`DrawMeshTasks*`), ray tracing (`*AccelerationStructure*`, `DispatchRays*`), push constants, device messages, HDR
  in `Window*`, variable rate shading, exclusive scissor.
- `main` holds all 700 names (the loader's whole table); which ones it really calls is measured from the references
  of the translated code.
- DLSS: `main` creates its network with `nvnDlssNetwork*` and evaluates it with `nvnCommandBufferEvaluateDlss`. It
  sits behind an option, off by default; an AMD GPU has no host DLSS.

## Shaders

`romfs/Shader/`: 9.3 MB compressed, 55 MB uncompressed. Six `DriveRefreshUber.*.bfsha` (FSHA with a BNSH inside, four
in `Procedural`) and `Drive.Nin_Ounce_NVN2.release.sarc` (another bfsha and agl's `sharcb`, `BAHS`): the same
containers as the Switch 1 game. It is a closed set: all of them can be translated ahead of time. eden-nx2 dumped
1 712 distinct MKW programs at run time (row 88).

Game version: `romfs/System/Resource/ResourceSizeTable.Product.100…` points to 1.0.0 (no `control.nacp`). The latest
is 1.8.0 (09/09/2026).
