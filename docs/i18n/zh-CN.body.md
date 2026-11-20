这是 [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) 的原生 PlayStation 5 自制移植版（`PPSA99193`）；mkw-recomp 是
Adrià 对《马力欧赛车 世界》（*Mario Kart World*，Nintendo Switch 2，1.8.0 和 1.0.0 版本）的静态重编译。你需要自己的游戏副本，
从你自己的 Switch 2 主机中提取：本仓库及其发布中不包含任何游戏内容。

**这是静态重编译，不是模拟器。** 游戏的 AArch64 代码会预先翻译成 C，并为 PS5 的 x86-64 Zen 2 CPU 编译（客体地址布局
`ps5-high144`）；没有 CPU 解释器，也没有 JIT。GPU 同样不是模拟的：Switch 2 的图形 API NVN2 在 API 层面基于 Vulkan 重新实现，
使用 Mesa 为 PS5 移植的 RADV 驱动；游戏的 SM86 着色器则在你的电脑上预先翻译成 SPIR-V。Nintendo SDK 的函数（`nn::*`）按名称
替换为我们自己的代码。

> **上游 PC 项目：** 想要 Linux 或 Windows 版本？请看
> [**adriadam10/mkw-recomp**](https://github.com/adriadam10/mkw-recomp)（[下方的 PC 说明](#@pc)）。

## 状态（PS5）

尚处早期，进展很快。截至 2026 年 11 月 18 日，在一台标准版 PS5 上：

- **启动、过场动画、菜单和比赛均可运行**，通过 RADV 直接以 Vulkan 输出画面，经 `sceAudioOut` 输出立体声，并支持最多四个已登录
  的 DualSense 手柄，带振动和 PlayStation 按键图标。
- **比赛默认以 60 fps 运行，不使用插帧**：在大奖赛基准测试中平均约 57-59 fps，偶尔较慢的帧会让比赛以实时速度的 0.95-0.98 倍
  进行（见[性能](#@performance)）。游戏的 30 fps 模式（全速）和插帧是 **Pit Stop > Display** 中的选项。
- **多人游戏：** 游戏的分屏（每位已登录玩家一个 DualSense），以及通过局域网的本地无线模式，可在多台 PS5 之间进行，或与同一版本的
  PC 版联机（[docs/ps5/multiplayer.md](docs/ps5/multiplayer.md)）。不支持 Nintendo 在线游戏。
- **游戏内 Pit Stop 覆盖菜单。** 随时按 **Options + 触控板按下** 打开 **Pit Stop**：一个为手柄设计的设置菜单，包含快捷行
  （**Master volume**、**Vibration strength**、**Performance HUD**）、所有设置分类以及实时 DualSense 按键重映射，保存到
  `/app0/mkw.ini`。
- **已知问题（[docs/project/roadmap.md](docs/project/roadmap.md)）：**
  - *长时间游玩：* 由于游戏会保留之前赛道的数据，可用内存会一场比赛接一场比赛地减少；内存不足时，纹理可能损坏或缺失。若出现这种
    情况，请在几场比赛后重启本作。
  - *稀疏图块：* 从启动开始就有少量稀疏纹理图块未加载：起跑线附近有轻微纹理错误，比赛开场画面为黑白。
  - *多人游戏：* 某赛道的第一场比赛可能会掉线一位着色器仍在编译中的玩家；之后的比赛正常。分屏在复杂场景中低于 30 fps。
  - *菜单：* 模式、角色和车辆选择界面与主机版仍略有不同。

## 你需要准备

1. **你自己的《马力欧赛车 世界》dump**，来自你自己的 Nintendo Switch 2：**1.8.0** 版（`exefs/main` 构建 ID
   `d00e4f399e411d06`）或 1.0.0 版（`9e244438c3d8ef1c`），可以是已解密的 `exefs/` 和 `romfs/` 文件夹，或 `.nup` 文件。
   在 `updates/` 中带有 1.8.0 更新的主机 dump 可直接使用；两者都有时会构建 1.8.0。
2. **一台已越狱的 PS5**，运行自制 payload 加载器（`etaHEN` 或 `elfldr`）、`kstuff`，以及 **ShadowMountPlus 1.7 Beta 5**
   或更新版本（它会挂载 `/data/homebrew/PPSA99193` 并在主屏幕上注册）。支持哪些固件取决于你的越狱、`kstuff` 和 RADV 自制环境
   （见 [docs/ps5/ps5.md](docs/ps5/ps5.md)）。
3. **一台约有 30 GB 可用空间的电脑**，与 PS5 处于同一网络，用于从你的 dump 构建本作：
   - **Windows 10 或 11（64 位）：** 无需安装其他任何东西，也不需要 WSL。Windows 构建器自带 Python、zig 和 glslc
     （[Windows 步骤](#@windows)）。
   - **Linux x86-64 或 WSL2：** 需要 `clang`、`lld` 和 Python 3（[Linux 步骤](#@linux)）。

## 在 Windows 上安装（无需 WSL）

1. **把占位程序放到 PS5 上。** 从[**发布页**](https://github.com/exploitizm/mkw-pit-stop/releases)解压
   `mkw-pit-stop-<version>-ps5-placeholder-PPSA99193.zip`，用 FTP 客户端（FileZilla、WinSCP）把其中的 `PPSA99193` 文件夹上传到
   `/data/homebrew/`，使 `/data/homebrew/PPSA99193/eboot.bin` 存在。ShadowMountPlus 注册后，启动
   **Mario Kart World: Pit Stop**：它会显示 **Game files missing** 和 `Console IP: <地址> (port 19913)`。请保持该画面打开；
   只有在显示该画面时 PS5 才会接收文件。
   <p align="center"><img src="docs/images/placeholder-ps5.png" width="560" alt="PS5 上的占位程序：缺少游戏文件、主机地址，以及电脑端构建的步骤和进度"></p>
2. **在电脑上解压构建器。** 下载 `mkw-pit-stop-<version>-ps5-builder-windows-x86_64.zip`，右键点击它，选择**属性**，勾选
   **解除锁定**，然后解压到一个有 30 GB 可用空间的磁盘上路径较短的文件夹，例如 `C:\PitStop`（不要用 `Program Files` 或
   OneDrive）。
3. **构建并发送。** 在该文件夹中运行 `mkw.exe`。在 **Game files** 中用 **Choose folder**（或 **Choose .nup or .nx file**）
   选择你的 dump；在 **Build for PS5** 下，把 PS5 画面上的地址填入 **PS5 IP**，然后按 **Build for PS5**。PS5 不需要桌面版的
   **Set up** 按钮。也可以在该文件夹的命令提示符中运行：
   ```bat
   prepare_ps5.bat --source "D:\Dumps\MKW" --send 192.168.1.50
   ```
   <p align="center"><img src="docs/images/launcher-ps5.png" width="560" alt="PS5 构建器的启动界面：已选择你的副本，已填入 PS5 地址，以及 Build for PS5 按钮"></p>
4. **启动游戏。** 电脑构建期间，PS5 画面会显示当前步骤和进度；所有文件都到达并校验完毕后会显示 **Transfer complete!**：按
   **叉键**，然后再次启动 **Mario Kart World: Pit Stop**。

**你的副本已经在 PS5 上了？** 勾选 **My game files are already on the PS5** 并在那里填写它的文件夹（例如 `/data/mkw`，其中或其
`updates\<version>` 中包含 `exefs` 和 `romfs`）：只会发送游戏代码和着色器，本作会在该文件夹中找到游戏。电脑构建时仍然需要你的
dump。

**没有网络？用 U 盘。** 在第 3 步中选择 **To a USB drive** 和 **Choose drive**，而不是填写 PS5 的地址：本作会写入该盘的
`homebrew\PPSA99193`（**exFAT**，约 25 GB 可用空间），把盘插到 PS5 上后 ShadowMountPlus 会在那里找到它（它会扫描
`/mnt/usb0`-`usb7/homebrew/`）。这种方式不需要第 1 步的占位程序；如果已经安装，请先删除 `/data/homebrew/PPSA99193`，确保只注册
一个副本。再次构建只会更新盘上发生变化的部分。

如果 PS5 一直停在 **Waiting for connection**，请检查地址以及两者是否在同一网络；构建失败的原因写在 `mkw.exe` 旁边的
`logs/setup.log` 中。更新时，解压新的构建器并重新构建即可。

## 在 PS5 上安装和更新（Linux 或 WSL2）

由于 `eboot.bin` 链接了重编译的游戏代码，而 `spirv/` 包含翻译后的游戏着色器，**不会分发可直接运行的 `eboot.bin` 或
`spirv/`**。你需要在自己的电脑上，用不含游戏内容的发布版构建器和你自己的 dump 构建 `PPSA99193` 文件夹：

1. 从[**发布页**](https://github.com/exploitizm/mkw-pit-stop/releases)下载
   `mkw-pit-stop-<version>-ps5-builder-linux-x86_64.tar.zst`，用 `SHA256SUMS` 核对其摘要，并在 Linux 或 WSL2 上解压：
   ```sh
   tar --zstd -xf mkw-pit-stop-*-ps5-builder-linux-x86_64.tar.zst
   cd mkw-pit-stop-*
   ```
2. 运行 `prepare_ps5.py`，指向你的《马力欧赛车 世界》dump（文件夹或 `.nup` 文件）：
   ```sh
   python3 prepare_ps5.py --source /path/to/your/mkw-dump --out ./PPSA99193
   ```
   脚本会识别 dump 的版本（1.8.0 或 1.0.0），把 AArch64 ExeFS 翻译成 C（`ps5-high144`），针对预编译的运行时和 PS5 RADV 归档
   编译并链接 `eboot.bin`，下载 NVIDIA 的 `nvdisasm` 和 `ptxas`（以 SHA-256 固定）以在本地生成 SM86 指令规格，把全部
   11,561 个着色器翻译到 `PPSA99193/spirv/`，并把 `exefs/` 和 `romfs/` 放入 `PPSA99193/`。
3. 通过 FTP 或 USB 把 `PPSA99193` 传到 PS5 的 `/data/homebrew/PPSA99193` —— 或者加上 `--deploy <PS5_IP>`，自动上传、逐个文件
   以 SHA-256 校验，并刷新 ShadowMountPlus 在 `/user/app/PPSA99193/sce_sys` 中注册的 `param.json` 和 `icon0.png`。
4. 从 PS5 主屏幕启动 **Mario Kart World: Pit Stop**。

**更新：** 解压更新的构建器，用相同的 `--work` 和 `--out` 目录运行 `prepare_ps5.py`。输入未变化的步骤（例如 `romfs/` 和未改动的
着色器）会被跳过，主机上的存档（`profile/`）和设置（`mkw.ini`）都会保留。

## Pit Stop 设置覆盖菜单

在任何画面按 DualSense 上的 **Options + 触控板按下**（桌面版按 **F1**）打开 **Pit Stop**
（[docs/ps5/pit-stop.md](docs/ps5/pit-stop.md)）：

| 操作 | 功能 |
|---|---|
| **Options + 触控板按下** | 在任何画面打开或关闭 Pit Stop |
| **方向键 / 左摇杆 上下** | 移动一行 |
| **方向键 / 左摇杆 左右** | 调整当前设置（关/开或按步进调整数值） |
| **圆圈键 / 叉键** | 确认 / 返回（跟随你的 Nintendo 或 Xbox 按键布局） |
| **L1 / R1** | 在 **All settings** 中切换到上一个 / 下一个分类 |

- 第一屏的**快捷行**可实时调整 **Master volume**（0–100%）、**Vibration strength**（0–300%）和 **Performance HUD**
  （Off、FPS、Full）。
- **All settings** 打开 **Display**、**Graphics**、**Controls**（可实时为每个动作捕获按键）、**Audio**、**General**、
  **Advanced**、**Hacks**（解锁所有车手、车辆、贴纸和特别杯）和 **Multiplayer**。更改会自动保存到 `/app0/mkw.ini`。

## 在电脑上游玩（Linux 和 Windows）

来自上游项目 [mkw-recomp](https://github.com/adriadam10/mkw-recomp)：

<p align="center">
  <img src="docs/images/launcher-files.png" width="400" alt="设置前启动界面的 Game files 页面">
  <img src="docs/images/launcher-main.png" width="400" alt="游戏设置完成后的启动界面">
</p>

1. 从你的 Switch 2 提取《马力欧赛车 世界》1.0.0 或 1.8.0（`exefs/` 和 `romfs/`，已解密）。
2. 从 [mkw-recomp 的发布页](https://github.com/adriadam10/mkw-recomp/releases)下载 PC 启动器：Linux 上用 `.AppImage`（构建的
   游戏保存在 `~/.local/share/mkw-recomp`；需要 glibc 2.31 或更新），Windows 上用 `.zip`。两者都需要约 30 GB 可用空间。
3. 运行 AppImage 或 `mkw.exe`，打开 **Game files**，指向你的 dump 并按 **Prepare**，然后按 **Play**。

## 从源代码构建

### PS5 原生目标（`PPSA99193`）

固定版本的 `PS5_VulkanTemplate`、`PS5_Vulkan`、`PS5_Mesa` 和 `PS5_PayloadSDK` 检出请见 [docs/ps5/ps5.md](docs/ps5/ps5.md) 和
[tools/ps5/README.md](tools/ps5/README.md)：

```sh
python3 recomp/gen.py --exefs "$MKW_GAME/exefs" --guest-layout ps5-high144 \
    --out build/ps5-game/generated --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" \
    --generated build/ps5-game/generated --exefs "$MKW_GAME/exefs"
python3 tools/ps5/deploy.py <PS5_IP> --game
```

从运行时清单（`--inventory`）打包不含游戏内容的 PS5 构建器发布版：

```sh
python3 tools/ps5/build_game.py --stack "$stack" --include "$headers" --inventory
python3 tools/ps5/release_ps5.py --stack "$stack" --version 0.1.0
```

### Linux 和 Windows 主机构建

详见 [tools/build/README.md](tools/build/README.md)：

```sh
python3 recomp/gen.py --exefs game/exefs --out build/generated --jobs 8 --patches recomp/patches-mkw.txt
python3 tools/shaders/extract.py && python3 tools/shaders/translate.py
python3 tools/build/build.py              # build/mkw; --target windows builds mkw.exe with llvm-mingw
build/mkw
```

| 文件夹 | 内容 |
|---|---|
| [recomp/](recomp/README.md) | 生成器：AArch64 → C |
| [runtime/](runtime/README.md) | 替代主机的部分：内核、内存、SDK 替代实现、基于 Vulkan 的 NVN2、音频、输入、Pit Stop 和启动器 |
| [sm86/](sm86/README.md) | 着色器翻译器：SM86 SASS → GLSL → SPIR-V |
| [tools/](tools/README.md) | 分析、设置、构建、PS5 打包、游戏运行和诊断 |
| [tests/](tests/README.md) | 不需要游戏或 GPU 的测试，在 Linux 和 Windows 的 CI 中运行 |
| [docs/](docs/README.md) | PS5 架构、Pit Stop、研究、来源、路线图和性能 |
| [i18n/](i18n/README.md) | 菜单文本的 gettext 文件（`mkw.pot`，每种语言一个 `.po`） |
| [server/](server/README.md) | 通过互联网进行本地无线游戏的大厅服务器（`server/lobby`） |
| `.github/` | CI、issue 和 pull request 模板 |

根目录：`pyproject.toml` 和 `uv.lock`（工具的 Python 环境）、`Makefile`（发布和 CI 流程的快捷方式）、`.gitignore`、
`LICENSE`、`CONTRIBUTING.md`、`CHANGELOG.md`（每个版本的更改及其说明）、`AGENTS.md`（给编程代理的指南）。

## 性能

在标准版 PS5 上，《马力欧赛车 世界》1.8.0，大奖赛比赛基准测试（`tools/ps5/bench.py`，比赛 210 秒，1080p，关闭插帧）：

| 日期 | 帧率设置 | 平均帧时间 | 典型帧 | 1% 低帧 | 比赛速度 |
|---|---|---|---|---|---|
| 2026-11-20 | 60 fps（新的默认值） | 17.0–17.4 ms（57–59 fps） | 16.7 ms（达到上限） | 35–41 fps | 0.95–0.98 倍 |
| 2026-11-16 | 30 fps（之前的默认值） | 33.3 ms | 33.3 ms | | 1.00 倍 |

新的默认值尚未在主机上以发布状态测试过：这些数据来自开启了运行时性能日志（`MKW_TRACE=perf`，会带来少量开销）、手动设为 60 fps
的基准比赛。菜单、计时赛、分屏、长时间游玩和其他赛道尚未在新的默认值下游玩过。

约 2% 的比赛帧超过 20 ms：它们出现在赛道加载新纹理的地方，决定了 1% 低帧和轻微的慢动作。已测量的内容和尚待处理的问题见
[docs/project/roadmap.md](docs/project/roadmap.md) 和 [docs/ps5/ps5.md](docs/ps5/ps5.md#frame-rate)；电脑上的数据见
[docs/project/performance.md](docs/project/performance.md)。

## 报告问题

请看 [CONTRIBUTING.md](CONTRIBUTING.md)。PS5 问题请在[本分支的 issues](https://github.com/exploitizm/mkw-pit-stop/issues) 中报告，
并附上主机内核日志（`klog`，前缀为 `[Mario Kart World: Pit Stop]`）以及电脑端构建器的 `logs/setup.log`。桌面版问题请附上
`logs/mkw.log` 和 `logs/setup.log`。切勿附上游戏文件、dump 或由你的 dump 构建的 `eboot.bin` 二进制文件。

## 许可证、致谢和免责声明

本仓库的源代码采用 [MIT 许可证](LICENSE)（`Copyright (c) 2026 Adrià`）。

**PS5 发布文件采用 GPL-3.0-or-later。** 每个 PS5 程序都链接了 payload SDK 分支的平台层和 `PS5_Vulkan` 的启动代码
（GPL-3.0-or-later），因此占位程序、构建器的 `ps5-bundle/` 以及你用它们构建的每个程序整体上都采用 GPL-3.0-or-later；
mkw-recomp 自己的代码仍为 MIT。每个发布都带有 `LEGAL.txt` 和 `licenses/`（每个部分及其许可证、版权和源码版本），并以
`source/*.tar.gz`（`source/SOURCES.txt`）附上每个部分的完整源代码，由 [tools/ps5/release_notices.py](tools/ps5/release_notices.py)
生成。

- **上游项目：** Adrià 的 [**mkw-recomp**](https://github.com/adriadam10/mkw-recomp) —— AArch64→C 提升器、Switch 2 内核和
  `nn::*` SDK 替代实现、基于 Vulkan 的 NVN2 渲染器，以及离线 SM86→SPIR-V 着色器翻译器。
- **MIT 及宽松许可的项目**（[recomp/](recomp/)、[runtime/](runtime/)、[sm86/decode/](sm86/decode/) 和
  [runtime/third_party/](runtime/third_party/README.md) 中的 `LICENSE.*` 文件）：
  [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) 及其 Linux 移植、
  [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)、
  [DocumentSASS](https://github.com/0xD0GF00D/DocumentSASS) 和
  [nvsass-disassembler](https://github.com/reoLantern/nvsass-disassembler)、
  [Ryujinx](https://git.ryujinx.app/ryubing/ryujinx)、[Mesa NAK](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/nouveau/compiler)、
  [mario-kart-world-save-editor](https://github.com/Ca1nM/mario-kart-world-save-editor)、Dear ImGui、
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)、AMD FidelityFX FSR 1、Vulkan Memory Allocator、Roboto 和 Lato。
- **PS5 工具链和运行时：** [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate)（MIT，Sascha Willems 和
  Mihawk）、[PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa)（MIT，PS5 版 RADV）、
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) 和 [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK)
  （GPL-3.0-or-later，Mihawk 和 [John Törnblom / ps5-payload-dev](https://github.com/ps5-payload-dev/sdk)），以及
  [ShadowMountPlus](https://github.com/drakmor/shadowMountPlus)。与 payload SDK 平台层和 `PS5_Vulkan` 的 CRT/存根一起链接进
  PS5 程序时，合并后的 PS5 程序二进制文件采用 GPL-3.0-or-later（见构建出的程序中的 `LEGAL.txt` 和 `licenses/`）。
- **仅参考研究和设计思路**（未从 GPL 或无许可证项目中复制代码）：eden-nx2、Hookshot、PS5_Hookshot、EglantinePS5、AnyPS5、
  prosper 和 shadPS4。每个来源都记录在 [docs/project/research.md](docs/project/research.md#sources) 中。

**免责声明：** *Mario Kart World* 和 *Mario* 是 Nintendo 的商标。"PlayStation" 和 "PS5" 是 Sony Interactive Entertainment Inc.
的商标。本项目与 Nintendo、Sony Interactive Entertainment 或任何游戏发行商无关，也未获得其认可；本项目不包含任何游戏代码、着色器
或资源，也不帮助获取它们。
