"""Cross-build the MKW runtime and its local lift as a private PS5 title.

Use `uv run tools/ps5/build_game.py --stack DIR --inventory` for the
game-free compile and import report. Add the completed PS5 lift and extracted
ExeFS with `--generated DIR --exefs DIR` to build the loader-stage title.

`--pgo-use FILE` compiles the generated C with a profile (`llvm-profdata merge` of the .profraw files of a Linux
build with `tools/build/build.py --pgo-gen`, same generated tree): objects in `game-objects/lift-pgo`, so the
build without it keeps its own, and the clang warnings about functions whose profile is missing or out of date
are summed up at the end (a generator change since the profile shows there).
"""

import argparse
import json
import re
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from contextlib import nullcontext
from pathlib import Path

from build_smoke import (APP_SDK, GLM, MESA, ROOT, TEMPLATE, VULKAN, png, require_checkout,
                         require_radv, write_changed)
from build import game_id, module_glue, port_version

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "i18n"))
import catalog  # noqa: E402  (tools/i18n: the menu texts of every language, from i18n/*.po)
from layout import output_lock, placement_defines, read_checked

WORK = ROOT / "build/ps5-game"
PROFILE = "ps5-high144"
TITLE_ID = "PPSA99193"
TITLE_NAME = "Mario Kart World: Pit Stop"
ICON = ROOT / "docs/images/icon0.png"
# param.json flexibleMemorySize, MiB. Flexible and direct memory share the app's budget on the PS5: 1024 left 11,712
# MiB of direct memory, which long sessions ran out of (docs/roadmap.md section 4), while flexible memory peaked at
# ~460-490 MiB in use ("flex free 562 MiB" in run pack1, 16/11/2026; task 38's report). 640 keeps ~150 MiB of that
# spare and should give the direct memory ~384 MiB more; the census line's "dmem free N of M MiB" shows M.
FLEXIBLE_MIB = 640
RUNTIME = ROOT / "runtime"
PLATFORM_SOURCES = {
    "common", "prot_watch", "ps5_guest_memory", "ps5_memory", "ps5_mixed", "ps5_loader_io",
    "ps5_mutex", "ps5_threads", "ps5_fiber", "ps5_recv", "ps5_sys", "sigsample", "fault_guard",
}
SDL_EXCLUDES = {
    "gfx_window_sdl.c", "input_sdl.c", "menu_sdl.cpp",
    "imgui_impl_sdl3.cpp", "imgui_impl_sdlrenderer3.cpp",
}


def source_files() -> list[Path]:
    sources = []
    for source in RUNTIME.rglob("*"):
        if source.suffix not in (".c", ".cpp"):
            continue
        relative = source.relative_to(ROOT)
        if relative.parts[:3] == ("runtime", "ui", "launcher"):
            continue
        if relative.parts[:2] == ("runtime", "platform") and source.stem not in PLATFORM_SOURCES:
            continue
        if source.stem.endswith("_win"):
            continue
        if source.name in SDL_EXCLUDES:
            continue
        sources.append(source)
    return sorted(sources)


def title_setup(template: Path, title: Path) -> None:
    if not title.is_dir():
        # new-title.py rejects ':' in --name; pass a plain name and set the full title in param.json below.
        safe_name = re.sub(r"[^A-Za-z0-9 ._-]", "", TITLE_NAME)[:40].strip()
        identity = dict(os.environ, GIT_AUTHOR_NAME=TITLE_NAME, GIT_COMMITTER_NAME=TITLE_NAME,
                        GIT_AUTHOR_EMAIL="mkw@localhost", GIT_COMMITTER_EMAIL="mkw@localhost")
        with tempfile.TemporaryDirectory(dir=title.parent) as directory:
            staged = Path(directory) / "title"
            subprocess.run([sys.executable, str(template / "ps5/tools/new-title.py"), str(staged),
                            "--title-id", TITLE_ID, "--name", safe_name, "--refresh", "60",
                            "--flexible-memory", str(FLEXIBLE_MIB)], check=True, env=identity)
            (staged / "ps5/sce_sys/icon0.png").unlink(missing_ok=True)
            staged.rename(title)
    # The home screen icon: our own "MKW: Pit Stop" drawing (tools/build/pit_stop_icon.py), rewritten on every build so a
    # title made before it, or a changed drawing, picks it up; link-title.sh copies it into dist/<title>/sce_sys.
    icon = title / "ps5/sce_sys/icon0.png"
    write_changed(icon, ICON.read_bytes() if ICON.is_file() else png(512))
    param_path = title / "ps5/sce_sys/param.json"
    param = json.loads(param_path.read_text(encoding="utf-8"))
    changed = False
    loc = param.setdefault("localizedParameters", {}).setdefault("en-US", {})
    if loc.get("titleName") != TITLE_NAME:
        loc["titleName"] = TITLE_NAME
        changed = True
    kernel = param.setdefault("kernel", {})
    if kernel.get("flexibleMemorySize") != FLEXIBLE_MIB << 20:
        kernel["flexibleMemorySize"] = FLEXIBLE_MIB << 20
        changed = True
    if "flexibleMemorySize" in param:   # a top-level key does nothing: the kernel section's is the one read
        param.pop("flexibleMemorySize")
        changed = True
    if "pagetableMemorySizeInMib" in param.get("amm", {}):
        param["amm"].pop("pagetableMemorySizeInMib", None)
        if not param["amm"]:
            param.pop("amm", None)
        changed = True
    if changed:
        param_path.write_text(json.dumps(param, indent=2) + "\n", encoding="utf-8")


