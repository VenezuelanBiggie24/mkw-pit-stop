"""Tests for PS5 Pit Stop builder & release checks (release_ps5.py, prepare_ps5.py)."""

from __future__ import annotations

import pytest
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for p in (ROOT / "tools/ps5", ROOT / "tools/prepare"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import prepare_ps5  # noqa: E402
import release_ps5  # noqa: E402


def _make_elf64_with_symtab(syms: list[tuple[str, int]], sh_flags: int = 0x4) -> bytes:
    """Build a minimal valid little-endian ELF64 binary with .shstrtab, .symtab, .strtab, and .text."""
    shstrtab = b"\x00.text\x00.symtab\x00.strtab\x00.shstrtab\x00"
    strtab = bytearray(b"\x00")
    symtab = bytearray(struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0))
    for name, val in syms:
        off = len(strtab)
        strtab.extend(name.encode() + b"\x00")
        # STB_GLOBAL=1, STT_FUNC=2 -> st_info = (1<<4)|2 = 0x12
        symtab.extend(struct.pack("<IBBHQQ", off, 0x12, 0, 1, val, 16))

    ehdr_size = 64
    text_off = ehdr_size
    text = b"\xc3" + b"\x00" * 15
    sym_off = text_off + len(text)
    str_off = sym_off + len(symtab)
    shstr_off = str_off + len(strtab)
    pad = (-shstr_off - len(shstrtab)) & 7
    shoff = shstr_off + len(shstrtab) + pad

    ehdr = struct.pack(
        "<16sHHIQQQIHHHHHH",
        b"\x7fELF\x02\x01\x01" + b"\x00" * 9,
        3,       # ET_DYN
        62,      # EM_X86_64
        1,
        0, 0, shoff, 0, ehdr_size, 0, 0, 64, 5, 4,
    )
    sh_null = struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    sh_text = struct.pack("<IIQQQQIIQQ", 1, 1, sh_flags | 0x2, 0x1000, text_off, len(text), 0, 0, 16, 0)
    sh_sym = struct.pack("<IIQQQQIIQQ", 7, 2, 0, 0, sym_off, len(symtab), 3, 1, 8, 24)
    sh_str = struct.pack("<IIQQQQIIQQ", 15, 3, 0, 0, str_off, len(strtab), 0, 0, 1, 0)
    sh_shstr = struct.pack("<IIQQQQIIQQ", 23, 3, 0, 0, shstr_off, len(shstrtab), 0, 0, 1, 0)
    return bytes(ehdr + text + symtab + strtab + shstrtab + (b"\x00" * pad) + sh_null + sh_text + sh_sym + sh_str + sh_shstr)


def test_check_no_game_ps5(tmp_path: Path) -> None:
    clean = tmp_path / "clean"
    clean.mkdir()
    (clean / "README.md").write_text("ok", encoding="utf-8")
    (clean / "sce_sys").mkdir()
    (clean / "sce_sys/param.json").write_text("{}", encoding="utf-8")
    assert release_ps5.check_no_game_ps5(clean) == []

    # Game-free placeholder eboot.bin is allowed only when allow_placeholder_eboot=True
    (clean / "eboot.bin").write_bytes(b"\x00\x01hello_world")
    assert len(release_ps5.check_no_game_ps5(clean, allow_placeholder_eboot=False)) == 1
    assert release_ps5.check_no_game_ps5(clean, allow_placeholder_eboot=True) == []

    # Even with allow_placeholder_eboot=True, lifted symbols and game markers must still be rejected!
    (clean / "eboot.bin").write_bytes(b"ELF \x00 f_70001234 \x00")
    errs = release_ps5.check_no_game_ps5(clean, allow_placeholder_eboot=True)
    assert any("lifted guest function symbols" in e for e in errs)

    bad = tmp_path / "bad"
    bad.mkdir()
    (bad / "test.spv").write_bytes(b"\x03\x02\x23\x07")
    (bad / "foo.c").write_text("/* MKW_RECOMP_" + "GENERATED_FROM_GAME_CODE */\n", encoding="utf-8")
    errs2 = release_ps5.check_no_game_ps5(bad)
    assert len(errs2) == 2


