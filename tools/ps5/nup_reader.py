"""Clean-room NUP (NX Package v1) reader and extractor for unencrypted Switch 1/2 dumps.

Why: players often store their decrypted Mario Kart World dump (1.0.0, or with its 1.8.0 update) as a single .nup container rather than a loose
exefs/ + romfs/ folder. Reading NUP directly (verifying exefs/main's build ID before unpacking 23 GB) lets both the
desktop setup and the PS5 builder accept .nup inputs with no console keys or external tools.

Format: implements the MIT-licensed NUP v1 specification (https://github.com/a55uka/nup, SPEC.md, Copyright (c) 2026
a55uka) from scratch using Python 3.14's built-in compression.zstd. No GPL code is copied.

Usage: python3 tools/ps5/nup_reader.py check PATH.nup
       python3 tools/ps5/nup_reader.py unpack PATH.nup --out DIR [--exefs-only]
"""

from __future__ import annotations

import argparse
from compression import zstd
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import struct
import sys
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recomp"))
from versions import VERSIONS  # noqa: E402

MAGIC = b"NUP\x01"
TOC_MAGIC = b"TOC\x01"
HEADER_SIZE = 0x80
TOC_HEAD_SIZE = 48
PKG_SIZE = 0x40
DIR_SIZE = 0x18
FILE_SIZE = 0x50
NO_DIR = 0xFFFFFFFF

KIND_BASE = 0
KIND_UPDATE = 1
KIND_MAX = 4

CODEC_RAW = 0
CODEC_ZSTD = 1

_SUPER_S = struct.Struct("<4sHHQQ32sQQII16s32s")
_TOC_HEAD_S = struct.Struct("<4sIQQQQQ")
_PKG_S = struct.Struct("<QIIIIBBHIQI20s")
_DIR_S = struct.Struct("<IIIIQ")
_FILE_S = struct.Struct("<IIQQQ32sBBH12s")

assert _SUPER_S.size == HEADER_SIZE
assert _TOC_HEAD_S.size == TOC_HEAD_SIZE
assert _PKG_S.size == PKG_SIZE
assert _DIR_S.size == DIR_SIZE
assert _FILE_S.size == FILE_SIZE

MAX_TREE_DEPTH = 128


class NupError(Exception):
    """Malformed, nonconforming, or unsupported NUP container."""


@dataclass(frozen=True)
class NupEntry:
    """Resolved file inside a NUP container (after layering base + updates)."""

    rel_path: str
    size: int
    extent_offset: int
    extent_size: int
    sha256: bytes
    codec: int


def is_nup(path: Path) -> bool:
    """True when path is a regular file with a .nup extension or NUP\\x01 magic."""
    if not path.is_file():
        return False
    if path.suffix.lower() == ".nup":
        return True
    try:
        with path.open("rb") as f:
            return f.read(4) == MAGIC
    except OSError:
        return False


def _validate_name(raw: bytes) -> str:
    if not raw:
        raise NupError("empty entry name in NUP TOC")
    if b"\x00" in raw or b"/" in raw or b"\\" in raw:
        raise NupError(f"illegal path character in NUP entry name: {raw!r}")
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        raise NupError(f"non-UTF-8 entry name in NUP TOC: {e}") from None
    if name in (".", ".."):
        raise NupError(f"illegal relative component in NUP TOC: {name!r}")
    return name


