"""The 1.0.0 dump check, copies and tool runs of the PS5 builder (tools/ps5/prepare_ps5.py).

Why: prepare_ps5.py used these from tools/prepare/prepare.py, which upstream split into modules with other signatures
(several game versions, .nx packages; 17/11/2026). The PS5 build is 1.0.0 only, so it keeps its own copies.

Not run on its own: prepare_ps5.py imports it.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from machine import Failed, crt_spec
from nup_reader import NupError, NupReader, is_nup

ROOT = Path(__file__).resolve().parents[2]


MAIN_BUILD_ID = "9e244438c3d8ef1c"  # MKW 1.0.0 exefs/main, the first 8 bytes of the NSO build id at 0x40
ROMFS_FILES = 205_660  # files of the 1.0.0 RomFS (06/11/2026)
ROMFS_MARGIN = 0.95  # a few short still passes: some dumping tools skip empty files
STEPS = ("check", "exefs", "romfs", "generate", "compile", "shaders", "warm", "cleanup")
LINK_TIMEOUT = 30 * 60  # far past a slow link: only a hung linker (issue #2: "stuck" after the last object)
WARM_SECONDS = 60  # the driver cache is saved every 20 s and the pipeline keys every 2 (vk_pipeline.c, vk_pipe_keys.c)

def emit(*words: object) -> None:
    print(*words, flush=True)
    print(*words, file=sys.stderr)

def find_game(src: Path) -> tuple[Path, Path]:
    """exefs/ and romfs/ in src or up to two levels below it."""
    for depth in range(3):
        for d in [src, *src.glob("/".join(["*"] * depth))] if depth else [src]:
            if (d / "exefs/main").is_file() and (d / "romfs").is_dir():
                return d / "exefs", d / "romfs"
    raise Failed(f"no exefs/main and romfs/ in {src}")


def count_files(top: Path, enough: int) -> int:
    n = 0
    for _, _, files in os.walk(top):
        n += len(files)
        if n >= enough:
            break
    return n


def check(src: Path) -> tuple[Path, Path]:
    if is_nup(src):
        try:
            with NupReader(src) as reader:
                reader.check_mkw_100(MAIN_BUILD_ID, int(ROMFS_FILES * ROMFS_MARGIN))
        except NupError as e:
            raise Failed(str(e)) from None
        return src, src
    exefs, romfs = find_game(src)
    head = (exefs / "main").read_bytes()[:0x48]
    if head[:4] != b"NSO0":
        raise Failed(f"{exefs / 'main'} is not an NSO: decrypted exefs needed")
    got = head[0x40:0x48].hex()
    if got != MAIN_BUILD_ID:
        raise Failed(f"exefs/main is not Mario Kart World 1.0.0 (build id {got})")
    need = int(ROMFS_FILES * ROMFS_MARGIN)
    n = count_files(romfs, need)
    if n < need:
        raise Failed(f"the RomFS has {n} files, 1.0.0 has {ROMFS_FILES}: an incomplete copy")
    return exefs, romfs


# ---- copies
def copy_tree(src: Path, dst: Path, total: int) -> None:
    """src into dst, file by file, with progress; files already there with the same size are kept (resume)."""
    done = 0
    for d, _, files in os.walk(src):
        out = dst / Path(d).relative_to(src)
        out.mkdir(parents=True, exist_ok=True)
        for f in files:
            s, t = Path(d, f), out / f
            if not (t.is_file() and t.stat().st_size == s.stat().st_size):
                part = t.with_name(t.name + ".part")
                shutil.copyfile(s, part)
                part.replace(t)
            done += 1
            if done % 500 == 0 or done == total:
                emit("progress", done, total)

def run(cmd: list[str], env: dict[str, str] | None = None, progress_prefix: str = "") -> None:
    """A tool's run; its 'progress n total' lines (translate.py) pass through, the rest goes to stderr."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    tail = []
    for line in p.stdout:
        if progress_prefix and line.startswith(progress_prefix):
            emit(line.strip())
        else:
            sys.stderr.write(line)
            tail = (tail + [line.strip()])[-5:]
    if p.wait():
        raise Failed(f"{Path(cmd[1] if len(cmd) > 1 else cmd[0]).name} failed: {' / '.join(tail)}")

def sm86_spec(game: Path, work: Path, cc: list[str]) -> Path:
    """The spec translate.py decodes with: a source checkout's own build/sm86, else built once into <game>/sm86."""
    for spec in (ROOT / "build/sm86/instructions.json", game / "sm86/instructions.json"):
        if spec.is_file():
            return spec
    crt = crt_spec(game, cc)  # spec.py links a DLL too on Windows: without it, zig builds its libc there (issue #2)
    run(spec_command(game, work, cc, crt))
    return game / "sm86/instructions.json"


def spec_command(game: Path, work: Path, cc: list[str], crt: Path | None) -> list[str]:
    """spec.py's command line; the compiler goes as JSON, whose paths may have spaces (issue #29)."""
    return [sys.executable, str(ROOT / "sm86/spec/spec.py"), "--out", str(game / "sm86"), "--cache",
            str(work / "nvidia"), "--cc", json.dumps(cc), *(["--crt", str(crt)] if crt else [])]  # fmt: skip
