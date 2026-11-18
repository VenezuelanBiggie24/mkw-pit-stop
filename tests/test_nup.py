"""Tests for the PS5 builder's clean-room NUP v1 reader (tools/ps5/nup_reader.py)."""

from compression import zstd
import hashlib
from pathlib import Path
import struct
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/prepare"))
sys.path.insert(0, str(ROOT / "tools/ps5"))
import nup_reader as nup  # noqa: E402
import ps5_prepare_common as prepare  # noqa: E402


def _build_nup(
    path: Path,
    packages: list[tuple[int, int, int, dict[str, bytes], dict[str, bytes]]],
    *,
    compress_names: set[str] | None = None,
    corrupt_toc_sha: bool = False,
    bad_name: bytes | None = None,
) -> None:
    """Construct a valid (or intentionally malformed) NUP v1 file for testing."""
    compress_names = compress_names or set()
    strings = bytearray()
    str_cache: dict[bytes, tuple[int, int]] = {}

    def intern(name: bytes) -> tuple[int, int]:
        if name not in str_cache:
            off = len(strings)
            strings.extend(name)
            str_cache[name] = (off, len(name))
        return str_cache[name]

    dirs: list[tuple[int, int, list[int]]] = []
    files: list[tuple[int, int, int, int, int, bytes, int]] = []
    blob = bytearray()
    extents: list[tuple[int, bytes, int, int, bytes, int]] = []

    def add_dir(name: bytes) -> int:
        off, ln = intern(name)
        idx = len(dirs)
        dirs.append((off, ln, []))
        return idx

    def add_tree(parent_idx: int, mapping: dict[str, bytes]) -> None:
        subdirs: dict[str, int] = {}
        for rel, data in sorted(mapping.items()):
            parts = rel.split("/")
            cur = parent_idx
            for part in parts[:-1]:
                key = f"{cur}/{part}"
                if key not in subdirs:
                    sub_idx = add_dir(part.encode("utf-8"))
                    dirs[cur][2].append(sub_idx)
                    subdirs[key] = sub_idx
                cur = subdirs[key]
            fname = bad_name if bad_name is not None else parts[-1].encode("utf-8")
            off, ln = intern(fname)
            sha = hashlib.sha256(data).digest()
            if len(data) == 0:
                extents.append((off, ln, 0, 0, 0, sha, 0))
            else:
                while len(blob) % 16:
                    blob.append(0)
                rel_off = len(blob)
                if parts[-1] in compress_names or rel in compress_names:
                    comp = zstd.compress(data)
                    blob.extend(comp)
                    extents.append((off, ln, len(data), rel_off, len(comp), sha, nup.CODEC_ZSTD))
                else:
                    blob.extend(data)
                    extents.append((off, ln, len(data), rel_off, len(data), sha, nup.CODEC_RAW))
            f_idx = len(extents) - 1
            files.append((off, ln, 0, 0, 0, b"", 0))
            dirs[cur][2].append(("file", f_idx, dirs[cur]))

    pkg_records = []
    for title_id, version, kind, exefs_map, romfs_map in packages:
        root_idx = add_dir(b"")
        exefs_idx = nup.NO_DIR
        romfs_idx = nup.NO_DIR
        if exefs_map:
            exefs_idx = add_dir(b"exefs")
            dirs[root_idx][2].append(exefs_idx)
            add_tree(exefs_idx, exefs_map)
        if romfs_map:
            romfs_idx = add_dir(b"romfs")
            dirs[root_idx][2].append(romfs_idx)
            add_tree(romfs_idx, romfs_map)
        pkg_records.append((title_id, root_idx, exefs_idx, romfs_idx, version, kind))

    dir_count = len(dirs)
    children: list[int] = []
    dir_packed = bytearray()
    for off, ln, ch in dirs:
        first = len(children)
        for item in ch:
            if isinstance(item, int):
                children.append(item)
            else:
                children.append(dir_count + item[1])
        dir_packed.extend(nup._DIR_S.pack(off, ln, first, len(ch), 0))

    pkg_packed = bytearray()
    for tid, r_dir, e_dir, rm_dir, ver, kind in pkg_records:
        flags = 1 if kind == nup.KIND_UPDATE and not any(p[5] == nup.KIND_BASE for p in pkg_records) else 0
        pkg_packed.extend(nup._PKG_S.pack(tid, r_dir, e_dir, rm_dir, ver, kind, flags, 0, 0, 0, 0, b"\0" * 20))

    toc_body_before_files = (
        nup.TOC_HEAD_SIZE + len(pkg_packed) + len(dir_packed) + len(extents) * nup.FILE_SIZE + len(children) * 4
    )
    toc_size = toc_body_before_files + len(strings)
    blob_offset = (nup.HEADER_SIZE + toc_size + 15) & ~15

    file_packed = bytearray()
    for off, ln, sz, rel_off, esz, sha, codec in extents:
        abs_off = 0 if sz == 0 else blob_offset + rel_off
        file_packed.extend(nup._FILE_S.pack(off, ln, sz, abs_off, esz, sha, codec, 0, 0, b"\0" * 12))

    toc_head = nup._TOC_HEAD_S.pack(
        nup.TOC_MAGIC,
        len(pkg_records),
        dir_count,
        len(extents),
        toc_body_before_files,
        len(strings),
        0,
    )
    children_packed = struct.pack(f"<{len(children)}I", *children)
    toc = bytes(toc_head + pkg_packed + dir_packed + file_packed + children_packed + strings)
    assert len(toc) == toc_size

    toc_sha = hashlib.sha256(toc).digest()
    if corrupt_toc_sha:
        toc_sha = bytes([toc_sha[0] ^ 0xFF]) + toc_sha[1:]
    file_size = blob_offset + len(blob)
    uuid = hashlib.sha256(b"test").digest()[:16]
    sb = nup._SUPER_S.pack(
        nup.MAGIC,
        nup.HEADER_SIZE,
        1,
        nup.HEADER_SIZE,
        toc_size,
        toc_sha,
        blob_offset,
        file_size,
        len(pkg_records),
        0,
        uuid,
        b"\0" * 32,
    )
    pad = b"\0" * (blob_offset - nup.HEADER_SIZE - toc_size)
    path.write_bytes(sb + toc + pad + blob)