def ninja_path(path: Path) -> str:
    return str(path).replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def compile_objects(sdk: Path, template: Path, includes: list[Path], env: dict[str, str],
                    jobs: int, generated: Path | None, exefs: Path | None,
                    pgo_use: Path | None = None) -> tuple[list[Path], bool]:
    out = WORK / ("game-objects" if generated else "inventory-objects")
    out.mkdir(parents=True, exist_ok=True)
    write_changed(out / "guest_layout.h", ("#pragma once\n" + placement_defines(PROFILE)).encode())
    ident = game_id(PROFILE)
    common = ["-march=znver2", "-fPIC", "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections"]
    include_dirs = sorted({RUNTIME, *(p.parent for p in RUNTIME.rglob("*.h"))})
    volk = template / "ps5/third_party/volk"
    flags = [*common, "-O2", "-DMKW_PS5_NATIVE", "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS",
             f'-DRT_GAME_ID="{ident}"',
             "-fno-jump-tables", "-DVK_NO_PROTOTYPES", "-include", str(volk / "volk.h"),
             *(f"-I{p}" for p in [out, *include_dirs, *includes, template / "external", ROOT / "scratchpad/vma"])]
    genflags = [*common, "-O1", "-Wno-return-type", "-Wno-unused-label", "-fvisibility=hidden",
                f"-I{RUNTIME / 'cpu'}"]
    # PGO changes layout, inlining and branch weights only, never what a guest instruction computes.
    # -Wprofile-instr-missing: a function the profile does not know, i.e. not compiled in the instrumented build.
    if pgo_use is not None:
        genflags += [f"-fprofile-instr-use={pgo_use}", "-Wprofile-instr-missing"]
    ninja = []
    for name, compiler, options in (
        ("cc", "prospero-clang", ["-std=gnu11", *flags]),
        ("cxx", "prospero-clang++", ["-std=c++17", *flags]),
        ("lift", "prospero-clang", [*genflags]),
    ):
        invocation = shlex.join([str(sdk / "bin" / compiler), *options]).replace("$", "$$")
        ninja += [f"rule {name}", f"  command = {invocation} -MMD -MF $out.d -c $in -o $out",
                  "  depfile = $out.d", "  deps = gcc", "  description = compile $in"]
    files = [(source, source.relative_to(ROOT), "cxx" if source.suffix == ".cpp" else "cc")
             for source in source_files()]
    # Use the MIT template implementation itself, including the console log
    # sink and display dispatch loader. No template example main is linked.
    files += [(template / "ps5/src/platform.c", Path("template/platform.c"), "cc"),
              (template / "ps5/third_party/volk/volk.c", Path("template/volk.c"), "cc")]
    defaults = out / "defaults.c"
    defaults_src = (
        '#include "gamelib.h"\n'
        'const char rt_default_exefs[] = "/app0/exefs";\n'
        'const char rt_default_romfs[] = "/app0/romfs";\n'
        'const char rt_default_profile[] = "";\n'
        f"const char rt_port_version[] = {json.dumps(port_version())};\n"
    )
    if generated is None:
        # Weak so the game-free placeholder links alone while strong modules.o overrides it cleanly.
        defaults_src += "__attribute__((weak)) const RtGame *rt_game_builtin(void) { return 0; }\n"
    write_changed(defaults, defaults_src.encode())
    files.append((defaults, Path("defaults.c"), "cc"))
    # the menu texts (ui_text.c's text_<lang> arrays), generated as build.py does
    files += [(path, Path("i18n") / path.name, "cc") for path in sorted(catalog.generate(out / "i18n"))]
    if generated is not None:
        modules = out / "modules.c"
        if exefs is not None:
            write_changed(modules, module_glue(generated, exefs, False, PROFILE).encode())
        elif not modules.is_file():
            raise SystemExit(f"missing {modules}; pass --exefs to generate it")
        flags.append(f"-I{generated}")
        # Only modules.c needs the generated layout/patch metadata.
        invocation = shlex.join([str(sdk / "bin/prospero-clang"), "-std=gnu11", *flags]).replace("$", "$$")
        ninja += ["rule tables", f"  command = {invocation} -MMD -MF $out.d -c $in -o $out",
                  "  depfile = $out.d", "  deps = gcc", "  description = compile game tables"]
        files.append((modules, Path("modules.c"), "tables"))
        lift = "lift-pgo" if pgo_use is not None else "lift"
        for name, _, _ in read_checked(generated, PROFILE):
            files += [(source, Path(lift) / name / source.name, "lift")
                      for source in sorted((generated / name).glob("*.c"))]
    objects = []
    for source, relative, rule in files:
        obj = out / relative.with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        objects.append(obj)
        # a new profile in the same file rebuilds the generated code: the profile is an input
        implicit = f" | {ninja_path(pgo_use)}" if rule == "lift" and pgo_use is not None else ""
        ninja.append(f"build {ninja_path(obj)}: {rule} {ninja_path(source)}{implicit}")
    manifest = out / "build.ninja"
    write_changed(manifest, ("\n".join(ninja) + "\n").encode())
    log = out / "compile.log"
    with log.open("w") as stream:
        result = subprocess.run(["ninja", "-C", str(out), "-f", str(manifest), "-j", str(jobs), "-k", "0"],
                                env=env, stdout=stream, stderr=subprocess.STDOUT, check=False)
    print(f"Compile log: {log}", flush=True)
    if pgo_use is not None:
        pgo_report(log.read_text(errors="replace"))
    failed = {line.removeprefix("FAILED: ").strip() for line in log.read_text().splitlines()
              if line.startswith("FAILED: ")}
    available = [obj for obj in objects if obj.is_file() and str(obj) not in failed]
    if result.returncode:
        print(f"Compile incomplete. The import reports cover only {len(available)} of {len(objects)} objects.")
    return available, result.returncode == 0


