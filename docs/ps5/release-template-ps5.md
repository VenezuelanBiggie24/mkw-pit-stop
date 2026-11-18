{summary}

## Changes

{changes}

## Tested before publishing

{tested}

## Requirements

- **Your own game copy:** *Mario Kart World* version 1.8.0 or 1.0.0 (`exefs/` and `romfs/`, decrypted, or a `.nup` file), dumped from your own Nintendo Switch 2; a console dump with the 1.8.0 update in `updates/` is used as it is. Nothing of the game is in this release or repository.
- **A jailbroken PS5:** running a payload loader (`etaHEN` or `elfldr`), `kstuff`, and **ShadowMountPlus 1.7 Beta 5** or newer to register homebrew titles in `/data/homebrew/`.
- **A PC with about 30 GB free** to build the title from your dump, on the same network as the PS5:
  - **Windows 10 or 11 (64-bit):** nothing to install; the Windows builder carries its own Python, zig and glslc. No WSL needed.
  - **Linux x86-64 or WSL2:** Python 3, with `clang` and `lld` (18 or newer).

## Install on Windows (no WSL)

1. **On the PS5:** unzip `mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip` and upload the `PPSA99193` folder to `/data/homebrew/` with an FTP client (FileZilla, WinSCP), so that `/data/homebrew/PPSA99193/eboot.bin` exists. Once ShadowMountPlus registers it, launch **Mario Kart World: Pit Stop**: it shows **Game files missing** with `Console IP: <address> (port 19913)`. Leave that screen open; the PS5 only receives files while it is shown.
2. **On the PC:** download `mkw-pit-stop-<version>-ps5-builder-windows-x86_64.zip`. Right-click it, **Properties**, tick **Unblock**, OK; then extract it to a short folder on a drive with 30 GB free, such as `C:\PitStop` (not `Program Files` or OneDrive).
3. Run `mkw.exe` from that folder. On **Game files**, choose your dump with **Choose folder** (or **Choose .nup or .nx file**). Under **Build for PS5**, type the IP from the PS5 screen into **PS5 IP** and press **Build for PS5**. The steps tick off one by one; the first build takes a long time. You do not need the desktop **Set up** button for the PS5.
   Without the launcher, from a Command Prompt in that folder:
   ```bat
   prepare_ps5.bat --source "D:\Dumps\MKW" --send 192.168.1.50
   ```
4. While the PC builds, the PS5 screen shows its step and progress; then **Receiving from PC builder...** and **Transfer complete!**. Press **Cross**, then launch **Mario Kart World: Pit Stop** again. Press **Options + touchpad click** at any time in game to open the **Pit Stop** settings overlay.

**Copy already on the PS5:** tick **My game files are already on the PS5** and type its folder there (with `exefs` and `romfs` in it or in its `updates\<version>`): only the game code and shaders are sent.

**Without the network:** choose **To a USB drive** and **Choose drive** in step 3: the title goes to `homebrew\PPSA99193` on the drive (exFAT, about 25 GB free), where ShadowMountPlus finds it on the PS5. The placeholder is not needed then; remove `/data/homebrew/PPSA99193` if it is installed, so only one copy is registered.

If the PS5 stays on **Waiting for connection**, check the IP and that the PC and the PS5 are on the same network. A failed build is explained in `logs/setup.log` beside `mkw.exe`.

## Install on Linux or WSL2

1. Download `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst` from this release, verify it against `SHA256SUMS`, and unpack it (`tar --zstd -xf mkw-pit-stop-*.tar.zst`).
2. Run `prepare_ps5.py` pointing at your *Mario Kart World* dump (a folder or a `.nup` file):
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   The first run finds the dump's version by `exefs/main`'s build ID (1.8.0 `d00e4f399e411d06`, preferred, or 1.0.0 `9e244438c3d8ef1c`), generates and compiles the game's C code for the PS5 (`ps5-high144`), links and signs `eboot.bin` against the bundled runtime and RADV driver, downloads NVIDIA's `nvdisasm` and `ptxas` (pinned by SHA-256) to build the SM86 shader specification on your machine, translates the game's 11,561 shaders to `spirv/`, and copies `exefs/` and `romfs/` into `PPSA99193/`.
3. Copy `PPSA99193` to `/data/homebrew/PPSA99193` on your PS5 over FTP or USB, or pass `--deploy <PS5_IP>` to upload, verify by SHA-256, and refresh ShadowMountPlus's registered `sce_sys`.
4. Launch **Mario Kart World: Pit Stop** (`PPSA99193`) from the PS5 home screen. Press **Options + touchpad click** at any time in game to open the **Pit Stop** settings overlay.

## Updating

Unpack the new builder over your folder (or pass the same `--work` directory) and build again (**Build for PS5** on Windows, `prepare_ps5.py` on Linux): steps whose inputs have not changed (such as `romfs/` and unchanged shaders) are skipped, and only updated parts are rebuilt and uploaded. Saves and `mkw.ini` settings on the console are kept.

## Known issues

{known}

## Licenses and source

The PS5 files in this release are under **GPL-3.0-or-later** as a whole: the PS5 title links GPL-3.0-or-later code (the PS5 payload SDK fork's platform layer and PS5_Vulkan's start-up code). mkw-recomp's own code is MIT. Each package holds `LEGAL.txt` and `licenses/` with every part, its license, copyright and source revision. The complete source of every part is attached to this release as `source/*.tar.gz` (listed in `source/SOURCES.txt`, digests in `SHA256SUMS`).

## Reporting a problem

Open an issue at https://github.com/exploitizm/mkw-pit-stop/issues and attach the console kernel log (`klog`) and `logs/setup.log` from the PC builder folder.

*Mario Kart World is © Nintendo. Mario Kart World: Pit Stop is an unofficial fan project and is not affiliated with or endorsed by Nintendo, Sony Interactive Entertainment, or any game publisher.*
