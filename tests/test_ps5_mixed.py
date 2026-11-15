"""Exercise an owned PS5 backing unit with synthetic AArch64 lowered to native x86."""

import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "recomp"))
from lifter import Lifter  # noqa: E402


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64 mmap")
def test_ps5_mixed_pages(tmp_path: Path) -> None:
    cc = shutil.which("clang-22") or shutil.which("clang")
    if not cc:
        pytest.skip("requires Clang for the lifted CPU headers")
    builtins = subprocess.check_output(
        [cc, "-E", "-P", "-x", "c", "-"], input="__has_builtin(__builtin_elementwise_sqrt)\n", text=True
    )
    if builtins.strip() != "1":
        pytest.skip("Clang lacks the elementwise FP builtins used by the lifted CPU headers")
    # Synthetic opcodes only. Each uses x1 as its address.
    opcodes = {
        "str_x": 0xF9000022,
        "ldr_x": 0xF9400022,
        "str_q": 0x3D800022,
        "ldr_q": 0x3DC00022,
        "st1": 0x4C00A022,
        "ld1": 0x4C40A022,
        "ldxr": 0xC85F7C22,
        "stxr": 0xC8007C22,
        "ldadd": 0xF8220023,
        "cas": 0xC8A27C23,
        "casp": 0x48227C24,
        "ldapr": 0xF8BFC022,
        "zva": 0xD50B7421,
    }
    lift = Lifter(0, simd="c")
    (tmp_path / "lifted.h").write_text(
        "\n".join(f"static void {name}(Cpu *c) {{ {lift.lift(word, 0).code} }}" for name, word in opcodes.items())
    )
    exe = tmp_path / "ps5_mixed"
    subprocess.run(
        [
            cc,
            "-std=gnu11",
            "-O2",
            "-march=x86-64-v3",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-I",
            str(ROOT / "runtime"),
            "-I",
            str(tmp_path),
            str(ROOT / "tests/ps5_mixed_host.c"),
            str(ROOT / "runtime/platform/ps5_mixed.c"),
            "-lm",
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True)
