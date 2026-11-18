"""Compile and run the game-free PS5 guest-span tests with Linux mmap and pthreads."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.skipif(sys.platform != "linux" or sys.maxsize < 2**39, reason="requires 64-bit Linux mmap")
def test_ps5_guest_span(tmp_path: Path) -> None:
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("requires a C compiler")
    exe = tmp_path / "ps5_guest_span"
    subprocess.run(
        [
            cc,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-I",
            str(ROOT / "runtime"),
            str(ROOT / "tests/ps5/ps5_guest_span_host.c"),
            str(ROOT / "tests/ps5/ps5_guest_span_backing.c"),
            str(ROOT / "runtime/platform/ps5_guest_span.c"),
            str(ROOT / "runtime/platform/ps5_mixed.c"),
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=15)