def test_elf64_symbol_and_ar_readers(tmp_path: Path) -> None:
    elf_bytes = _make_elf64_with_symtab([("host_foo", 0x1234), ("f_7000abcd", 0x2000)])
    elf_path = tmp_path / "test.elf"
    elf_path.write_bytes(elf_bytes)

    sym_out = tmp_path / "mkw.sym"
    n = prepare_ps5.write_mkw_sym(elf_path, sym_out)
    assert n == 1
    assert sym_out.read_text(encoding="utf-8") == "1234 host_foo\n"

    # Minimal `ar` archive containing one ELF object with ps5_localeconv
    obj_bytes = _make_elf64_with_symtab([("ps5_localeconv", 0x10)])
    hdr = f"member.o/       0           0     0     100644  {len(obj_bytes):<10d}`\n".encode("ascii")
    ar_path = tmp_path / "libps5platform.a"
    ar_path.write_bytes(b"!<arch>\n" + hdr + obj_bytes + (b"\n" if len(obj_bytes) & 1 else b""))
    syms = prepare_ps5.read_ar_defined(ar_path)
    assert "ps5_localeconv" in syms


def test_copy_to_usb_updates_in_place(tmp_path: Path, capsys) -> None:
    """--usb writes DRIVE/homebrew/PPSA99193 (where ShadowMountPlus looks), copies only what changed, removes another
    build's leftovers in exefs/ and spirv/, and keeps the player's mkw.ini and saves on the drive."""
    stage, drive = tmp_path / "stage", tmp_path / "drive"
    for rel, data in {"eboot.bin": "e1", "exefs/main": "m", "spirv/a.spv": "s"}.items():
        (stage / rel).parent.mkdir(parents=True, exist_ok=True)
        (stage / rel).write_text(data)
    dest = prepare_ps5.copy_to_usb(stage, drive)
    assert dest == drive / "homebrew/PPSA99193" and (dest / "eboot.bin").read_text() == "e1"
    (dest / "exefs/subsdk1").write_text("old")
    (dest / "spirv/gone.spv").write_text("old")
    (dest / "mkw.ini").write_text("user")
    (dest / "profile").mkdir()
    (dest / "profile/save.bin").write_text("save")
    (stage / "eboot.bin").write_text("e2")
    st = (stage / "eboot.bin").stat()
    os.utime(stage / "eboot.bin", ns=(st.st_atime_ns, st.st_mtime_ns + 10 * 10**9))   # a rebuild, later than the copy
    capsys.readouterr()
    prepare_ps5.copy_to_usb(stage, drive)
    out = capsys.readouterr().out
    assert "Copying 2 B" in out or "Copying 0 MiB" in out   # only eboot.bin went again
    files = sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file())
    assert files == ["eboot.bin", "exefs/main", "mkw.ini", "profile/save.bin", "spirv/a.spv"]
    assert (dest / "eboot.bin").read_text() == "e2"


def test_nup_writer_round_trip(tmp_path: Path) -> None:
    """romfs.nup (nup_writer.py) reads back file for file through nup_reader.py, which checks every SHA-256, with
    the RomFS as the last package's romfs/ and an empty exefs/ (the C reader walks both)."""
    import nup_reader
    import nup_writer

    src = tmp_path / "romfs"
    files = {"System/Resource/big.bin": os.urandom(1_000_003), "Font/a.txt": b"hi\n", "zero.bin": b"",
             "System/odd.bin": os.urandom(17)}  # fmt: skip
    for rel, data in files.items():
        (src / rel).parent.mkdir(parents=True, exist_ok=True)
        (src / rel).write_bytes(data)
    out = tmp_path / "romfs.nup"
    assert nup_writer.write_nup(src, out) == len(files)
    with nup_reader.NupReader(out) as r:
        assert r.exefs == {} and sorted(r.romfs) == sorted(files)
        for rel, data in files.items():
            assert r.read_entry(r.romfs[rel]) == data
            assert r.romfs[rel].codec == nup_reader.CODEC_RAW   # read in place on the PS5, never decoded


