<p align="center"><img src="docs/images/icon.svg" width="144" alt="Mario Kart World: Pit Stop icon"></p>

# Mario Kart World: Pit Stop

A native PlayStation 5 homebrew port (`PPSA99193`) of [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp),
Adrià's static recompilation of *Mario Kart World* (Nintendo Switch 2, version 1.0.0). It needs your own copy of the
game, dumped from your own Switch 2 console: nothing of the game is in this repository or in its releases.

**It is a static recompilation, not an emulator.** The game's AArch64 code is translated to C ahead of time and
compiled for the PS5's x86-64 Zen 2 CPU (`ps5-high144` guest address layout); there is no CPU interpreter or JIT. The
GPU is not emulated either: NVN2, the Switch 2 graphics API, is reimplemented on top of Vulkan at the API level using
Mesa's RADV driver for the PS5, and the game's SM86 shaders are translated to SPIR-V ahead of time on your PC. Nintendo
SDK functions (`nn::*`) are replaced by name with our own code.

> **Upstream PC project:** Looking for the Linux or Windows version? See [**adriadam10/mkw-recomp**](https://github.com/adriadam10/mkw-recomp) ([PC instructions below](#playing-on-pc-linux-and-windows)).

## Status (PS5)

Early, and moving fast. On 16/11/2026 on a base PS5:

- **Boot, movies, menus and races work.** The title outputs 1080p60 over direct Vulkan display (`VK_KHR_display`), plays
  48 kHz stereo audio through `sceAudioOut`, and supports up to four signed-in DualSense controllers with rumble and
  PlayStation button prompts.
- **Races run at 30 fps at full speed (1.00x).** The PS5 device profile defaults to the game's 30 fps mode (two game
  ticks per frame, steady 33.3 ms frame times on busy courses; 60 fps remains an option in **Pit Stop > Display > Frame
  rate**). With hardware page-protection batching and `MKW_PW_HOT`, a race frame's work is ~26–28 ms.
- **Pit Stop in-game overlay.** Press **Options + touchpad click** at any time to open **Pit Stop**, a controller-first
  settings menu with quick controls (**Master volume**, **Vibration strength**, **Performance HUD**), full graphics and
  game settings, and live DualSense button remapping, saved to `/app0/mkw.ini`.
- **Known issues ([docs/roadmap.md](docs/roadmap.md)):**
  - *Memory in long sessions:* across three or more races in one session, free direct memory falls as the game's 2.4 GiB
    `CPU_CACHED|GPU_CACHED` pool keeps earlier courses' data; near 100 MiB free, ASTC texture decoding can run out of
    scratch space and leave textures corrupted or missing (`MKW_PACK=1` compresses cold guest memory with zstd and is in
    console testing).
  - *Sparse tile cap:* up to 128 MiB of sparse tiles log non-resident from boot (`MKW_SPARSE_MIB=1536`), causing minor
    texture corruption near the start line and a black-and-white race intro.
  - *Intermittent magenta blocks:* occasional magenta ASTC error blocks on character select cards or scenery; restarting
    the title or clearing the shader cache clears them.
  - *Menus and modes:* mode, character and vehicle select screens still have minor differences from the Switch 2; local
    split screen is untested; there is no online play.

## What you need ("You bring")

1. **Your own *Mario Kart World* 1.0.0 dump** from your own Nintendo Switch 2 (`exefs/` and `romfs/`, decrypted;
   `exefs/main` build ID `9e244438c3d8ef1c`).
2. **A jailbroken PS5** running a homebrew payload loader (`etaHEN` or `elfldr`), `kstuff`, and **ShadowMountPlus 1.7
   Beta 5** or newer (which mounts and registers `/data/homebrew/PPSA99193` on the home screen). Firmware support
   follows your jailbreak, `kstuff`, and RADV homebrew environment (see [docs/ps5.md](docs/ps5.md)).
3. **A PC (Linux x86-64 or WSL2 on Windows)** with `clang`, `lld`, Python 3, and about 30 GB of free disk space to
   prepare the PS5 title folder from your dump.

## Installing and updating on PS5

Because `eboot.bin` links the recompiled game code and `spirv/` holds the game's translated shaders, **no ready-to-run
`eboot.bin` or `spirv/` is distributed**. You build the `PPSA99193` title folder on your PC from the game-free release
builder and your own dump:

