"""Check that a build consumes generated addresses from the requested layout."""

import struct
import subprocess
import sys
from dataclasses import replace
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "recomp"))
import layout  # noqa: E402


def test_legacy_desktop_layout(tmp_path: Path) -> None:
    modules = [("main", 0x7000000000, 0x1000)]
    layout.write(modules, tmp_path)
    (tmp_path / "placement.json").unlink()
    assert layout.read_checked(tmp_path, layout.DEFAULT) == modules
    with pytest.raises(ValueError, match="placement.json"):
        layout.read_checked(tmp_path, "ps5-smoke")
    with pytest.raises(ValueError, match="different placement"):
        layout.check_output_profile(tmp_path, "ps5-smoke")


def test_interrupted_and_mismatched_layout(tmp_path: Path) -> None:
    layout.write([("main", 0x7000000000, 0x1000)], tmp_path)
    pending = tmp_path / layout.INCOMPLETE
    pending.touch()
    with pytest.raises(ValueError, match="incomplete generation"):
        layout.read_checked(tmp_path, layout.DEFAULT)
    layout.check_output_profile(tmp_path, layout.DEFAULT)
    pending.unlink()
    (tmp_path / "layout.h").write_text("")
    with pytest.raises(ValueError, match="layout.h does not match"):
        layout.read_checked(tmp_path, layout.DEFAULT)


def test_guest_windows_cannot_overlap(tmp_path: Path) -> None:
    placement = layout.PLACEMENTS["ps5-smoke"]
    size = placement.kernel_base - placement.module_base + 0x1000
    with pytest.raises(ValueError, match="reaches the kernel window"):
        layout.write([("main", placement.module_base, size)], tmp_path, "ps5-smoke")


def test_alias_before_stack_layout() -> None:
    placement = layout.PLACEMENTS["ps5-high144"]
    layout.validate_placement(placement)
    assert layout.first_lower_ranges(placement) == (144 * layout.GIB, 64 * layout.GIB)


@pytest.mark.parametrize("low_gib", [52, 120])
def test_stack_and_alias_cannot_overlap(low_gib: int) -> None:
    placement = layout.PLACEMENTS["ps5-high144"]
    with pytest.raises(ValueError, match="stack/heap and alias reservations overlap"):
        layout.validate_placement(replace(placement, low_base=low_gib * layout.GIB))


def test_synthetic_generation_uses_selected_layout(tmp_path: Path) -> None:
    exefs, out = tmp_path / "exefs", tmp_path / "generated"
    exefs.mkdir()
    # A synthetic NSO with a single return function seeded by its .eh_frame_hdr.
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    text = struct.pack("<IIII", 0xD65F03C0, 0x20, 0xD65F03C0, 0xD65F03C0)
    rodata = bytearray(0x54)
    rodata[:4] = b"MOD0"
    struct.pack_into("<iiiiii", rodata, 4, 0x20, 0x70, 0x70, 0x40, 0x54, -0x20)
    rodata[0x40:] = struct.pack("<BBBBIIII", 1, 3, 3, 3, 0, 1, 0, 0)
    for i, (address, segment) in enumerate(((0, text), (0x20, rodata), (0x80, bytes(16)))):
        struct.pack_into("<III", header, 0x10 + i * 0x10, len(header), address, len(segment))
        struct.pack_into("<I", header, 0x60 + i * 4, len(segment))
        header.extend(segment)
    (exefs / "main").write_bytes(header)
    command = [sys.executable, str(ROOT / "recomp/gen.py"), "--exefs", str(exefs), "--out", str(out),
               "--modules", "main", "--jobs", "1"]  # fmt: skip
    subprocess.run([*command, "--guest-layout", "ps5-smoke"], check=True)
    base = layout.PLACEMENTS["ps5-smoke"].module_base
    assert layout.read_checked(out, "ps5-smoke") == [("main", base, 0x1000)]
    assert f"f_{base:x}(" in (out / "main/main_table.c").read_text()
    mismatch = subprocess.run(command, capture_output=True, text=True, check=False)
    assert mismatch.returncode != 0
    assert "does not match the selected 'desktop' placement" in mismatch.stderr
    (out / layout.INCOMPLETE).touch()
    subprocess.run([*command, "--guest-layout", "ps5-smoke"], check=True)
    assert layout.read_checked(out, "ps5-smoke") == [("main", base, 0x1000)]
    (out / "main/stale.c").touch()
    with pytest.raises(ValueError, match="sources do not match"):
        layout.read_checked(out, "ps5-smoke")
