#!/usr/bin/env python3
"""Prepare and deploy Mario Kart World: Pit Stop (PPSA99193) on a PC from a dump folder or .nup file.

Why: a PS5 release cannot ship game code (which links statically into eboot.bin), exefs/, romfs/, or translated
shaders (spirv/). This builder runs on Windows or Linux without a PS5 SDK or bash installed, validates Mario Kart
World 1.0.0 (folder or .nup), unpacks .nup if needed, lifts AArch64 code to C, compiles with zig cc (or clang),
links and signs eboot.bin with ps5-native-tool[.exe], translates shaders (reusing desktop caches if present), stages
PPSA99193/, and streams changed files to the Pit Stop placeholder receiver over TCP (PIT1) or FTP.

Steps (tracked in <work>/setup-state.json so interrupted or updated runs rebuild and send only what changed):
  check     verify Mario Kart World 1.0.0 (main build id 9e244438c3d8ef1c) and builder bundle
  nup       unpack .nup into <work>/nup once (skipped for folder dumps or cached .nup)
  exefs     stage exefs/ into <out>/exefs/
  romfs     stage romfs/ into <out>/romfs/ (or skip with --romfs skip)
  generate  recomp/gen.py --guest-layout ps5-high144 -> <work>/generated
  compile   compile lifted C chunks + modules.c for znver2 -> <work>/lift-obj
  link      relocatable link + RADV recipe + ps5-native-tool link & sign -> <out>/eboot.bin, mkw.sym
  shaders   fetch/build SM86 spec locally and translate shaders -> <out>/spirv/
  deploy    send <out>/ to the console over PIT1 TCP (or --deploy-ftp over FTP)

Usage: python3 tools/ps5/prepare_ps5.py --source DUMP_OR_NUP [--out DIR] [--work DIR] [--bundle DIR]
                                        [--romfs copy|skip] [--send PS5_IP] [--jobs N] [--check-only]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1] if (HERE.parents[1] / "recomp/gen.py").is_file() else HERE
PKG_DIR = ROOT.parents[1] if ROOT.name == "src" and ROOT.parent.name == "setup" else ROOT
for _p in (ROOT / "tools/build", ROOT / "tools/prepare", ROOT / "tools/ps5", ROOT / "recomp"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import build  # noqa: E402
from layout import output_lock, read_checked  # noqa: E402
from machine import Failed, default_cc, default_glslc, mem_gb, own_children, shader_jobs, tool_env  # noqa: E402
from nup_reader import NupError, NupReader, is_nup  # noqa: E402
from ps5_prepare_common import ROMFS_FILES, check, copy_tree, emit, run, sm86_spec  # noqa: E402
from send_ps5 import TransferError, send_directory  # noqa: E402

TITLE_ID = "PPSA99193"
PROFILE = "ps5-high144"
PREP_PS5_VERSION = 2
STEPS = ("check", "nup", "exefs", "romfs", "generate", "compile", "link", "shaders", "deploy")

GEN_FLAGS = [
    "-fPIC", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections",
    "-Wno-return-type", "-Wno-unused-label", "-fvisibility=hidden",
]
TAB_FLAGS = [
    "-fPIC", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections", "-O2", "-DMKW_PS5_NATIVE",
]

WRAP_SYMS = (
    "pthread_create", "pthread_join", "pthread_detach", "fclose", "fflush",
    "malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc", "memalign",
    "malloc_usable_size", "reallocf", "reallocarray", "getline", "getdelim",
)

DEFSYM_CORE = (
    "qsort_r", "mkstemps", "openlog", "popen", "pclose", "open_memstream", "__xuname", "__assert",
    "__memset_chk", "regcomp", "regexec", "regfree", "regerror", "localtime_r", "newlocale", "freelocale",
    "strtod_l", "strtof_l", "dladdr", "utimensat", "localeconv_l", "strtoll_l", "strtoull_l", "strtold_l",
    "snprintf_l", "sscanf_l", "asprintf_l", "strcoll_l", "strxfrm_l", "strftime_l", "wcscoll_l", "wcsxfrm_l",
    "btowc_l", "wctob_l", "iswctype_l", "mbrlen_l", "mbrtowc_l", "mbsrtowcs_l", "mbsnrtowcs_l", "wcrtomb_l",
    "wcsnrtombs_l", "mbtowc_l", "___mb_cur_max_l", "___runetype_l", "___tolower_l", "___toupper_l",
    "__runes_for_locale", "catopen", "catgets", "catclose", "backtrace", "backtrace_symbols_fd",
    "__cxa_thread_atexit_impl",
    "arc4random", "arc4random_buf", "arc4random_uniform", "gmtime_r", "statvfs", "fstatvfs",
    "futimens", "clock_nanosleep", "getaddrinfo", "freeaddrinfo", "if_nameindex", "if_freenameindex",
    "opendir", "fdopendir", "readdir", "rewinddir", "dirfd", "closedir", "nl_langinfo", "nl_langinfo_l",
    "getpwuid_r", "posix_fallocate", "access",
    "openat", "unlinkat", "fchmodat", "fstatat", "mkdirat", "renameat", "memfd_create",
)

DEFSYM_OPT = ("localeconv", "strcasestr", "mkstemp", "readlink", "link", "symlink")
IGNORED_NULL_LIBS = {"libkernel_sys.so", "libScePosixForWebKit.so"}


# ---- minimal pure-Python ELF64 & ar readers (removes runtime dependency on llvm-nm, grep, awk, comm, bash)

def _iter_elf_symbols(data: bytes, section_type: int):
    """Yield (sym_type, sym_bind, shndx, value, name, sh_flags) from SHT_SYMTAB (2) or SHT_DYNSYM (11)."""
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 58)
    if not e_shoff or e_shnum == 0:
        return
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, _, sh_offset, sh_size, sh_link, _, _, sh_entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, off
        )
        sections.append((sh_type, sh_flags, sh_offset, sh_size, sh_link, sh_entsize))
    for sh_type, _, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type != section_type or sh_link >= len(sections):
            continue
        strtab_off, strtab_sz = sections[sh_link][2], sections[sh_link][3]
        strtab = data[strtab_off : strtab_off + strtab_sz]
        entsz = sh_entsize or 24
        for off in range(sh_offset, sh_offset + sh_size, entsz):
            st_name, st_info, _, st_shndx, st_value, _ = struct.unpack_from("<IBBHQQ", data, off)
            if st_name >= len(strtab):
                continue
            end = strtab.find(b"\x00", st_name)
            name = strtab[st_name : end if end >= 0 else None].decode("utf-8", errors="ignore")
            sh_flags = sections[st_shndx][1] if 0 < st_shndx < len(sections) else 0
            yield (st_info & 0xF, st_info >> 4, st_shndx, st_value, name, sh_flags)


def read_ar_defined(path: Path) -> set[str]:
    """Return defined global/weak symbols in a Unix `ar` static library."""
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        return set()
    defined: set[str] = set()
    pos = 8
    while pos + 60 <= len(data):
        hdr = data[pos : pos + 60]
        if hdr[58:60] != b"`\n":
            break
        size = int(hdr[48:58].decode("ascii").strip() or "0")
        body = data[pos + 60 : pos + 60 + size]
        if body.startswith(b"\x7fELF"):
            for _, bind, shndx, _, name, _ in _iter_elf_symbols(body, 2):
                if shndx != 0 and shndx < 0xFF00 and bind in (1, 2) and name:
                    defined.add(name)
        pos += 60 + size + (size & 1)
    return defined


def read_elf_dynsyms(path: Path) -> tuple[set[str], set[str]]:
    """Return (defined, undefined) symbol names from SHT_DYNSYM, stripping @version suffixes."""
    data = path.read_bytes()
    defined: set[str] = set()
    undefined: set[str] = set()
    for _, _, shndx, _, name, _ in _iter_elf_symbols(data, 11):
        base = name.split("@", 1)[0]
        if not base:
            continue
        if shndx == 0:
            undefined.add(base)
        else:
            defined.add(base)
    return defined, undefined


def write_mkw_sym(elf: Path, out: Path) -> int:
    """Write '<hex_addr> <name>' lines for host functions in elf (excluding lifted f_* guest functions)."""
    data = elf.read_bytes()
    entries: list[tuple[int, str]] = []
    seen: set[tuple[int, str]] = set()
    for sym_type, bind, shndx, value, name, sh_flags in _iter_elf_symbols(data, 2):
        if not name or name.startswith("f_") or shndx == 0 or shndx >= 0xFF00:
            continue
        if (sh_flags & 0x4) == 0 and sym_type != 2:  # SHF_EXECINSTR or STT_FUNC
            continue
        if bind not in (0, 1, 2):
            continue
        key = (value, name)
        if key not in seen:
            seen.add(key)
            entries.append(key)
    entries.sort()
    out.write_text("".join(f"{addr:x} {name}\n" for addr, name in entries), encoding="utf-8")
    return len(entries)


# ---- bundle & toolchain discovery

def find_bundle(bundle_arg: Path | None) -> Path:
    """Locate ps5-bundle (self-contained release) or build/ps5-game (developer checkout)."""
    for candidate in (bundle_arg, PKG_DIR / "setup/ps5-bundle", ROOT / "ps5-bundle",
                      ROOT / "build/ps5-bundle", ROOT / "build/ps5-game"):
        if candidate and candidate.is_dir():
            return candidate.resolve()
    raise Failed("PS5 bundle not found; run from an unpacked PS5 builder release or pass --bundle DIR")


def find_zig(bundle: Path) -> Path | None:
    exe = "zig.exe" if os.name == "nt" else "zig"
    for cand in (
        bundle / "bin" / exe,
        PKG_DIR / "setup/zig" / exe,
        ROOT / "setup/zig" / exe,
        bundle.parent / "zig" / exe,
        Path(shutil.which("zig") or ""),
    ):
        if cand and str(cand) and cand.is_file():
            lib = cand.parent / "lib"
            if (lib / "std/std.zig").is_file():
                os.environ["ZIG_LIB_DIR"] = str(lib)
                return cand.resolve()
    return None


def resolve_bundle_layout(bundle: Path) -> dict:
    """Normalise either a self-contained ps5-bundle/ or a developer build/ps5-game/ into one layout dict."""
    zig = find_zig(bundle)
    tool_exe = "ps5-native-tool.exe" if os.name == "nt" else "ps5-native-tool"

    if (bundle / "pitstop-runtime.json").is_file() or (bundle / "lib/app_crt.o").is_file():
        meta_path = bundle / "pitstop-runtime.json"
        meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.is_file() else {}
        native_tool = bundle / "bin" / tool_exe
        if not native_tool.is_file():
            native_tool = bundle / "bin/ps5-native-tool"
        ld_cmd = [str(zig), "ld.lld"] if zig else [str(bundle / "bin/ld.lld")]
        cc_cmd = (
            [str(zig), "cc", "-target", "x86_64-freebsd"]
            if zig
            else [str(bundle / "bin/prospero-clang")]
        )
        return {
            "kind": "bundle",
            "meta": meta,
            "rt_game_id": meta.get("rt_game_id") or build.game_id(PROFILE),
            "port_version": meta.get("port_version") or build.port_version(),
            "rt_obj": bundle / "loader-inventory.o",
            "app_crt": bundle / "lib/app_crt.o",
            "agc_stubs": [bundle / "lib/libSceAgc.so", bundle / "lib/libSceAgcDriver.so"],
            "radv": bundle / "lib/libvulkan_radeon.ps5.a",
            "dep_libs": [p for p in (bundle / "lib/libopus.a", bundle / "lib/libvpx.a", bundle / "lib/libzstd.a")
                         if p.is_file()],
            "cxx_libs": [
                bundle / "lib/libc++.a",
                bundle / "lib/libc++abi.a",
                bundle / "lib/libunwind.a",
                bundle / "lib/libclang_rt.builtins-x86_64.a",
                bundle / "lib/libps5platform.a",
            ],
            "sdk_include": bundle / "sdk_include",
            "sdk_lib": bundle / "sdk_lib",
            "ld_dir": bundle / "ld",
            "sce_sys": bundle / "sce_sys",
            "sce_module": bundle / "sce_module",
            "notices": [p for p in (bundle / "LEGAL.txt", bundle / "licenses") if p.exists()],
            "native_tool": native_tool,
            "cc_cmd": cc_cmd,
            "ld_cmd": ld_cmd,
            "is_zig": zig is not None,
        }

    # Developer checkout layout (build/ps5-game)
    title = bundle / "title"
    sdk = title / ".deps/native/ps5-payload-sdk"
    vulkan = bundle / "PS5_Vulkan"
    stack = bundle.parent / "ps5-stack"
    deps_dir = stack / "deps"
    dep_libs = [p for p in (
        deps_dir / "opus/lib/libopus.a",
        deps_dir / "libvpx/lib/libvpx.a",
        deps_dir / "zstd-1.5.7/lib/libzstd.a",
    ) if p.is_file()]
    if zig:
        cc_cmd = [str(zig), "cc", "-target", "x86_64-freebsd"]
        ld_cmd = [str(zig), "ld.lld"]
    else:
        cc_cmd = [str(sdk / "bin/prospero-clang")]
        ld_cmd = [str(sdk / "bin/ld.lld")]
    native_tool = vulkan / "build/host" / tool_exe
    if not native_tool.is_file():
        native_tool = vulkan / "build/host/ps5-native-tool"
    builtins = _find_dev_builtins(sdk)
    return {
        "kind": "dev",
        "meta": {},
        "rt_game_id": build.game_id(PROFILE),
        "port_version": build.port_version(),
        "rt_obj": bundle / "inventory-objects/loader-inventory.o",
        "vulkan": vulkan,
        "title": title,
        "sdk": sdk,
        "app_crt": None,  # built on demand in work/link if dev checkout
        "agc_stubs": [],
        "radv": vulkan / ".deps/native/radv-release/lib/libvulkan_radeon.ps5.a",
        "dep_libs": dep_libs,
        "cxx_libs": [
            sdk / "target/lib/libc++.a",
            sdk / "target/lib/libc++abi.a",
            sdk / "target/lib/libunwind.a",
            *([] if not builtins else [builtins]),
            sdk / "target/lib/libps5platform.a",
        ],
        "sdk_include": sdk / "target/include",
        "sdk_lib": sdk / "target/lib",
        "ld_dir": vulkan / "tooling/native",
        "ld_script": vulkan / "tooling/psbc/ps5-pie-unwind.ld",
        "sce_sys": title / "ps5/sce_sys",
        "sce_module": vulkan / "runtime",
        "notices": [],
        "native_tool": native_tool,
        "cc_cmd": cc_cmd,
        "ld_cmd": ld_cmd,
        "is_zig": zig is not None,
    }


def _find_dev_builtins(sdk: Path) -> Path | None:
    clang = os.environ.get("PS5_CLANG") or shutil.which("clang-22") or shutil.which("clang")
    if not clang:
        return None
    r = subprocess.run([clang, "--print-resource-dir"], capture_output=True, text=True, check=False)
    if r.returncode != 0:
        return None
    p = Path(r.stdout.strip()) / "lib/linux/libclang_rt.builtins-x86_64.a"
    return p if p.is_file() else None


def _ensure_dev_crt_and_stubs(layout: dict, link_dir: Path) -> tuple[Path, list[Path]]:
    """Build app_crt.o + AGC stubs once when running against a dev build/ps5-game tree."""
    if layout["app_crt"] and layout["app_crt"].is_file() and all(s.is_file() for s in layout["agc_stubs"]):
        return layout["app_crt"], layout["agc_stubs"]
    vulkan: Path = layout["vulkan"]
    sdk: Path = layout["sdk"]
    obj_dir = link_dir / "obj"
    stub_dir = link_dir / "stubs"
    obj_dir.mkdir(parents=True, exist_ok=True)
    stub_dir.mkdir(parents=True, exist_ok=True)
    clang = shutil.which("clang-22") or shutil.which("clang") or str(sdk / "bin/prospero-clang")
    base_flags = [
        "-target", "x86_64-sie-ps5", "-fvisibility-nodllstorageclass=default",
        "-isysroot", str(sdk), "-isystem", str(sdk / "target/include/c++/v1"),
        "-isystem", str(sdk / "target/include"), "-fno-stack-protector", "-fno-plt", "-femulated-tls",
        "-fdenormal-fp-math=ieee",
    ]
    app_crt = obj_dir / "app_crt.o"
    if not app_crt.is_file():
        subprocess.run([clang, *base_flags, "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti",
                        "-c", str(vulkan / "tooling/native/app_crt.cpp"), "-o", str(app_crt)], check=True)
    stubs = []
    for lib, src in (
        ("libSceAgc", "vendor/ps5/sdk/stubs/agc_canary_link_stub.c"),
        ("libSceAgcDriver", "vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c"),
    ):
        so = stub_dir / f"{lib}.so"
        if not so.is_file():
            so_obj = obj_dir / f"{lib}_stub.o"
            subprocess.run([clang, *base_flags, "-std=c11", "-O2", "-fPIC",
                            "-c", str(vulkan / src), "-o", str(so_obj)], check=True)
            subprocess.run([*layout["ld_cmd"], "--shared", "-soname", f"{lib}.prx",
                            "-o", str(so), str(so_obj)], check=True)
        stubs.append(so)
    return app_crt, stubs


# ---- compile & link steps

def compile_ps5_game(layout: dict, exefs: Path, work: Path, jobs: int) -> list[Path]:
    """Compile the ps5-high144 generated C chunks and modules.c into object files."""
    gen, obj = work / "generated", work / "lift-obj"
    obj.mkdir(parents=True, exist_ok=True)
    modules_c = obj / "modules.c"
    new_modules = build.module_glue(gen, exefs, False, PROFILE)
    if not modules_c.is_file() or modules_c.read_text(encoding="utf-8") != new_modules:
        modules_c.write_text(new_modules, encoding="utf-8")

    rt_game_id = layout["rt_game_id"]
    gid = f'-DRT_GAME_ID="{rt_game_id}"'
    cc = layout["cc_cmd"]
    is_zig = layout["is_zig"]
    cpu_flag = "-mcpu=znver2" if is_zig else "-march=znver2"

    gen_flags = [cpu_flag, *GEN_FLAGS]
    if is_zig:
        gen_flags += ["-Xclang", "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-fomit-frame-pointer",
                      "-g0", "-fno-sanitize=all"]
    else:
        gen_flags += ["-O1"]

    tab_flags = [cpu_flag, *TAB_FLAGS, gid]
    if layout["sdk_include"] and layout["sdk_include"].is_dir():
        gen_flags.extend(["-isystem", str(layout["sdk_include"])])
        tab_flags.extend(["-isystem", str(layout["sdk_include"])])

    inc = [f"-I{d}" for d in sorted({p.parent for p in (ROOT / "runtime").rglob("*.h")})] + [f"-I{gen}"]
    tasks: list[tuple[Path, Path, list[str], str]] = []
    for name, _, _ in read_checked(gen, PROFILE):
        for src in sorted((gen / name).glob("*.c")):
            tasks.append((src, obj / f"{name}_{src.stem}.o", [*gen_flags, f"-I{ROOT / 'runtime/cpu'}"], "lift"))
    # Recompile modules.o whenever rt_game_id changes (write marker)
    gid_stamp = obj / ".rt_game_id"
    force_modules = not gid_stamp.is_file() or gid_stamp.read_text(encoding="utf-8").strip() != rt_game_id
    tasks.append((modules_c, obj / "modules.o", [*tab_flags, *inc], "tables"))
    total, done = len(tasks), 0

    def one(item: tuple[Path, Path, list[str], str]) -> str:
        src, out, flags, kind = item
        if kind != "tables" or not force_modules:
            if out.is_file() and out.stat().st_mtime >= src.stat().st_mtime:
                return ""
        r = subprocess.run([*cc, "-std=gnu11", *flags, "-c", str(src), "-o", str(out)],
                           capture_output=True, text=True, check=False)
        return "" if not r.returncode else f"{src.name}: {r.stderr.strip()[-300:]}"

    with ThreadPoolExecutor(jobs) as pool:
        for err in pool.map(one, tasks):
            if err:
                raise Failed(f"compiling PS5 game code failed: {err}")
            done += 1
            if done % 10 == 0 or done == total:
                emit("progress", done, total)
    gid_stamp.write_text(rt_game_id + "\n", encoding="utf-8")
    return [out for _, out, _, _ in tasks]


def link_ps5_eboot(layout: dict, rt_obj: Path, extra_objs: list[Path], work: Path, out_dir: Path) -> None:
    """Link rt_obj + extra_objs into <out_dir>/eboot.bin, mkw.sym, and stage sce_sys/sce_module/notices."""
    link_dir = work / "link"
    link_dir.mkdir(parents=True, exist_ok=True)
    ld = layout["ld_cmd"]

    if layout["kind"] == "dev":
        app_crt, agc_stubs = _ensure_dev_crt_and_stubs(layout, link_dir)
    else:
        app_crt, agc_stubs = layout["app_crt"], layout["agc_stubs"]

    radv: Path = layout["radv"]
    sdk_lib: Path = layout["sdk_lib"]
    ld_dir: Path = layout["ld_dir"]
    ld_script: Path = layout.get("ld_script") or (ld_dir / "ps5-pie-unwind.ld")
    app_symbols_map: Path = ld_dir / "app-symbols.map"
    platform_a = next(p for p in layout["cxx_libs"] if p.name == "libps5platform.a")
    native_tool: Path = layout["native_tool"]

    for req in (rt_obj, app_crt, radv, platform_a, ld_script, app_symbols_map, native_tool, *agc_stubs):
        if not req.is_file():
            raise Failed(f"missing PS5 link input: {req}")

    combined = link_dir / "combined.o"
    rsp = link_dir / "objects.rsp"
    all_objs = [rt_obj, *extra_objs]
    rsp.write_text("\n".join(f'"{str(o).replace(chr(92), "/")}"' for o in all_objs) + "\n", encoding="utf-8")
    r = subprocess.run([*ld, "-r", "--gc-sections", "-u", "main", "-u", "catchReturnFromMain",
                        f"@{rsp}", "-o", str(combined)], capture_output=True, text=True, check=False)
    if r.returncode:
        raise Failed(f"PS5 relocatable link failed: {r.stderr.strip()[-400:]}")

    platform_syms = read_ar_defined(platform_a)
    has_localeconv = "ps5_localeconv" in platform_syms

    defsyms: list[str] = list(DEFSYM_CORE)
    for opt in DEFSYM_OPT:
        if f"ps5_{opt}" in platform_syms:
            defsyms.append(opt)

    local_map = link_dir / "radv-platform-local.map"
    local_map.write_text(
        "{\n    local:\n" + "".join(f"        {name};\n" for name in defsyms) + "};\n",
        encoding="utf-8",
    )

    flags: list[str] = [
        "-m", "elf_x86_64",
        "-pie",
        "-z", "max-page-size=0x4000",
        "--hash-style=gnu",
        "-T", str(ld_script),
        "-L", str(ld_dir),
        "--eh-frame-hdr",
        *(f"--wrap={name}" for name in WRAP_SYMS),
        *(f"--defsym={name}=ps5_{name}" for name in defsyms),
        "--version-script", str(local_map),
        "--version-script", str(app_symbols_map),
        "--exclude-libs=ALL",
        "-e", "_start",
    ]
    sdk_sos = sorted(sdk_lib.glob("*.so"))
    if not sdk_sos:
        raise Failed(f"no SDK stub .so files found in {sdk_lib}")

    inputs: list[str] = [
        str(app_crt),
        str(combined),
        *(str(p) for p in layout["dep_libs"]),
        *(str(p) for p in agc_stubs),
        "-L", str(sdk_lib),
        "--whole-archive", str(radv), "--no-whole-archive",
        "--start-group", *(str(p) for p in layout["cxx_libs"] if p.is_file()), "--end-group",
        "--as-needed", *(str(p) for p in sdk_sos),
    ]

    pie_elf = link_dir / "llvm-pie.elf"
    final_rsp = link_dir / "final-link.rsp"
    final_rsp.write_text("\n".join(f'"{x.replace(chr(92), "/")}"' for x in [*flags, *inputs]) + "\n", encoding="utf-8")
    r = subprocess.run([*ld, f"@{final_rsp}", "-o", str(pie_elf)], capture_output=True, text=True, check=False)
    if r.returncode:
        raise Failed(f"PS5 final ELF link failed: {r.stderr.strip()[-500:]}")

    elf_def, elf_undef = read_elf_dynsyms(pie_elf)
    if has_localeconv and "localeconv" in elf_undef:
        raise Failed("localeconv is not bound to libps5platform's ps5_localeconv")
    exported: set[str] = set()
    for lib_so in [*sdk_sos, *agc_stubs]:
        if lib_so.name in IGNORED_NULL_LIBS:
            continue
        so_def, _ = read_elf_dynsyms(lib_so)
        exported.update(so_def)
    null_imports = sorted(elf_undef - exported - elf_def)
    if null_imports:
        raise Failed(f"unresolved null runtime imports in ELF: {' '.join(null_imports[:20])}")

    out_dir.mkdir(parents=True, exist_ok=True)
    raw_elf = link_dir / "eboot.elf"
    link_cmd = [str(native_tool), "link", "--in", str(pie_elf), "--out", str(raw_elf), "--stub-dir", str(sdk_lib)]
    for stub in agc_stubs:
        link_cmd.extend(["--stub", str(stub)])
    link_cmd.extend(["--module-sdk", "0x02000009", "--companion-sdk", "0x08050001", "--file-name", "eboot.elf"])
    r = subprocess.run(link_cmd, capture_output=True, text=True, check=False)
    if r.returncode:
        raise Failed(f"ps5-native-tool link failed: {(r.stdout + r.stderr).strip()[-400:]}")

    eboot_dst = out_dir / "eboot.bin"
    eboot_tmp = out_dir / "eboot.bin.tmp"
    r = subprocess.run([str(native_tool), "self", "--sign", "--in", str(raw_elf),
                        "--out", str(eboot_tmp), "--magic", "0x1D3D154F"],
                       capture_output=True, text=True, check=False)
    if r.returncode:
        raise Failed(f"ps5-native-tool sign failed: {(r.stdout + r.stderr).strip()[-400:]}")
    r = subprocess.run([str(native_tool), "self", "--inspect", "--file", str(eboot_tmp)],
                       capture_output=True, text=True, check=False)
    if r.returncode:
        raise Failed(f"signed eboot.bin failed inspect: {(r.stdout + r.stderr).strip()[-300:]}")
    eboot_tmp.replace(eboot_dst)

    n_syms = write_mkw_sym(pie_elf, out_dir / "mkw.sym")
    print(f"mkw.sym: {n_syms} host symbols", file=sys.stderr)

    # Copy sce_sys, sce_module/libc.prx, notices, and runtime metadata
    sce_sys_dst = out_dir / "sce_sys"
    sce_sys_dst.mkdir(exist_ok=True)
    for asset in ("param.json", "icon0.png", "pic0.dds", "pic1.dds", "snd0.at9"):
        src_a = layout["sce_sys"] / asset
        if src_a.is_file():
            shutil.copy2(src_a, sce_sys_dst / asset)

    libc_prx = layout["sce_module"] / "libc.prx"
    libc_sha = layout["sce_module"] / "libc.prx.sha256"
    if not libc_prx.is_file():
        raise Failed(f"missing {libc_prx}")
    if libc_sha.is_file():
        want_sha = libc_sha.read_text(encoding="utf-8").strip().split()[0].lower()
        got_sha = hashlib.sha256(libc_prx.read_bytes()).hexdigest().lower()
        if got_sha != want_sha:
            raise Failed(f"libc.prx SHA-256 mismatch ({got_sha} != {want_sha})")
    sce_mod_dst = out_dir / "sce_module"
    sce_mod_dst.mkdir(exist_ok=True)
    shutil.copy2(libc_prx, sce_mod_dst / "libc.prx")

    if layout["notices"]:
        for notice in layout["notices"]:
            dst_n = out_dir / notice.name
            if notice.is_dir():
                shutil.copytree(notice, dst_n, dirs_exist_ok=True)
            else:
                shutil.copy2(notice, dst_n)
    elif layout["kind"] == "dev":
        stage_notices = layout["title"] / "ps5/tools/stage-notices.py"
        if stage_notices.is_file():
            subprocess.run([sys.executable, str(stage_notices), str(out_dir),
                            str(layout["vulkan"]), str(radv)], check=True)

    (out_dir / "pitstop-game.id").write_text(layout["rt_game_id"] + "\n", encoding="utf-8")
    (out_dir / "pitstop-runtime.json").write_text(
        json.dumps({"title_id": TITLE_ID, "port_version": layout["port_version"],
                    "rt_game_id": layout["rt_game_id"], "profile": PROFILE}, indent=2) + "\n",
        encoding="utf-8",
    )
    seq_nodes = ROOT / "build/seq_nodes.tsv"
    if seq_nodes.is_file():
        shutil.copy2(seq_nodes, out_dir / "seq_nodes.tsv")


# ---- shader reuse + setup state

def find_existing_spec(shader_cache: Path | None, bundle: Path) -> Path | None:
    candidates = [ROOT / "build/sm86/instructions.json", ROOT / "sm86/instructions.json"]
    if shader_cache:
        candidates.append(shader_cache.parent / "sm86/instructions.json")
    candidates.append(bundle.parent / "sm86/instructions.json")
    for p in candidates:
        if p.is_file():
            return p
    return None


def seed_shader_cache(shader_cache: Path | None, out_spirv: Path) -> int:
    if not shader_cache or not shader_cache.is_dir():
        return 0
    out_spirv.mkdir(parents=True, exist_ok=True)
    copied = 0
    for f in shader_cache.iterdir():
        if f.is_file() and f.suffix in (".spv", ".json"):
            dst = out_spirv / f.name
            if not dst.is_file() or dst.stat().st_size != f.stat().st_size:
                shutil.copy2(f, dst)
                copied += 1
    return copied


def load_state(work: Path) -> dict:
    p = work / "setup-state.json"
    try:
        s = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"version": PREP_PS5_VERSION, "done": {}}
    return s if s.get("version") == PREP_PS5_VERSION else {"version": PREP_PS5_VERSION, "done": {}}


def save_state(work: Path, state: dict) -> None:
    state["version"] = PREP_PS5_VERSION
    p = work / "setup-state.json"
    tmp = p.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    tmp.replace(p)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", type=Path, required=True, help="decrypted dump folder (exefs/+romfs/) or .nup file")
    ap.add_argument("--out", type=Path, help=f"output title folder (default: <work>/stage/{TITLE_ID} or ./{TITLE_ID})")
    ap.add_argument("--work", type=Path, help="working directory for generated C, unpacked NUP, and objects")
    ap.add_argument("--bundle", type=Path, help="PS5 builder bundle directory (ps5-bundle/ or build/ps5-game/)")
    ap.add_argument("--romfs", choices=("copy", "skip"), default="copy", help="stage romfs/ into out_dir (default copy)")
    ap.add_argument("--send", metavar="PS5_IP", help="send staged PPSA99193 to Pit Stop TCP receiver (port 19913)")
    ap.add_argument("--deploy", metavar="PS5_IP", help="upload via legacy FTP (deploy.py) instead of PIT1 TCP")
    ap.add_argument("--shader-cache", type=Path, help="reuse translated desktop shaders/ directory if present")
    ap.add_argument("--log", type=Path, help="write detailed subprocess output to this log file")
    ap.add_argument("--events", action="store_true", help="emit machine-readable step lines (default)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compile/shader jobs")
    ap.add_argument("--glslc", help="path to glslc")
    ap.add_argument("--check-only", action="store_true", help="verify dump and builder prerequisites without building")
    args = ap.parse_args()
    own_children()

    if args.out:
        out_dir = args.out.resolve()
        if out_dir.name != TITLE_ID:
            out_dir = out_dir / TITLE_ID
        work = (args.work or out_dir.with_name(f"{TITLE_ID}.work")).resolve()
    elif args.work:
        work = args.work.resolve()
        out_dir = work / "stage" / TITLE_ID
    else:
        out_dir = (Path.cwd() / TITLE_ID).resolve()
        work = out_dir.with_name(f"{TITLE_ID}.work")

    work.mkdir(parents=True, exist_ok=True)
    log_path = args.log.resolve() if args.log else (work / "setup.log")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    sys.stderr = open(log_path, "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    print(f"prepare_ps5: {' '.join(sys.argv)}", file=sys.stderr)

    state = load_state(work)
    done_map: dict[str, str] = state.setdefault("done", {})

    try:
        emit("step", "check")
        src = args.source.resolve()
        exefs_checked, romfs_checked = check(src)
        from_nup = is_nup(src)

        bundle = find_bundle(args.bundle)
        layout = resolve_bundle_layout(bundle)
        if not layout["rt_obj"].is_file():
            raise Failed(f"missing game-free runtime object: {layout['rt_obj']}")
        if not layout["native_tool"].is_file():
            raise Failed(f"missing ps5-native-tool: {layout['native_tool']}")
        if not layout["radv"].is_file():
            raise Failed(f"missing RADV archive: {layout['radv']}")

        if from_nup:
            with NupReader(src) as nr:
                src_id = f"nup:{nr.toc_sha256.hex()}"
        else:
            src_id = f"dir:{exefs_checked}:{romfs_checked}"

        if args.check_only:
            emit("done")
            return 0

        jobs = args.jobs or max(1, min(os.cpu_count() or 1, int(mem_gb() * 0.6)))
        tools_root = PKG_DIR if (PKG_DIR / "setup").is_dir() else ROOT
        glslc = args.glslc or default_glslc(tools_root)

        if from_nup:
            nup_dir = work / "nup"
            if done_map.get("nup") == src_id and (nup_dir / "exefs/main").is_file() and (nup_dir / "romfs").is_dir():
                emit("skip", "nup")
            else:
                emit("step", "nup")
                with NupReader(src) as reader:
                    reader.unpack_tree("exefs", nup_dir / "exefs")
                    reader.unpack_tree("romfs", nup_dir / "romfs", progress_cb=lambda d, t: emit("progress", d, t))
                done_map["nup"] = src_id
                save_state(work, state)
            exefs_src, romfs_src = nup_dir / "exefs", nup_dir / "romfs"
        else:
            emit("skip", "nup")
            exefs_src, romfs_src = exefs_checked, romfs_checked

        if done_map.get("exefs") == src_id and (out_dir / "exefs/main").is_file():
            emit("skip", "exefs")
        else:
            emit("step", "exefs")
            copy_tree(exefs_src, out_dir / "exefs", sum(1 for p in exefs_src.iterdir() if p.is_file()))
            done_map["exefs"] = src_id
            save_state(work, state)

        rom_key = f"{args.romfs}:{src_id}"
        if args.romfs == "skip":
            emit("skip", "romfs")
        elif done_map.get("romfs") == rom_key and (out_dir / "romfs").is_dir():
            emit("skip", "romfs")
        else:
            emit("step", "romfs")
            copy_tree(romfs_src, out_dir / "romfs", ROMFS_FILES)
            done_map["romfs"] = rom_key
            save_state(work, state)

        gen = work / "generated"
        gen_key = f"{PROFILE}:{src_id}:{hashlib.sha256((ROOT / 'recomp/patches-mkw.txt').read_bytes()).hexdigest()[:16]}"
        if done_map.get("generate") == gen_key and (gen / "layout.json").is_file():
            emit("skip", "generate")
        else:
            emit("step", "generate")
            run([sys.executable, str(ROOT / "recomp/gen.py"), "--guest-layout", PROFILE,
                 "--exefs", str(exefs_src), "--out", str(gen), "--jobs", str(jobs),
                 "--patches", str(ROOT / "recomp/patches-mkw.txt")])
            done_map["generate"] = gen_key
            save_state(work, state)

        rt_id = layout["rt_game_id"]
        comp_key = f"{gen_key}:{rt_id}:{layout['is_zig']}"
        if done_map.get("compile") == comp_key and (work / "lift-obj/modules.o").is_file():
            emit("skip", "compile")
            lift_objs = sorted((work / "lift-obj").glob("*.o"))
        else:
            emit("step", "compile")
            with output_lock(gen):
                lift_objs = compile_ps5_game(layout, exefs_src, work, jobs)
            done_map["compile"] = comp_key
            save_state(work, state)

        rt_hash = hashlib.sha256(layout["rt_obj"].read_bytes()).hexdigest()[:16]
        link_key = f"{comp_key}:{rt_hash}"
        if done_map.get("link") == link_key and (out_dir / "eboot.bin").is_file() and (out_dir / "pitstop-game.id").is_file():
            emit("skip", "link")
        else:
            emit("step", "link")
            link_ps5_eboot(layout, layout["rt_obj"], lift_objs, work, out_dir)
            done_map["link"] = link_key
            save_state(work, state)

        sh_key = f"{src_id}:{hashlib.sha256((ROOT / 'tools/shaders/translate.py').read_bytes()).hexdigest()[:16]}"
        if done_map.get("shaders") == sh_key and (out_dir / "spirv").is_dir():
            emit("skip", "shaders")
        else:
            emit("step", "shaders")
            spec = find_existing_spec(args.shader_cache, bundle)
            if spec is None:
                host_target = "windows" if os.name == "nt" else "linux"
                host_cc = default_cc(tools_root, host_target)
                spec = sm86_spec(work, work, host_cc)
            copied = seed_shader_cache(args.shader_cache, out_dir / "spirv")
            if copied:
                print(f"shaders: reused {copied} cached shader files from {args.shader_cache}", file=sys.stderr)
            sh_dir = work / "shaders"
            run([sys.executable, str(ROOT / "tools/shaders/extract.py"), "--romfs", str(romfs_src),
                 "--exefs", str(exefs_src), "--out", str(sh_dir)])
            run([sys.executable, str(ROOT / "tools/shaders/translate.py"), "--shaders", str(sh_dir),
                 "--out", str(out_dir / "spirv"), "--glslc", glslc, "--no-validate", "--progress",
                 "--jobs", str(args.jobs or shader_jobs()), "--spec", str(spec)],
                env=tool_env(glslc), progress_prefix="progress")
            for gfile in (out_dir / "spirv").glob("*.glsl"):
                gfile.unlink()
            done_map["shaders"] = sh_key
            save_state(work, state)

        if args.send:
            emit("step", "deploy")
            cache_tsv = work / "send-sha256.tsv"
            try:
                sent, skipped, total_bytes = send_directory(
                    args.send,
                    out_dir,
                    cache_path=cache_tsv,
                    on_progress=lambda bd, bt, fd, ft: emit("progress", fd, ft),
                )
            except (TransferError, OSError) as e:
                raise Failed(f"sending to PS5 {args.send} failed: {e}") from None
            print(f"deploy: sent {sent} files ({total_bytes / (1024**2):.1f} MiB), skipped {skipped} unchanged",
                  file=sys.stderr)
        elif args.deploy:
            emit("step", "deploy")
            run([sys.executable, str(HERE / "deploy.py"), args.deploy, "--game"])
        else:
            emit("skip", "deploy")

        emit("done")
        return 0
    except (Failed, NupError) as e:
        emit("error", "failed", f"{e} (see {log_path.name})")
        return 1
    except OSError as e:
        emit("error", "io", f"{e} (see {log_path.name})")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
