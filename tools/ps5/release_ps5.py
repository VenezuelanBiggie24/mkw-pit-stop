#!/usr/bin/env python3
"""Pack the game-free PS5 placeholder title zip (PPSA99193) and self-contained Linux/Windows PS5 builder releases.

Why: a PS5 title release cannot carry game code (linked into eboot.bin), exefs/, romfs/, or translated shaders
(spirv/). Instead, this script produces:
  1. mkw-pit-stop-<ver>-ps5-placeholder-PPSA99193.zip — the console-installable game-free PPSA99193/ title (links
     only loader-inventory.o + RADV + weak rt_game_builtin = NULL), which shows the "Game files missing" screen
     with the console IP and built-in TCP receiver.
  2. Self-contained Linux (.tar.zst) and Windows (.zip) builder packages holding the PC builder (recomp/, sm86/,
     tools/, zig + python + glslc when built with --with-toolchain, ps5-native-tool[.exe], and the self-contained
     ps5-bundle/ with app_crt.o, libSceAgc*.so, SDK headers/stubs, RADV & deps archives, libc.prx, sce_sys).
     When build/dist/<target>/mkw[.exe] exists, it is bundled too so players launch the UI directly and click
     "Build for PS5".
  3. SHA256SUMS and release notes, after check_no_game_ps5() passes on every staged asset.
  4. LEGAL.txt and licenses/ in the placeholder, each builder package and its ps5-bundle/, and source/*.tar.gz with
     the source of every part (release_notices.py): the PS5 parts link GPL-3.0-or-later code, so these releases are
     GPL-3.0-or-later as a whole.

Usage: python3 tools/ps5/release_ps5.py --version 0.1.0 [--target all|linux|windows|placeholder]
                                        [--with-toolchain] [--include-radv] [--check-dir DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from compression import zstd
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/release/ps5"
GAME_WORK = ROOT / "build/ps5-game"
TITLE_ID = "PPSA99193"
PROFILE = "ps5-high144"

for _p in (ROOT / "tools/build", ROOT / "tools/ps5", ROOT / "tools/prepare", ROOT / "recomp"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import build  # noqa: E402
import dist  # noqa: E402
import prepare_ps5  # noqa: E402
import release_notices  # noqa: E402

GAME_MARKER = ("MKW_RECOMP_" + "GENERATED_FROM_GAME_CODE").encode()
LIFTED_SYMBOL_RE = re.compile(rb"\bf_(?:28|70)[0-9a-f]{6,10}\b")
FORBIDDEN_EXTS = {".nso", ".nca", ".nsp", ".xci", ".keys", ".bfsha", ".bntx", ".zs", ".spv"}
FORBIDDEN_NAMES = {
    "main", "sdk", "subsdk0", "subsdk1", "rtld", "instructions.json", "sm_86_instructions.txt",
    "eboot.bin", "mkw.sym", "seq_nodes.tsv",
}
SOURCE_ALLOW = {"gen.py", "release.py", "release_ps5.py", "prepare.py"}
# The sources the PS5 builder runs: the desktop package's list (dist.SOURCES: what prepare_ps5.py imports, kept with
# it upstream) and the PS5 release's own tools. Its own copy of the list fell behind (no tools/i18n/catalog.py, which
# build.py imports since 519de2c): the 18/11/2026 builder stopped at its first import.
PS5_SOURCES = [
    "LICENSE",
    "README.md",
    "tools/build/icon.py",
    "tools/ps5/build_game.py",
    "tools/ps5/build_smoke.py",
    "tools/ps5/build_radv.py",
    "tools/ps5/release_ps5.py",
    "tools/ps5/release_notices.py",
    "tools/ps5/LICENSE-llvm.TXT",
]
SOURCES = list(dict.fromkeys(dist.SOURCES + PS5_SOURCES))


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def check_no_game_ps5(root: Path, allow_placeholder_eboot: bool = False) -> list[str]:
    """Return a list of game-content violations under root (empty when clean)."""
    errors: list[str] = []
    for p in sorted(root.rglob("*")):
        if p.is_symlink() or not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        if p.suffix.lower() in FORBIDDEN_EXTS:
            errors.append(f"{rel}: forbidden game/shader file extension ({p.suffix})")
            continue
        if p.name in FORBIDDEN_NAMES:
            if not (allow_placeholder_eboot and p.name == "eboot.bin"):
                errors.append(f"{rel}: forbidden game/binary file name ({p.name})")
                continue
        rel_parts = p.relative_to(root).parts
        if rel_parts[:2] != ("tools", "romfs") and rel_parts[:4] != ("setup", "src", "tools", "romfs") and any(
            part in ("exefs", "romfs", "spirv", "generated", "lift", "lift-pgo", "lift-O2", "lift-pgo-O2") for part in rel_parts
        ):
            errors.append(f"{rel}: forbidden game/generated directory in release")
            continue
        data = p.read_bytes()
        if p.name not in SOURCE_ALLOW and GAME_MARKER in data:
            errors.append(f"{rel}: contains generated game code marker")
        if p.suffix in (".o", ".a", ".so", ".elf", ".bin", ".sym") and LIFTED_SYMBOL_RE.search(data):
            errors.append(f"{rel}: binary/object contains lifted guest function symbols (f_...)")
    return errors


def copy_sources(dst: Path) -> None:
    skip = shutil.ignore_patterns("__pycache__", "*.pyc")
    for rel in SOURCES:
        s, d = ROOT / rel, dst / rel
        d.parent.mkdir(parents=True, exist_ok=True)
        if s.is_dir():
            shutil.copytree(s, d, ignore=skip, dirs_exist_ok=True)
        elif s.is_file():
            shutil.copy2(s, d)
        else:   # a missing source used to be skipped quietly, and the player's builder failed instead
            raise SystemExit(f"release_ps5: source {rel} is missing")


def _ensure_host_zig() -> Path:
    """Return host Linux zig executable from DL cache (downloading pinned tarball if absent)."""
    host = dist.DL / "zig-host"
    exe = host / "zig"
    if exe.is_file() and (host / "lib/std/std.zig").is_file():
        return exe
    dist.DL.mkdir(parents=True, exist_ok=True)
    archive = dist.fetch(*dist.PINS["linux"]["zig"])
    dist.unpack_top(archive, host)
    return exe


def _build_windows_native_tool(vulkan: Path, zig: Path, out_exe: Path) -> None:
    """Cross-compile static Windows ps5-native-tool.exe from PS5_Vulkan/tooling/native + zlib using zig c++."""
    zlib_src = vulkan / ".deps/native/zlib/zlib-1.3.2"
    if not zlib_src.is_dir():
        cands = sorted((vulkan / ".deps/native/zlib").glob("zlib-*"))
        zlib_src = next((c for c in cands if c.is_dir()), zlib_src)
    if not zlib_src.is_dir():
        raise FileNotFoundError(f"missing zlib source under {vulkan / '.deps/native/zlib'}")

    zlib_c = [
        "adler32.c", "compress.c", "crc32.c", "deflate.c", "gzclose.c", "gzlib.c", "gzread.c", "gzwrite.c",
        "infback.c", "inffast.c", "inflate.c", "inftrees.c", "trees.c", "uncompr.c", "zutil.c",
    ]
    native = vulkan / "tooling/native"
    cpp_srcs = [
        native / "native_app_builder.cpp",
        native / "self_container.cpp",
        native / "elf_object.cpp",
        native / "sce_module_writer.cpp",
    ]
    out_exe.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        zobjs = []
        env = {**os.environ, "ZIG_LIB_DIR": str(zig.parent / "lib"), "ZIG_GLOBAL_CACHE_DIR": str(t / "zcache")}
        for name in zlib_c:
            obj = t / f"z_{name}.o"
            subprocess.run([str(zig), "cc", "-target", "x86_64-windows-gnu", "-O2", f"-I{zlib_src}",
                            "-c", str(zlib_src / name), "-o", str(obj)], env=env, check=True)
            zobjs.append(str(obj))
        subprocess.run([
            str(zig), "c++", "-target", "x86_64-windows-gnu", "-std=c++20", "-O2", "-static",
            f"-I{zlib_src}", *(str(s) for s in cpp_srcs), *zobjs, "-o", str(out_exe),
        ], env=env, check=True)


def stage_ps5_bundle(bundle: Path, target_os: str, version: str, include_radv: bool) -> None:
    """Build a self-contained SDK-free ps5-bundle/ for linux or windows."""
    rt_obj = GAME_WORK / "inventory-objects/loader-inventory.o"
    if not rt_obj.is_file():
        sys.exit(f"missing {rt_obj}: run build_game.py --inventory first")

    title = GAME_WORK / "title"
    vulkan = GAME_WORK / "PS5_Vulkan"
    sdk = title / ".deps/native/ps5-payload-sdk"
    deps_dir = ROOT / "build/ps5-stack/deps"
    zig = _ensure_host_zig()

    bundle.mkdir(parents=True, exist_ok=True)
    shutil.copy2(rt_obj, bundle / "loader-inventory.o")

    rt_id = build.game_id(PROFILE)
    (bundle / "pitstop-runtime.json").write_text(
        json.dumps({"title_id": TITLE_ID, "port_version": version, "rt_game_id": rt_id, "profile": PROFILE},
                   indent=2) + "\n",
        encoding="utf-8",
    )

    # Precompiled PS5 link support objects/stubs + SDK & RADV static libraries
    lib_dir = bundle / "lib"
    lib_dir.mkdir(exist_ok=True)
    env = {**os.environ, "ZIG_LIB_DIR": str(zig.parent / "lib")}
    clang = shutil.which("clang-22") or shutil.which("clang") or str(sdk / "bin/prospero-clang")
    base_ps5 = [
        "-target", "x86_64-sie-ps5", "-fvisibility-nodllstorageclass=default",
        "-isysroot", str(sdk), "-isystem", str(sdk / "target/include/c++/v1"),
        "-isystem", str(sdk / "target/include"), "-fno-stack-protector", "-fno-plt", "-femulated-tls",
        "-fdenormal-fp-math=ieee",
    ]
    app_crt = lib_dir / "app_crt.o"
    subprocess.run([clang, *base_ps5, "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti",
                    "-c", str(vulkan / "tooling/native/app_crt.cpp"), "-o", str(app_crt)], check=True)

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        for lib, src in (
            ("libSceAgc", "vendor/ps5/sdk/stubs/agc_canary_link_stub.c"),
            ("libSceAgcDriver", "vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c"),
        ):
            so_obj = t / f"{lib}.o"
            subprocess.run([clang, *base_ps5, "-std=c11", "-O2", "-fPIC",
                            "-c", str(vulkan / src), "-o", str(so_obj)], check=True)
            subprocess.run([str(zig), "ld.lld", "--shared", "-soname", f"{lib}.prx",
                            "-o", str(lib_dir / f"{lib}.so"), str(so_obj)], env=env, check=True)

    for static_lib in (
        sdk / "target/lib/libps5platform.a",
        sdk / "target/lib/libc++.a",
        sdk / "target/lib/libc++abi.a",
        sdk / "target/lib/libunwind.a",
        deps_dir / "opus/lib/libopus.a",
        deps_dir / "libvpx/lib/libvpx.a",
        deps_dir / "zstd-1.5.7/lib/libzstd.a",
    ):
        if not static_lib.is_file():
            raise FileNotFoundError(f"missing static PS5 dependency archive: {static_lib}")
        shutil.copy2(static_lib, lib_dir / static_lib.name)

    builtins_r = subprocess.run([clang, "--print-resource-dir"], capture_output=True, text=True, check=True)
    builtins = Path(builtins_r.stdout.strip()) / "lib/linux/libclang_rt.builtins-x86_64.a"
    if builtins.is_file():
        shutil.copy2(builtins, lib_dir / builtins.name)

    radv = vulkan / ".deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
    if include_radv and radv.is_file():
        shutil.copy2(radv, lib_dir / radv.name)
        prov = radv.parents[1] / "PROVENANCE.txt"
        if prov.is_file():
            shutil.copy2(prov, lib_dir / "PROVENANCE.txt")

    # Linker scripts
    ld_dir = bundle / "ld"
    ld_dir.mkdir(exist_ok=True)
    for ldf in (vulkan / "tooling/psbc/ps5-pie-unwind.ld",
                vulkan / "tooling/native/ps5-pie.ld",
                vulkan / "tooling/native/app-symbols.map"):
        shutil.copy2(ldf, ld_dir / ldf.name)

    # SDK stub .so libraries, libpthread.a (libc++abi/libunwind .deplibs), and target C headers
    sdk_lib_dst = bundle / "sdk_lib"
    sdk_lib_dst.mkdir(exist_ok=True)
    for so in sorted((sdk / "target/lib").glob("*.so")):
        shutil.copy2(so, sdk_lib_dst / so.name)
    for empty_ar in ("libpthread.a", "libdl.a"):
        p_ar = sdk / "target/lib" / empty_ar
        if p_ar.is_file():
            shutil.copy2(p_ar, sdk_lib_dst / empty_ar)
    sdk_inc_src = sdk / "target/include"
    if sdk_inc_src.is_dir():
        shutil.copytree(sdk_inc_src, bundle / "sdk_include", dirs_exist_ok=True)

    # ps5-native-tool binary for target OS
    bin_dir = bundle / "bin"
    bin_dir.mkdir(exist_ok=True)
    if target_os == "windows":
        _build_windows_native_tool(vulkan, zig, bin_dir / "ps5-native-tool.exe")
    else:
        tool_src = vulkan / "build/host/ps5-native-tool"
        if tool_src.is_file():
            shutil.copy2(tool_src, bin_dir / "ps5-native-tool")
            (bin_dir / "ps5-native-tool").chmod(0o755)

    # sce_sys, libc.prx, and static notices (so player builds need zero git checkouts)
    sce_sys = bundle / "sce_sys"
    sce_sys.mkdir(exist_ok=True)
    param_src = title / "ps5/sce_sys/param.json"
    if param_src.is_file():
        param = param_src.read_text(encoding="utf-8").replace('"Mario Kart World PS5"', '"Mario Kart World: Pit Stop"')
        (sce_sys / "param.json").write_text(param, encoding="utf-8")
    icon = ROOT / "docs/images/icon0.png"
    if icon.is_file():
        shutil.copy2(icon, sce_sys / "icon0.png")

    sce_mod = bundle / "sce_module"
    sce_mod.mkdir(exist_ok=True)
    for mf in ("libc.prx", "libc.prx.sha256"):
        src_m = vulkan / "runtime" / mf
        if src_m.is_file():
            shutil.copy2(src_m, sce_mod / mf)

    # LEGAL.txt and licenses/ for every part (GPL-3.0-or-later as a whole); prepare_ps5 copies them into each title
    release_notices.stage(bundle)


def build_placeholder_title(bundle: Path, out_title: Path, version: str) -> None:
    """Link the game-free placeholder PPSA99193/eboot.bin (rt_game_builtin() == NULL) using bundle."""
    zig = _ensure_host_zig()
    layout = prepare_ps5.resolve_bundle_layout(bundle)
    # Force host Linux zig ld.lld & ps5-native-tool regardless of bundle target_os
    vulkan = GAME_WORK / "PS5_Vulkan"
    layout["ld_cmd"] = [str(zig), "ld.lld"]
    layout["native_tool"] = vulkan / "build/host/ps5-native-tool"
    if not layout["radv"].is_file():
        radv = vulkan / ".deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
        layout["radv"] = radv
    layout["port_version"] = version

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        env = {**os.environ, "ZIG_LIB_DIR": str(zig.parent / "lib")}
        stub_c = work / "builtin_null.c"
        stub_o = work / "builtin_null.o"
        stub_c.write_text("const void *rt_game_builtin(void) { return 0; }\n", encoding="utf-8")
        subprocess.run([str(zig), "cc", "-target", "x86_64-freebsd", "-mcpu=znver2", "-fPIC", "-O2",
                        "-c", str(stub_c), "-o", str(stub_o)], env=env, check=True)
        prepare_ps5.link_ps5_eboot(layout, layout["rt_obj"], [stub_o], work, out_title)
        # Keep placeholder free of game/profiling tables (mkw.sym, seq_nodes.tsv) and game id
        (out_title / "mkw.sym").unlink(missing_ok=True)
        (out_title / "seq_nodes.tsv").unlink(missing_ok=True)
        (out_title / "pitstop-game.id").unlink(missing_ok=True)


def _populate_toolchain(setup_dir: Path, target_os: str) -> None:
    """Stage pinned python, zig, and glslc into setup_dir (reusing dist.py's DL cache)."""
    dist.DL.mkdir(parents=True, exist_ok=True)
    pins = dist.PINS[target_os]
    got = {name: dist.fetch(url, want) for name, (url, want) in pins.items()}
    dist.unpack_top(got["python"], setup_dir / "python")
    site = setup_dir / "python" / ("Lib/site-packages" if target_os == "windows" else "lib/python3.14/site-packages")
    for wheel in ("numpy", "lz4"):
        dist.unpack_wheel(got[wheel], site)
    dist.unpack_top(got["zig"], setup_dir / "zig")
    glslc = setup_dir / "glslc"
    shutil.rmtree(glslc, ignore_errors=True)
    for name in ("shaderc", "glslang", "spirv-tools", "libstdcxx", "libgcc", "vc14_runtime"):
        if name in got:
            dist.unpack_conda(got[name], glslc)
    if target_os == "windows":
        lib_bin = glslc / "Library/bin"
        if lib_bin.is_dir():
            for p in lib_bin.iterdir():
                if p.suffix in (".exe", ".dll"):
                    shutil.copy2(p, glslc / p.name)
        dist_win_crt = ROOT / "build/dist/windows/setup/lib/crt"
        if dist_win_crt.is_dir():
            shutil.copytree(dist_win_crt, setup_dir / "lib/crt", dirs_exist_ok=True)
        else:
            dist.windows_crt(setup_dir / "lib/crt")


def _stage_builder_tree(stage: Path, target_os: str, version: str, include_radv: bool, with_toolchain: bool) -> None:
    """Populate a Linux or Windows PS5 builder directory tree cleanly."""
    setup_dir = stage / "setup"
    src_dir = setup_dir / "src"
    copy_sources(src_dir)
    stage_ps5_bundle(setup_dir / "ps5-bundle", target_os, version, include_radv=True)
    release_notices.stage(stage)   # the package's own LEGAL.txt and licenses/, where a player sees them first
    if with_toolchain:
        _populate_toolchain(setup_dir, target_os)

    # Optionally include the desktop+PS5 launcher executable (mkw / mkw.exe) when build/dist/<target>/ has it
    dist_dir = ROOT / "build/dist" / target_os
    exe_name = "mkw.exe" if target_os == "windows" else "mkw"
    if (dist_dir / exe_name).is_file():
        shutil.copy2(dist_dir / exe_name, stage / exe_name)
        if target_os == "windows":
            for dll in dist.WIN_DLLS:
                if (dist_dir / dll).is_file():
                    shutil.copy2(dist_dir / dll, stage / dll)
            if (dist_dir / "setup/lib/libmkw.dll.a").is_file():
                (setup_dir / "lib").mkdir(parents=True, exist_ok=True)
                shutil.copy2(dist_dir / "setup/lib/libmkw.dll.a", setup_dir / "lib/libmkw.dll.a")

    # Root CLI launcher wrapper
    wrapper = stage / "prepare_ps5.py"
    wrapper.write_text(
        "#!/usr/bin/env python3\n"
        "import runpy, sys\nfrom pathlib import Path\n"
        "sys.exit(runpy.run_path(str(Path(__file__).resolve().parent / 'setup/src/tools/ps5/prepare_ps5.py'), "
        "run_name='__main__'))\n",
        encoding="utf-8",
    )
    wrapper.chmod(0o755)
    if target_os == "windows":
        (stage / "prepare_ps5.bat").write_text(
            "@echo off\r\n"
            "setlocal\r\n"
            "\"%~dp0setup\\python\\python.exe\" -I \"%~dp0setup\\src\\tools\\ps5\\prepare_ps5.py\" %*\r\n",
            encoding="utf-8",
        )


def _write_zip(src_dir: Path, root_arcname: str, out_zip: Path) -> None:
    with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for p in sorted(src_dir.rglob("*")):
            if p.is_symlink() or not p.is_file():
                continue
            arc = f"{root_arcname}/{p.relative_to(src_dir).as_posix()}" if root_arcname else p.relative_to(src_dir).as_posix()
            zf.write(p, arc)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", default="0.1.0", help="release version, e.g. 0.1.0")
    ap.add_argument("--changelog-section", help="CHANGELOG.md section name (default: ps5-<version>; a fixed ps5-0.1.0 "
                    "default gave the first 0.2.0 pack 0.1.0's notes)")
    ap.add_argument("--target", choices=("all", "linux", "windows", "placeholder"), default="all",
                    help="which release asset(s) to build (default: all)")
    ap.add_argument("--include-radv", action="store_true",
                    help="kept for compatibility; ps5-bundle always includes libvulkan_radeon.ps5.a")
    ap.add_argument("--with-toolchain", action="store_true",
                    help="bundle pinned portable python, zig, and glslc inside builder releases")
    ap.add_argument("--check-dir", type=Path, help="only run the no-game-content check on DIR and exit")
    ap.add_argument("--allow-placeholder-eboot", action="store_true",
                    help="with --check-dir: allow a game-free placeholder eboot.bin while scanning its symbols")
    args = ap.parse_args()

    if args.check_dir:
        errors = check_no_game_ps5(args.check_dir.resolve(), allow_placeholder_eboot=args.allow_placeholder_eboot)
        if errors:
            for err in errors:
                print(f"release_ps5: FAIL: {err}", file=sys.stderr)
            return 1
        print(f"release_ps5: {args.check_dir} passed game-content check (0 violations)")
        return 0

    OUT.mkdir(parents=True, exist_ok=True)
    top_name = f"mkw-pit-stop-{args.version}"
    artifacts: list[Path] = []

    with tempfile.TemporaryDirectory(dir=OUT) as tmp:
        troot = Path(tmp)
        # Always stage Linux ps5-bundle first so placeholder title can link from it too.
        placeholder_bundle = troot / "ph-bundle"
        stage_ps5_bundle(placeholder_bundle, "linux", args.version, include_radv=True)

        if args.target in ("all", "placeholder"):
            ph_stage = troot / TITLE_ID
            build_placeholder_title(placeholder_bundle, ph_stage, args.version)
            errors = check_no_game_ps5(ph_stage, allow_placeholder_eboot=True)
            if errors:
                for err in errors:
                    print(f"release_ps5: FAIL (placeholder): {err}", file=sys.stderr)
                return 1
            ph_zip = OUT / f"{top_name}-ps5-placeholder-{TITLE_ID}.zip"
            _write_zip(ph_stage, TITLE_ID, ph_zip)
            artifacts.append(ph_zip)
            print(f"{ph_zip}: {ph_zip.stat().st_size / 2**20:.1f} MiB (placeholder clean)")

        if args.target in ("all", "linux"):
            lin_stage = troot / f"{top_name}-linux"
            _stage_builder_tree(lin_stage, "linux", args.version, args.include_radv, args.with_toolchain)
            errors = check_no_game_ps5(lin_stage)
            if errors:
                for err in errors:
                    print(f"release_ps5: FAIL (linux): {err}", file=sys.stderr)
                return 1
            lin_arc = OUT / f"{top_name}-ps5-builder-linux-x86_64.tar.zst"
            with zstd.open(lin_arc, "wb", level=9) as zf, tarfile.open(fileobj=zf, mode="w|") as t:
                t.add(lin_stage, arcname=top_name)
            artifacts.append(lin_arc)
            print(f"{lin_arc}: {lin_arc.stat().st_size / 2**20:.1f} MiB (clean)")

        if args.target in ("all", "windows"):
            win_stage = troot / f"{top_name}-windows"
            _stage_builder_tree(win_stage, "windows", args.version, args.include_radv, args.with_toolchain)
            errors = check_no_game_ps5(win_stage)
            if errors:
                for err in errors:
                    print(f"release_ps5: FAIL (windows): {err}", file=sys.stderr)
                return 1
            win_zip = OUT / f"{top_name}-ps5-builder-windows-x86_64.zip"
            _write_zip(win_stage, top_name, win_zip)
            artifacts.append(win_zip)
            print(f"{win_zip}: {win_zip.stat().st_size / 2**20:.1f} MiB (clean)")

    notes_path = OUT / f"notes-ps5-{args.version}.md"
    subprocess.run([sys.executable, str(ROOT / "tools/build/notes.py"), "--version", args.changelog_section or f"ps5-{args.version}",
                    "--template", str(ROOT / "docs/ps5/release-template-ps5.md"), "--out", str(notes_path)], check=True)
    artifacts.append(notes_path)

    icon_dst = OUT / "icon0.png"
    if (ROOT / "docs/images/icon0.png").is_file():
        shutil.copy2(ROOT / "docs/images/icon0.png", icon_dst)
        artifacts.append(icon_dst)

    # The complete source of the GPL parts (and of mkw-recomp and the title foundation), attached to the same release:
    # GPL-3.0 section 6(a) and (d), since the placeholder and ps5-bundle/ are distributed as object code
    artifacts += release_notices.source_archives(OUT)

    sums_path = OUT / "SHA256SUMS"
    sums = [(sha256(p), p.relative_to(OUT).as_posix()) for p in artifacts if p.is_file()]
    sums_path.write_text("".join(f"{h}  {n}\n" for h, n in sums), encoding="utf-8")
    print(f"{notes_path}")
    print(f"{sums_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