class NupReader:
    """Fail-closed NUP v1 reader: validates superblock, TOC SHA-256, bounds, and resolves exefs/romfs."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._fp = path.open("rb")
        try:
            self._parse()
        except Exception:
            self._fp.close()
            raise

    def close(self) -> None:
        self._fp.close()

    def __enter__(self) -> NupReader:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _parse(self) -> None:
        self._fp.seek(0, os.SEEK_END)
        actual_size = self._fp.tell()
        if actual_size < HEADER_SIZE:
            raise NupError(f"{self.path.name}: file smaller than 128-byte NUP superblock")
        self._fp.seek(0)
        sb = self._fp.read(HEADER_SIZE)
        (
            magic,
            header_size,
            header_version,
            toc_offset,
            toc_size,
            toc_sha256,
            blob_offset,
            file_size,
            package_count,
            flags,
            uuid,
            _reserved,
        ) = _SUPER_S.unpack(sb)
        if magic != MAGIC:
            raise NupError(f"{self.path.name}: bad NUP magic {magic!r} (expected {MAGIC!r})")
        if header_size < HEADER_SIZE or header_version < 1:
            raise NupError(f"{self.path.name}: invalid NUP header size/version ({header_size}, {header_version})")
        if toc_offset != header_size:
            raise NupError(f"{self.path.name}: toc_offset {toc_offset} != header_size {header_size}")
        if file_size != actual_size:
            raise NupError(f"{self.path.name}: superblock file_size {file_size} != actual {actual_size}")
        if toc_size < TOC_HEAD_SIZE or toc_offset + toc_size > blob_offset or blob_offset > file_size:
            raise NupError(f"{self.path.name}: invalid TOC/blob bounds")
        if blob_offset % 16 != 0:
            raise NupError(f"{self.path.name}: blob_offset {blob_offset} is not 16-byte aligned")
        if package_count < 1:
            raise NupError(f"{self.path.name}: NUP contains no packages")
        if flags != 0:
            raise NupError(f"{self.path.name}: unsupported NUP superblock flags {flags:#x}")

        self.uuid = uuid
        self.file_size = file_size
        self.blob_offset = blob_offset
        self.toc_sha256 = toc_sha256

        self._fp.seek(toc_offset)
        toc = self._fp.read(toc_size)
        if len(toc) != toc_size:
            raise NupError(f"{self.path.name}: truncated NUP TOC")
        if hashlib.sha256(toc).digest() != toc_sha256:
            raise NupError(f"{self.path.name}: NUP TOC SHA-256 mismatch (corrupt archive)")

        (
            toc_magic,
            toc_pkg_count,
            dir_count,
            file_count,
            str_off,
            str_size,
            toc_reserved,
        ) = _TOC_HEAD_S.unpack_from(toc, 0)
        if toc_magic != TOC_MAGIC:
            raise NupError(f"{self.path.name}: bad TOC magic {toc_magic!r}")
        if toc_pkg_count != package_count or toc_reserved != 0:
            raise NupError(f"{self.path.name}: inconsistent TOC header")

        pos = TOC_HEAD_SIZE
        pkg_end = pos + package_count * PKG_SIZE
        dir_end = pkg_end + dir_count * DIR_SIZE
        file_end = dir_end + file_count * FILE_SIZE
        if file_end > toc_size or str_off < file_end or str_off + str_size > toc_size:
            raise NupError(f"{self.path.name}: TOC arrays/string pool out of bounds")

        packages = []
        for i in range(package_count):
            fields = _PKG_S.unpack_from(toc, pos + i * PKG_SIZE)
            title_id, root_dir, exefs_dir, romfs_dir, version, kind, pflags, r1, r2, meta_off, meta_sz, _r3 = fields
            if kind > KIND_MAX:
                raise NupError(f"{self.path.name}: unknown package kind {kind}")
            if r1 != 0 or r2 != 0:
                raise NupError(f"{self.path.name}: nonzero reserved package field")
            packages.append(
                {
                    "index": i,
                    "title_id": title_id,
                    "root_dir": root_dir,
                    "exefs_dir": exefs_dir,
                    "romfs_dir": romfs_dir,
                    "version": version,
                    "kind": kind,
                    "flags": pflags,
                }
            )

        dirs = [_DIR_S.unpack_from(toc, pkg_end + i * DIR_SIZE) for i in range(dir_count)]
        files = [_FILE_S.unpack_from(toc, dir_end + i * FILE_SIZE) for i in range(file_count)]

        total_slots = sum(d[3] for d in dirs)
        children_bytes = total_slots * 4
        if file_end + children_bytes > str_off:
            raise NupError(f"{self.path.name}: TOC children array overlaps string pool")
        children = struct.unpack_from(f"<{total_slots}I", toc, file_end)
        strings = memoryview(toc)[str_off : str_off + str_size]

        def get_str(off: int, ln: int) -> str:
            if off + ln > str_size:
                raise NupError(f"{self.path.name}: string out of pool bounds")
            return _validate_name(bytes(strings[off : off + ln]))

        def collect_tree(dir_idx: int, prefix: str, out: dict[str, NupEntry], seen: set[int], depth: int) -> None:
            if depth > MAX_TREE_DEPTH:
                raise NupError(f"{self.path.name}: directory depth limit exceeded")
            if dir_idx >= dir_count:
                raise NupError(f"{self.path.name}: directory index {dir_idx} out of range")
            if dir_idx in seen:
                raise NupError(f"{self.path.name}: cycle detected in directory tree")
            seen.add(dir_idx)
            _, _, first_child, child_count, d_res = dirs[dir_idx]
            if d_res != 0 or first_child + child_count > total_slots:
                raise NupError(f"{self.path.name}: invalid directory child range")
            for slot in range(first_child, first_child + child_count):
                nid = children[slot]
                if nid < dir_count:
                    noff, nlen, _, _, _ = dirs[nid]
                    sub = get_str(noff, nlen)
                    collect_tree(nid, f"{prefix}/{sub}" if prefix else sub, out, seen, depth + 1)
                else:
                    fid = nid - dir_count
                    if fid >= file_count:
                        raise NupError(f"{self.path.name}: file node {nid} out of range")
                    noff, nlen, sz, eoff, esz, fsha, codec, cflags, r2, _r3 = files[fid]
                    if codec not in (CODEC_RAW, CODEC_ZSTD) or cflags != 0 or r2 != 0:
                        raise NupError(f"{self.path.name}: unsupported file codec/flags ({codec}, {cflags})")
                    if sz == 0 and (eoff != 0 or esz != 0 or codec != CODEC_RAW):
                        raise NupError(f"{self.path.name}: non-empty extent on zero-byte file")
                    if sz != 0 and (esz == 0 or eoff < blob_offset or eoff + esz > file_size or eoff % 16 != 0):
                        raise NupError(f"{self.path.name}: invalid blob extent ({eoff}, {esz})")
                    fname = get_str(noff, nlen)
                    rel = f"{prefix}/{fname}" if prefix else fname
                    out[rel] = NupEntry(rel, sz, eoff, esz, fsha, codec)
            seen.remove(dir_idx)

        bases = [p for p in packages if p["kind"] == KIND_BASE]
        if len(bases) > 1:
            raise NupError(f"{self.path.name}: multiple base packages in NUP are not supported")
        updates = sorted((p for p in packages if p["kind"] == KIND_UPDATE), key=lambda p: (p["version"], p["index"]))
        # A .nup with no base package is still taken when its update holds the whole game: an installed game's update
        # is dumped with the full patched exefs and RomFS, as a console dump's updates/917504 folder is (1.8.0 with
        # 205k RomFS files), and players' "base + 1.8.0" .nup files that the PC setup builds from (tools/prepare/nup.py
        # reads the newest package, whatever its kind) stopped here with "no base game package" (r/PS5_Jailbreak,
        # 23/11/2026). check_mkw's build id and RomFS file count still refuse a real update-only patch.
        if not bases and not updates:
            raise NupError(f"{self.path.name}: no base game or update package in NUP")
        self.has_base = bool(bases)

        self.exefs: dict[str, NupEntry] = {}
        self.romfs: dict[str, NupEntry] = {}
        for pkg in [*bases, *updates]:
            if pkg["exefs_dir"] != NO_DIR:
                collect_tree(pkg["exefs_dir"], "", self.exefs, set(), 0)
            if pkg["romfs_dir"] != NO_DIR:
                collect_tree(pkg["romfs_dir"], "", self.romfs, set(), 0)
        self.packages = packages
        self.has_updates = bool(updates)

    def read_entry(self, entry: NupEntry) -> bytes:
        """Read, decompress (if zstd), and SHA-256-verify a single NUP file entry into memory."""
        if entry.size == 0:
            raw = b""
        else:
            self._fp.seek(entry.extent_offset)
            stored = self._fp.read(entry.extent_size)
            if len(stored) != entry.extent_size:
                raise NupError(f"{self.path.name}: truncated extent for {entry.rel_path}")
            if entry.codec == CODEC_RAW:
                raw = stored
            else:
                try:
                    raw = zstd.decompress(stored)
                except Exception as e:
                    raise NupError(f"{self.path.name}: zstd decode failed for {entry.rel_path}: {e}") from None
            if len(raw) != entry.size:
                raise NupError(f"{self.path.name}: size mismatch for {entry.rel_path} ({len(raw)} != {entry.size})")
        if hashlib.sha256(raw).digest() != entry.sha256:
            raise NupError(f"{self.path.name}: SHA-256 mismatch for {entry.rel_path}")
        return raw

    def unpack_entry(self, entry: NupEntry, dst: Path) -> None:
        """Stream-extract (or copy) a single NUP entry to dst, verifying uncompressed SHA-256."""
        dst.parent.mkdir(parents=True, exist_ok=True)
        part = dst.with_name(dst.name + ".part")
        h = hashlib.sha256()
        written = 0
        if entry.size == 0:
            part.write_bytes(b"")
        elif entry.codec == CODEC_RAW:
            self._fp.seek(entry.extent_offset)
            rem = entry.extent_size
            with part.open("wb") as out:
                while rem > 0:
                    chunk = self._fp.read(min(1 << 20, rem))
                    if not chunk:
                        raise NupError(f"{self.path.name}: truncated raw extent for {entry.rel_path}")
                    h.update(chunk)
                    out.write(chunk)
                    rem -= len(chunk)
                    written += len(chunk)
        else:
            dec = zstd.ZstdDecompressor()
            self._fp.seek(entry.extent_offset)
            rem = entry.extent_size
            with part.open("wb") as out:
                while rem > 0:
                    comp = self._fp.read(min(1 << 20, rem))
                    if not comp:
                        raise NupError(f"{self.path.name}: truncated zstd extent for {entry.rel_path}")
                    rem -= len(comp)
                    chunk = dec.decompress(comp)
                    if chunk:
                        h.update(chunk)
                        out.write(chunk)
                        written += len(chunk)
                if not dec.eof:
                    raise NupError(f"{self.path.name}: incomplete zstd stream for {entry.rel_path}")
        if written != entry.size:
            part.unlink(missing_ok=True)
            raise NupError(f"{self.path.name}: unpacked size mismatch for {entry.rel_path} ({written} != {entry.size})")
        if h.digest() != entry.sha256:
            part.unlink(missing_ok=True)
            raise NupError(f"{self.path.name}: unpacked SHA-256 mismatch for {entry.rel_path}")
        part.replace(dst)

    def check_mkw(self, versions, margin: float):
        """The version of exefs/main (with the updates merged over the base, the newest update's) among versions
        (recomp/versions.py rows), checked against that version's RomFS size, before unpacking."""
        if "main" not in self.exefs:
            raise NupError(f"{self.path.name}: missing exefs/main")
        main_bytes = self.read_entry(self.exefs["main"])
        if main_bytes[:4] != b"NSO0":
            raise NupError(f"{self.path.name}: exefs/main is not an NSO0 (decrypted dump required)")
        got = main_bytes[0x40:0x48].hex()
        v = next((v for v in versions if v.main_build_id == got), None)
        if v is None:
            names = ", ".join(v.name for v in sorted(versions, key=lambda v: v.number))
            raise NupError(f"{self.path.name}: exefs/main is not Mario Kart World {names} (build id {got})")
        n_romfs = len(self.romfs)
        if n_romfs < int(v.romfs_files * margin):
            if not self.has_base:
                raise NupError(
                    f"{self.path.name}: holds only an update, no base game: its RomFS has {n_romfs} files, {v.name} has "
                    f"~{v.romfs_files} (use a .nup or dump folder with the base game, or with the update merged in)"
                )
            raise NupError(f"{self.path.name}: RomFS has {n_romfs} files, {v.name} has ~{v.romfs_files} (incomplete copy)")
        return v

    def unpack_tree(
        self,
        which: str,
        dst_dir: Path,
        progress_cb: Callable[[int, int], None] | None = None,
        stamp_file: Path | None = None,
    ) -> int:
        """Unpack 'exefs' or 'romfs' into dst_dir, skipping files already present with matching size (or stamp)."""
        tree = self.exefs if which == "exefs" else self.romfs
        stamp_key = f"{self.toc_sha256.hex()}:{which}:{len(tree)}"
        if stamp_file and stamp_file.is_file() and stamp_file.read_text(encoding="utf-8").strip() == stamp_key:
            return len(tree)
        dst_dir.mkdir(parents=True, exist_ok=True)
        total = len(tree)
        done = 0
        for rel, entry in tree.items():
            out_path = dst_dir / rel
            if not (out_path.is_file() and out_path.stat().st_size == entry.size):
                self.unpack_entry(entry, out_path)
            done += 1
            if progress_cb and (done % 500 == 0 or done == total):
                progress_cb(done, total)
        if stamp_file:
            stamp_file.parent.mkdir(parents=True, exist_ok=True)
            stamp_file.write_text(stamp_key + "\n", encoding="utf-8")
        return total


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_chk = sub.add_parser("check", help="validate a .nup container and print its MKW version")
    p_chk.add_argument("nup", type=Path)
    p_unp = sub.add_parser("unpack", help="unpack exefs/ and romfs/ from a .nup container")
    p_unp.add_argument("nup", type=Path)
    p_unp.add_argument("--out", type=Path, required=True)
    p_unp.add_argument("--exefs-only", action="store_true")
    args = ap.parse_args()
    try:
        with NupReader(args.nup) as reader:
            v = reader.check_mkw(VERSIONS, 0.95)
            n_exe, n_rom = len(reader.exefs), len(reader.romfs)
            if args.cmd == "check":
                print(f"OK: {args.nup.name}: MKW {v.name}, {n_exe} exefs files, {n_rom} romfs files, uuid={reader.uuid.hex()}")
                return 0
            reader.unpack_tree("exefs", args.out / "exefs")
            if not args.exefs_only:
                reader.unpack_tree("romfs", args.out / "romfs")
            print(f"Unpacked {args.nup.name} -> {args.out}")
            return 0
    except NupError as e:
        print(f"nup: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