def pgo_report(text: str) -> None:
    """Sums clang's per-file profile warnings: "of N functions, M have mismatched data" (the C of M functions is not
    what the profile was taken from: a generator or header change since) and "of N functions, M have no data" (not
    in the instrumented build). Ninja shows a file's warnings only when it compiles it: a no-op build reports nothing."""
    sums = {}
    for kind, pattern in (("mismatched (out of date)", r"of (\d+) functions?, (\d+) (?:has|have) mismatched data"),
                          ("without data (missing)", r"of (\d+) functions?, (\d+) (?:has|have) no data")):
        found = [(int(a), int(b)) for a, b in re.findall(pattern, text)]
        sums[kind] = (len(found), sum(b for _, b in found), sum(a for a, _ in found))
    other = sorted({m for m in re.findall(r"warning: (.*profile.*)", text) if "of " not in m})[:10]
    for kind, (files, bad, total) in sums.items():
        print(f"PGO: {kind}: {bad} of {total} functions in {files} files")
    for line in other:
        print(f"PGO: {line}")


def import_inventory(sdk: Path, objects: list[Path], env: dict[str, str], out: Path, reachable: bool) -> Path | None:
    if not objects:
        return None
    label = "loader" if reachable else "runtime"
    aggregate = out / f"{label}-inventory.o"
    log = out / f"{label}-link.log"
    report = out / f"{label}-imports.txt"
    gc = ["--gc-sections", "-u", "main", "-u", "catchReturnFromMain"] if reachable else []
    result = subprocess.run([str(sdk / "bin/ld.lld"), "-r", *gc, *map(str, objects), "-o", str(aggregate)],
                            env=env, text=True, capture_output=True, check=False)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        report.unlink(missing_ok=True)
        print(f"{label} relocatable link failed. See {log}")
        return None
    output = subprocess.check_output([str(sdk / "bin/llvm-nm"), "--undefined-only", str(aggregate)],
                                     env=env, text=True)
    report.write_text(output)
    print(f"{label} imports from {len(objects)} input objects: {report}")
    return aggregate


