{summary}

## Changes

{changes}

## Tested before publishing

{tested}

## Requirements

- **Your own game copy:** *Mario Kart World* version 1.0.0 (`exefs/` and `romfs/`, decrypted), dumped from your own Nintendo Switch 2. Nothing of the game is in this release or repository.
- **A jailbroken PS5:** running a payload loader (`etaHEN` or `elfldr`), `kstuff`, and **ShadowMountPlus 1.7 Beta 5** or newer to register homebrew titles in `/data/homebrew/`.
- **A PC (Linux x86-64 or WSL2 on Windows):** with `clang` and `lld` (18 or newer) and about 30 GB of free disk space to build the title folder from your dump.

## Install (PC preparation to PS5)

1. Download `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` from this release, verify it against `SHA256SUMS`, and unpack it (`tar --zstd -xf mkw-pit-stop-*.tar.zst`).
2. Run `prepare_ps5.py` pointing at your decrypted *Mario Kart World* 1.0.0 dump:
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   The first run checks that `exefs/main` is version 1.0.0 (`9e244438c3d8ef1c`), generates and compiles the game's C code for the PS5 (`ps5-high144`), links and signs `eboot.bin` against the bundled runtime and RADV driver, Downloads NVIDIA's `nvdisasm` and `ptxas` (pinned by SHA-256) to build the SM86 shader specification on your machine, translates the game's 11,561 shaders to `spirv/`, and copies `exefs/` and `romfs/` into `PPSA99193/`.
3. Copy `PPSA99193` to `/data/homebrew/PPSA99193` on your PS5 over FTP or USB, or pass `--deploy <PS5_IP>` to upload, verify by SHA-256, and refresh ShadowMountPlus's registered `sce_sys`.
4. Launch **Mario Kart World: Pit Stop** (`PPSA99193`) from the PS5 home screen. Press **Options + touchpad click** at any time in game to open the **Pit Stop** settings overlay.

## Updating

Unpack the new builder over your folder (or pass the same `--work` directory) and run `prepare_ps5.py` again: steps whose inputs have not changed (such as `romfs/` and unchanged shaders) are skipped, and only updated parts are rebuilt and uploaded. Saves and `mkw.ini` settings on the console are kept.

## Known issues

{known}

## Reporting a problem

Open an issue on GitHub and attach the console kernel log (`klog`) and `logs/setup.log` from the PC preparation folder.

*Mario Kart World is © Nintendo. Mario Kart World: Pit Stop is an unofficial fan project and is not affiliated with or endorsed by Nintendo, Sony Interactive Entertainment, or any game publisher.*