def _fake_nso(build_id_hex: str) -> bytes:
    buf = bytearray(0x100)
    buf[0:4] = b"NSO0"
    buf[0x40:0x48] = bytes.fromhex(build_id_hex)
    return bytes(buf)


def test_nup_roundtrip_and_update_overlay(tmp_path: Path) -> None:
    p = tmp_path / "mkw.nup"
    v100 = next(v for v in prepare.PS5_VERSIONS if v.name == "1.0.0")
    n_rom = int(v100.romfs_files * prepare.ROMFS_MARGIN)
    # Build just enough romfs entries for a fast test check with custom min_romfs_files, plus test full check rejection.
    romfs_base = {"a.bin": b"base-a", "sub/b.bin": b"hello-zstd" * 10, "empty.bin": b""}
    romfs_upd = {"a.bin": b"upd-a"}
    _build_nup(
        p,
        [
            (0x0400C3F00006E000, 0x10000, nup.KIND_BASE, {"main": _fake_nso("9e244438c3d8ef1c"), "sdk": b"s"}, romfs_base),
            (0x0400C3F00006E000, 0x20000, nup.KIND_UPDATE, {"sdk": b"s2"}, romfs_upd),
        ],
        compress_names={"main", "sub/b.bin"},
    )
    with nup.NupReader(p) as r:
        assert r.check_mkw(prepare.PS5_VERSIONS, 3 / v100.romfs_files) == v100
        assert (len(r.exefs), len(r.romfs)) == (2, 3)
        assert r.read_entry(r.exefs["sdk"]) == b"s2"
        assert r.read_entry(r.romfs["a.bin"]) == b"upd-a"
        assert r.read_entry(r.romfs["sub/b.bin"]) == b"hello-zstd" * 10
        assert r.read_entry(r.romfs["empty.bin"]) == b""
        out = tmp_path / "unpacked"
        r.unpack_tree("exefs", out / "exefs")
        r.unpack_tree("romfs", out / "romfs")
    assert (out / "romfs/a.bin").read_bytes() == b"upd-a"
    assert (out / "romfs/sub/b.bin").read_bytes() == b"hello-zstd" * 10
    assert (out / "romfs/empty.bin").read_bytes() == b""
    # Default prepare.check() rejects an incomplete RomFS (< 95% of 1.0.0's 205,660) cleanly.
    with pytest.raises(prepare.Failed, match="RomFS has 3 files"):
        prepare.check(p)
    assert n_rom > 190_000


