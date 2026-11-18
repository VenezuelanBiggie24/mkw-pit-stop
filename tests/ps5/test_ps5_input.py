"""Build the PS5 input adapter with MKW's real binding code and run host vectors."""

import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))
from layout import placement_defines  # noqa: E402


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_input(tmp_path: Path) -> None:
    cc = shutil.which("clang-22") or shutil.which("clang")
    if not cc:
        pytest.skip("requires Clang for the runtime CPU headers")
    builtins = subprocess.check_output(
        [cc, "-E", "-P", "-x", "c", "-"], input="__has_builtin(__builtin_elementwise_sqrt)\n", text=True
    )
    if builtins.strip() != "1":
        pytest.skip("Clang lacks the elementwise FP builtins used by the runtime CPU headers")

    (tmp_path / "guest_layout.h").write_text("#pragma once\n" + placement_defines("ps5-high144"))
    exe = tmp_path / "ps5_input"
    subprocess.run(
        [
            cc,
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-ffunction-sections",
            "-fdata-sections",
            "-Wl,--gc-sections",
            f"-I{tmp_path}",
            *(f"-I{ROOT / 'runtime' / directory}" for directory in ("", "cpu", "core/base", "input")),
            str(ROOT / "tests/ps5/ps5_input_host.c"),
            str(ROOT / "runtime/input/ps5_input.c"),
            str(ROOT / "runtime/input/input_map.c"),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=15)