def test_builder_package_ships_what_prepare_imports() -> None:
    """Every tools/ps5 module the builder imports is in the package: nup_writer.py was not, and the packaged builder
    could not start (18/11/2026)."""
    import ast

    for script in ("prepare_ps5.py", "send_ps5.py", "ps5_prepare_common.py", "nup_reader.py", "nup_writer.py"):
        assert f"tools/ps5/{script}" in release_ps5.SOURCES
        for node in ast.walk(ast.parse((ROOT / "tools/ps5" / script).read_text(encoding="utf-8"))):
            names = [a.name for a in node.names] if isinstance(node, ast.Import) else [node.module] if isinstance(node, ast.ImportFrom) and node.module else []
            for name in names:
                if (ROOT / "tools/ps5" / f"{name}.py").is_file():
                    assert f"tools/ps5/{name}.py" in release_ps5.SOURCES, f"{script} imports {name}"


def test_free_space_only_from_a_receiver_that_measures_it() -> None:
    import send_ps5

    assert send_ps5.usable_free({"free_bytes": 16 << 30}) is None   # an older placeholder: /app0's made-up size
    assert send_ps5.usable_free({"free_bytes": 16 << 30, "free_real": 1}) is None   # the title's view, 18-20/11
    assert send_ps5.usable_free({"free_bytes": 200 << 30, "free_real": 1}) == 200 << 30


def test_nup_writer_keeps_only_what_is_asked(tmp_path: Path) -> None:
    import nup_reader
    import nup_writer

    src = tmp_path / "spirv"
    src.mkdir()
    for name in ("a.spv", "a.cs.spv", "a.res.json", "a.done", "a.result", "failed.tsv"):
        (src / name).write_bytes(name.encode())
    out = tmp_path / "spirv.nup"
    assert nup_writer.write_nup(src, out, keep=lambda n: n.endswith((".spv", ".res.json"))) == 3
    with nup_reader.NupReader(out) as r:
        assert sorted(r.romfs) == ["a.cs.spv", "a.res.json", "a.spv"]


def test_content_version() -> None:
    # every release before 0.2.1 shipped 01.000.000: each new one must compare higher, as ProsperoStore compares them
    assert release_ps5.content_version("0.2.1") == "01.002.001"
    assert release_ps5.content_version("0.10.0") == "01.010.000"
    assert release_ps5.content_version("1.0.0") == "02.000.000"
    assert release_ps5.content_version("0.2.1-rc1") == "01.002.001"
    assert "01.000.000" < release_ps5.content_version("0.2.1") < release_ps5.content_version("0.10.0")
    for bad in ("0.2", "ps5-0.2.1", "0.2.1000"):
        with pytest.raises(ValueError):
            release_ps5.content_version(bad)


def test_game_staged_where_the_title_looks(tmp_path: Path) -> None:
    """The game goes to the title folder or its updates/<version>/ (ps5_recv.c ps5_game_dir), never the dump's own
    path (base/, updates.old/917504: "missing exefs/main" on the console, 23/11/2026); copies an earlier build staged
    elsewhere go, so they are not sent or copied to a drive again, and the title's other files stay."""
    assert prepare_ps5.title_game_rel(tmp_path / "dump/base/exefs") == Path()
    assert prepare_ps5.title_game_rel(tmp_path / "dump/exefs") == Path()
    assert prepare_ps5.title_game_rel(tmp_path / "dump/base/Updates/917504/exefs") == Path("updates/917504")
    out = tmp_path / "PPSA99193"
    for rel in ("updates.old/917504", "base", "updates/917504", ""):
        (out / rel / "exefs").mkdir(parents=True)
        (out / rel / "exefs/main").write_bytes(b"m")
        (out / rel / "romfs.nup").write_bytes(b"n")
    (out / "spirv/exefs").mkdir(parents=True)
    (out / "eboot.bin").write_bytes(b"e")
    prepare_ps5.drop_other_games(out, out / "updates/917504")
    assert sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()) == [
        "eboot.bin", "updates/917504/exefs/main", "updates/917504/romfs.nup"]
    assert (out / "spirv/exefs").is_dir()
    prepare_ps5.drop_other_games(out, out)
    assert sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()) == ["eboot.bin"]