1. Download `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` from **Releases**, check its digest against
   `SHA256SUMS`, and unpack it on Linux or WSL2:
   ```sh
   tar --zstd -xf mkw-pit-stop-*-ps5-builder-linux-x86_64.tar.zst
   cd mkw-pit-stop-*
   ```
2. Run `prepare_ps5.py` pointing at your decrypted *Mario Kart World* 1.0.0 dump:
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   The script verifies your 1.0.0 dump, translates the AArch64 ExeFS to C (`ps5-high144`), compiles and links
   `eboot.bin` against the prebuilt runtime and PS5 RADV archive, downloads NVIDIA's `nvdisasm` and `ptxas` (pinned by
   SHA-256) to generate the SM86 instruction spec locally, translates all 11,561 shaders into `PPSA99193/spirv/`, and
   stages `exefs/` and `romfs/` into `PPSA99193/`.
3. Transfer `PPSA99193` to `/data/homebrew/PPSA99193` on your PS5 via FTP or USB — or pass `--deploy <PS5_IP>` to
   upload, verify every file by SHA-256, and refresh ShadowMountPlus's registered `param.json` and `icon0.png` in
   `/user/app/PPSA99193/sce_sys`.
4. Launch **Mario Kart World: Pit Stop** from the PS5 home screen.

**Updating:** unpack the newer builder and run `prepare_ps5.py` with the same `--work` and `--out` directories. Steps
whose inputs did not change (such as `romfs/` and unchanged shaders) are skipped, and your saves (`profile/`) and
settings (`mkw.ini`) on the console are kept.

## Pit Stop settings overlay

Press **Options + touchpad click** on the DualSense (or **F1** on desktop) from any screen to open **Pit Stop**
([docs/pit-stop.md](docs/pit-stop.md)):

| Control | Action |
|---|---|
| **Options + touchpad click** | Open or close Pit Stop from any screen |
| **D-pad / Left stick Up & Down** | Move one row |
| **D-pad / Left stick Left & Right** | Adjust the focused setting (Off/On or step value) |
| **Circle / Cross** | Confirm / Back (follows your Nintendo or Xbox button layout) |
| **L1 / R1** | Previous / Next category inside **All settings** |

- **Quick rows** on the first screen adjust **Master volume** (0–100%), **Vibration strength** (0–300%), and
  **Performance HUD** (Off, FPS, Full) live.
- **All settings** opens **Display**, **Graphics**, **Controls** (with live per-action button capture), **Audio**,
  **Game**, **Menu**, **Advanced**, **Hacks** (unlock all drivers, vehicles, stickers and Special Cup), and **Quit
  game**. Changes save automatically to `/app0/mkw.ini`.

## Playing on PC (Linux and Windows)