def test_nup_rejects_bad_version_toc_hash_and_traversal(tmp_path: Path) -> None:
    wrong_ver = tmp_path / "180.nup"
    _build_nup(wrong_ver, [(1, 0x80000, nup.KIND_BASE, {"main": _fake_nso("deadbeefcafebabe")}, {"x": b"1"})])
    with pytest.raises(prepare.Failed, match="not Mario Kart World 1.0.0, 1.8.0"):
        prepare.check(wrong_ver)

    bad_hash = tmp_path / "badhash.nup"
    _build_nup(bad_hash, [(1, 0, nup.KIND_BASE, {"main": _fake_nso("9e244438c3d8ef1c")}, {"x": b"1"})],
               corrupt_toc_sha=True)
    with pytest.raises(nup.NupError, match="SHA-256 mismatch"):
        nup.NupReader(bad_hash)

    trav = tmp_path / "trav.nup"
    _build_nup(trav, [(1, 0, nup.KIND_BASE, {"main": _fake_nso("9e244438c3d8ef1c")}, {"x": b"1"})],
               bad_name=b"../evil")
    with pytest.raises(nup.NupError, match="illegal path character"):
        nup.NupReader(trav)

    upd_only = tmp_path / "upd.nup"
    _build_nup(upd_only, [(1, 0x20000, nup.KIND_UPDATE, {"main": _fake_nso("9e244438c3d8ef1c")}, {"x": b"1"})])
    with pytest.raises(nup.NupError, match="no base game package"):
        nup.NupReader(upd_only)


def _dump(top: Path, build_id: str, product: int, files: int) -> None:
    (top / "exefs").mkdir(parents=True)
    (top / "exefs/main").write_bytes(_fake_nso(build_id))
    res = top / "romfs/System/Resource"
    res.mkdir(parents=True)
    (res / f"ResourceSizeTable.Product.{product}.rsizetable.zs").write_bytes(b"t")
    for i in range(files - 1):
        (top / "romfs" / f"f{i}").write_bytes(b"")


def test_console_dump_with_update_builds_180(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A console dump keeps 1.0.0 at its top and the 1.8.0 update in updates/917504 (dev PC's G:/Downloads/base): the
    PS5 build takes 1.8.0; the same folder without the update, 1.0.0; a 1.8.0 exefs over 1.0.0's RomFS is refused."""
    small = tuple(type(v)(v.name, v.number, v.main_build_id, 4, v.patches) for v in prepare.PS5_VERSIONS)
    monkeypatch.setattr(prepare, "PS5_VERSIONS", small)
    monkeypatch.setattr(prepare, "detect_id", lambda got: next(v for v in small if v.main_build_id == got))
    base = tmp_path / "base"
    _dump(base, "9e244438c3d8ef1c", 100, 4)
    assert prepare.check(base)[2].name == "1.0.0"
    _dump(base / "updates/917504", "d00e4f399e411d06", 180, 4)
    exefs, romfs, v = prepare.check(base)
    assert (v.name, exefs, romfs) == ("1.8.0", base / "updates/917504/exefs", base / "updates/917504/romfs")
    mixed = tmp_path / "mixed"
    _dump(mixed, "d00e4f399e411d06", 100, 4)
    with pytest.raises(prepare.Failed, match="1.0.0"):
        prepare.check(mixed)


def test_remove_stale(tmp_path: Path) -> None:
    src, dst = tmp_path / "src", tmp_path / "dst"
    (src / "a").mkdir(parents=True)
    (src / "a/keep").write_bytes(b"1")
    (dst / "a").mkdir(parents=True)
    (dst / "old").mkdir()
    for f in ("a/keep", "a/gone", "old/x", "a/keep.part"):
        (dst / f).write_bytes(b"1")
    prepare.remove_stale(src, dst, "1.8.0")
    assert sorted(p.relative_to(dst).as_posix() for p in dst.rglob("*")) == ["a", "a/keep"]
