"""The dump check, copies and tool runs of the PS5 builder (tools/ps5/prepare_ps5.py).

Why: prepare_ps5.py used these from tools/prepare/prepare.py, which upstream split into modules with other signatures
(several game versions, .nx packages; 17/11/2026), so the PS5 builder keeps its own copies. The versions themselves
(build ids, RomFS sizes, patch files) come from recomp/versions.py, as upstream's setup takes them.

Not run on its own: prepare_ps5.py imports it.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT / "recomp") not in sys.path:
    sys.path.insert(0, str(ROOT / "recomp"))

from machine import Failed, crt_spec  # noqa: E402
from nup_reader import NupError, NupReader, is_nup  # noqa: E402
from versions import ROMFS_PRODUCT, VERSIONS, Version, build_id_of, detect_id, romfs_mismatch  # noqa: E402

# The versions the PS5 build takes, the preferred first: a dump holding several (a console dump with its update in
# updates/<title version>/, as G:/Downloads/base on the dev PC: 1.0.0 at the top, 1.8.0 in updates/917504) builds the
# first found; to build 1.0.0 from such a dump, choose a folder without updates/ (find_games). 1.8.0 is the release's
# version (user's choice, 17/11/2026); 1.0.0 is what the port ran on until then.
PS5_VERSIONS = tuple(sorted(VERSIONS, key=lambda v: -v.number))
ROMFS_MARGIN = 0.95  # a few short still passes: some dumping tools skip empty files
STEPS = ("check", "exefs", "romfs", "generate", "compile", "shaders", "warm", "cleanup")
LINK_TIMEOUT = 30 * 60  # far past a slow link: only a hung linker (issue #2: "stuck" after the last object)
WARM_SECONDS = 60  # the driver cache is saved every 20 s and the pipeline keys every 2 (vk_pipeline.c, vk_pipe_keys.c)

EMIT_HOOKS: list = []   # each gets an event's words (prepare_ps5.py: send_ps5.StatusNotifier, the PS5's screen)


def emit(*words: object) -> None:
    # The launcher reading the pipe may be gone (closed, or replaced by an update): on Windows the write then fails with
    # EINVAL, which hid the real error behind a second traceback (18/11/2026). The build's result does not depend on it.
    for out in (sys.stdout, sys.stderr):
        try:
            print(*words, file=out, flush=True)
        except (OSError, ValueError):
            pass
    for hook in EMIT_HOOKS:
        hook(" ".join(str(w) for w in words).split())

def known_names() -> str:
    return ", ".join(v.name for v in reversed(PS5_VERSIONS))


def _subdir(d: Path, name: str) -> Path | None:
    """d's folder named name, in any case (a dump copied from Windows can say Updates/ or BASE/)."""
    try:
        return next((e for e in sorted(d.iterdir()) if e.name.lower() == name and e.is_dir()), None)
    except OSError:
        return None


def find_games(src: Path) -> list[tuple[Path, Path]]:
    """The exefs/ + romfs/ pairs where a dump keeps them: src itself, its base/, and the updates/<version>/ below
    either, the same places the title looks (ps5_recv.c add_game_tree). Other folders are not searched: a player who
    renamed updates/ to updates.old/ to build 1.0.0 got 1.8.0 from it, staged where the title never looked (23/11/2026),
    and the old two-level search never looked inside an exefs/ or romfs/ either (a glob into the RomFS's 200,000 files
    took minutes on a USB drive)."""
    tops = [src] + [b for b in (_subdir(src, "base"),) if b]
    dirs = []
    for top in tops:
        dirs.append(top)
        up = _subdir(top, "updates")
        if up:
            try:
                dirs += sorted(e for e in up.iterdir() if e.is_dir())
            except OSError:
                pass
    found = [(d / "exefs", d / "romfs") for d in dirs if (d / "exefs/main").is_file() and (d / "romfs").is_dir()]
    if not found:
        raise Failed(f"no exefs/main and romfs/ in {src}, its base/ or their updates/<version>/")
    return found


def count_files(top: Path, enough: int) -> int:
    n = 0
    for _, _, files in os.walk(top):
        n += len(files)
        if n >= enough:
            break
    return n


def romfs_products_dir(romfs: Path) -> list[str]:
    """The RomFS's ResourceSizeTable.Product.* names, as versions.romfs_mismatch reads them (one folder listed, not
    the 200,000 files)."""
    d = romfs / Path(ROMFS_PRODUCT).parent
    stem = Path(ROMFS_PRODUCT).name
    try:
        return [f"{Path(ROMFS_PRODUCT).parent.as_posix()}/{n}" for n in os.listdir(d) if n.startswith(stem)]
    except OSError:
        return []


def version_of(head: bytes, where: str) -> Version:
    try:
        got = build_id_of(head, where)
    except ValueError as e:
        raise Failed(str(e)) from None
    try:
        return detect_id(got)
    except ValueError:
        raise Failed(f"{where} is not Mario Kart World {known_names()} (build id {got})") from None


def check(src: Path) -> tuple[Path, Path, Version]:
    """The dump's exefs/, romfs/ and version (for a .nup: the package itself twice, its base with its updates over it,
    as nup_reader merges them). Several versions in a folder: the first of PS5_VERSIONS."""
    if is_nup(src):
        try:
            with NupReader(src) as reader:
                v = reader.check_mkw(PS5_VERSIONS, ROMFS_MARGIN)
        except NupError as e:
            raise Failed(str(e)) from None
        return src, src, v
    games, errors = [], []
    for exefs, romfs in find_games(src):
        try:
            games.append((version_of((exefs / "main").read_bytes()[:0x48], str(exefs / "main")), exefs, romfs))
        except Failed as e:
            errors.append(str(e))
    if not games:
        raise Failed(errors[0])
    games.sort(key=lambda g: PS5_VERSIONS.index(g[0]))
    v, exefs, romfs = games[0]
    if why := romfs_mismatch(v, romfs_products_dir(romfs)):
        raise Failed(why)
    need = int(v.romfs_files * ROMFS_MARGIN)
    n = count_files(romfs, need)
    if n < need:
        raise Failed(f"the RomFS has {n} files, {v.name} has {v.romfs_files}: an incomplete copy")
    return exefs, romfs, v


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

def remove_stale(src: Path, dst: Path, version: str) -> None:
    """dst's files that src does not have removed: a folder built from another version keeps that version's files
    otherwise (1.0.0's Product_100 tables among 1.8.0's Product_180, upstream #261; an exefs/subsdk1 under 1.0.0)."""
    if not dst.is_dir() or dst.resolve() == src.resolve():
        return
    removed = 0
    for d, _, files in os.walk(dst, topdown=False):
        for f in files:
            t = Path(d, f)
            if f.endswith(".part") or not (src / t.relative_to(dst)).exists():
                t.unlink()
                removed += 1
        if Path(d) != dst and not any(Path(d).iterdir()):
            Path(d).rmdir()
    if removed:
        print(f"copy: removed {removed} files that {version} does not have from {dst}", file=sys.stderr)


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