From upstream [mkw-recomp](https://github.com/adriadam10/mkw-recomp):

<p align="center">
  <img src="docs/images/launcher-setup.png" width="400" alt="The start screen setting the game up from the player's dump">
  <img src="docs/images/launcher-main.png" width="400" alt="The start screen once the game is set up">
</p>

1. Dump Mario Kart World 1.0.0 from your Switch 2 (`exefs/` and `romfs/`, decrypted).
2. Download the PC launcher from [mkw-recomp Releases](https://github.com/adriadam10/mkw-recomp/releases): on Linux the
   `.AppImage` (keeps the game it builds in `~/.local/share/mkw-recomp`; glibc 2.31 or newer), on Windows the `.zip`.
   Either needs about 30 GB free.
3. Run the AppImage or `mkw.exe`, open **Game files**, point it at your dump and press **Prepare**, then **Play**.

## Building from source

### PS5 native target (`PPSA99193`)

See [docs/ps5.md](docs/ps5.md) and [tools/ps5/README.md](tools/ps5/README.md) for the pinned `PS5_VulkanTemplate`,
`PS5_Vulkan`, `PS5_Mesa` and `PS5_PayloadSDK` checkouts:

```sh
python3 recomp/gen.py --exefs "$MKW_GAME/exefs" --guest-layout ps5-high144 \
    --out build/ps5-game/generated --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" \
    --generated build/ps5-game/generated --exefs "$MKW_GAME/exefs"
python3 tools/ps5/deploy.py <PS5_IP> --game
```

To pack a game-free PS5 builder release from the runtime inventory (`--inventory`):

```sh
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" --inventory
python3 tools/ps5/release_ps5.py --stack "$stack" --version 0.1.0
```

### Linux and Windows host builds

Details in [tools/build/README.md](tools/build/README.md):

```sh
python3 recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

| Folder | Contents |
|---|---|
| [recomp/](recomp/README.md) | The generator: AArch64 → C |
| [runtime/](runtime/README.md) | What plays the console: kernel, memory, SDK replacements, NVN2 over Vulkan, audio, input, Pit Stop and launcher |
| [sm86/](sm86/README.md) | The shader translator: SM86 SASS → GLSL → SPIR-V |
| [tools/](tools/README.md) | Analysis, setup, build, PS5 packaging, game runs and diagnostics |
| [tests/](tests/README.md) | Tests without the game or a GPU, in the CI on Linux and Windows |
| [docs/](docs/README.md) | PS5 architecture, Pit Stop, research, sources, roadmap and performance |
| [i18n/](i18n/README.md) | The menu's texts as gettext files (`mkw.pot`, one `.po` per language) |
| `.github/` | CI, issue and pull request templates |

At the root: `pyproject.toml` and `uv.lock` (the Python environment of the tools), `Makefile` (shortcuts for the
release and CI flows), `.gitignore`, `LICENSE`,
`CONTRIBUTING.md`, `CHANGELOG.md` (each release's changes, its notes), `AGENTS.md` (a guide for coding agents).

## Performance

Measured frame times, each with its date, are in [docs/performance.md](docs/performance.md) and
[docs/ps5.md](docs/ps5.md#frame-rate).

| Date | Platform / GPU | Internal resolution | Race frame time |
|---|---|---|---|
| 16/11/2026 | PlayStation 5 (base, RADV, 30 fps default) | 1080p | 33.3 ms (1.00x speed; ~26–28 ms busy) |
| 09/11/2026 | Radeon 660M (Linux, RADV) | 720p | 31–33 ms |
| 09/11/2026 | GTX 1050 Ti (Windows) | 1080p | 42–45 ms |

## Reporting bugs

See [CONTRIBUTING.md](CONTRIBUTING.md). For PS5 issues, attach the console kernel log (`klog`, prefixed with
`[Mario Kart World: Pit Stop]`) and `logs/setup.log` from the PC builder. For desktop issues, attach `logs/mkw.log` and
`logs/setup.log`. Never attach game files, dumps, or `eboot.bin` binaries built from your dump.

## Licenses, credits and disclaimer

This repository's source code is under the [MIT License](LICENSE) (`Copyright (c) 2026 Adrià`).

- **Upstream project:** [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) by Adrià — the AArch64→C lifter,
  Switch 2 kernel and `nn::*` SDK replacements, NVN2-over-Vulkan renderer, and offline SM86→SPIR-V shader translator.
- **MIT and permissive projects** (`LICENSE.*` files in [recomp/](recomp/), [runtime/](runtime/),
  [sm86/decode/](sm86/decode/) and [runtime/third_party/](runtime/third_party/README.md)):
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) and its Linux port,
  [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp),
  [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) and
  [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler),
  [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx), [Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler),
  [mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor), Dear ImGui,
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), AMD FidelityFX FSR 1, Vulkan Memory Allocator, Roboto and
  Lato.
- **PS5 toolchain and runtime:** [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate) (MIT, Sascha
  Willems and Mihawk), [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (MIT, RADV for PS5),
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) and [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)
  (GPL-3.0-or-later, Mihawk and [John Törnblom / ps5-payload-dev](https://github.com/ps5-payload-dev/sdk)), and
  [ShadowMountPlus](https://github.com/drakmor/shadowMountPlus). When linked into a PS5 title with the payload SDK
  platform layer and `PS5_Vulkan` CRT/stubs, the combined PS5 title binary is covered by GPL-3.0-or-later
  (`LEGAL.txt` and `licenses/` inside the built title).
- **Research and design ideas only** (no code copied from GPL or unlicensed projects): eden-nx2, Hookshot,
  PS5_Hookshot, EglantinePS5, AnyPS5, prosper, and shadPS4. Every source is documented in
  [docs/research.md](docs/research.md#sources).

**Disclaimer:** *Mario Kart World* and *Mario* are trademarks of Nintendo. "PlayStation" and "PS5" are trademarks of
Sony Interactive Entertainment Inc. This project is not affiliated with or endorsed by Nintendo, Sony Interactive
Entertainment, or any game publisher, contains no game code, shaders or assets, and does not help to obtain them.