def write_symbols(sdk: Path, env: dict[str, str], elf: Path, out: Path) -> None:
    """mkw.sym: the host functions of the unstripped image (link/link/llvm-pie.elf, the eboot's addresses from VA 0),
    "<hex address> <name>" per line, for the sampling profiler's names (runtime/core/diag/prof_posix.c). The eboot
    keeps no symbols. Lifted functions (f_<guest address>) are left out: the runtime names them from its own tables."""
    nm = sdk / "bin/llvm-nm"
    listing = subprocess.run([str(nm if nm.is_file() else "llvm-nm"), "--defined-only", "-n", str(elf)], env=env,
                             text=True, capture_output=True, check=False).stdout
    lines = []
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW" and not parts[2].startswith("f_"):
            lines.append(f"{int(parts[0], 16):x} {parts[2]}")
    out.write_text("\n".join(lines) + "\n")
    print(f"{out}: {len(lines)} host functions")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stack", type=Path, required=True, help="pinned PS5 build checkouts")
    parser.add_argument("--include", type=Path, action="append", default=[], help="additional portable dependency headers")
    parser.add_argument("--library", type=Path, action="append", default=[], help="native static dependency archive")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--inventory", action="store_true", help="compile and report unresolved imports without packaging")
    parser.add_argument("--generated", type=Path, help="completed ps5-high144 lift")
    parser.add_argument("--exefs", type=Path, help="local extracted ExeFS for the built-in module hashes")
    parser.add_argument("--pgo-use", type=Path, metavar="FILE",
                        help="profile for the generated C (.profdata from tools/build/build.py --pgo-gen runs)")
    args = parser.parse_args()
    if not args.inventory and args.generated is None:
        parser.error("use --inventory or specify --generated (with --exefs on first build)")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    generated = args.generated.resolve() if args.generated else None
    pgo_use = args.pgo_use.resolve() if args.pgo_use else None
    if pgo_use is not None and not pgo_use.is_file():
        parser.error(f"--pgo-use: {pgo_use} is not a file")
    exefs = args.exefs.resolve() if args.exefs else None
    includes = [path.resolve() for path in args.include]
    libraries = [path.resolve() for path in args.library]
    stack = args.stack.resolve()
    template, vulkan, mesa, sdk_fork = (stack / name for name in
                                       ("PS5_VulkanTemplate", "PS5_Vulkan", "PS5_Mesa", "PS5_PayloadSDK"))
    for checkout, revision in ((template, TEMPLATE), (template / "external/glm", GLM), (vulkan, VULKAN),
                               (mesa, MESA), (sdk_fork, APP_SDK)):
        require_checkout(checkout, revision)
    WORK.mkdir(parents=True, exist_ok=True)
    title = WORK / "title"
    title_setup(template, title)
    for checkout in (vulkan, mesa, sdk_fork):
        link = WORK / checkout.name
        if link.is_symlink() and link.resolve() != checkout:
            link.unlink()
        if not link.exists():
            link.symlink_to(checkout, target_is_directory=True)
    env = dict(os.environ, PS5_PAYLOAD_SDK_FORK=str(sdk_fork))
    llvm_config = shutil.which("llvm-config-22")
    if llvm_config:
        env.setdefault("LLVM_CONFIG", llvm_config)
    clang22 = shutil.which("clang-22")
    if clang22:
        env.setdefault("PS5_CLANG", clang22)
    sdk = Path(subprocess.check_output(["bash", str(title / "ps5/tools/setup-sdk.sh")], env=env, text=True).strip())
    with output_lock(generated) if generated else nullcontext():
        objects, complete = compile_objects(sdk, template, includes, env, args.jobs, generated, exefs, pgo_use)
    out = WORK / ("game-objects" if generated else "inventory-objects")
    aggregate = import_inventory(sdk, objects, env, out, reachable=False)
    loader = import_inventory(sdk, objects, env, out, reachable=True)
    if args.inventory or not complete:
        return 0 if complete and aggregate and loader else 1
    if not loader:
        return 1
    archive, _ = require_radv(vulkan)
    env["RADV_ARCHIVE"] = str(archive)
    link = WORK / "link"
    link.mkdir(exist_ok=True)
    (link / "samples.txt").write_text("\n")
    log = link / "title-link.log"
    with log.open("w") as stream:
        result = subprocess.run(["bash", str(title / "ps5/tools/link-title.sh"), str(link), str(vulkan), str(sdk),
                                 str(loader), *map(str, libraries)], env=env, stdout=stream,
                                stderr=subprocess.STDOUT, check=False)
    print(f"Title link log: {log}")
    if result.returncode:
        return 1
    package = title / "dist" / TITLE_ID
    write_changed(package / "pitstop-game.id", (game_id(PROFILE) + "\n").encode())
    write_changed(package / "pitstop-runtime.json", (json.dumps({
        "title_id": TITLE_ID, "port_version": port_version(), "rt_game_id": game_id(PROFILE), "profile": PROFILE,
    }, indent=2) + "\n").encode())
    write_symbols(sdk, env, link / "link/llvm-pie.elf", package / "mkw.sym")
    seq_nodes = ROOT / "build/seq_nodes.tsv"
    if seq_nodes.is_file():
        shutil.copy2(seq_nodes, package / "seq_nodes.tsv")
    if exefs is not None:
        destination = package / "exefs"
        destination.mkdir(exist_ok=True)
        for source in sorted(exefs.iterdir()):
            if source.is_file() and not source.name.startswith("."):
                shutil.copy2(source, destination / source.name)
    print(package)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
