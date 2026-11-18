"""Compile the real QueryMemory handler in desktop and checked PS5 fixture modes."""

import platform
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))
from layout import placement_defines  # noqa: E402


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64 mmap")
def test_ps5_query_memory(tmp_path: Path) -> None:
    cc = shutil.which("clang-22") or shutil.which("clang")
    if not cc:
        pytest.skip("requires Clang for the runtime CPU headers")
    builtins = subprocess.check_output(
        [cc, "-E", "-P", "-x", "c", "-"], input="__has_builtin(__builtin_elementwise_sqrt)\n", text=True
    )
    if builtins.strip() != "1":
        pytest.skip("Clang lacks the elementwise FP builtins used by the runtime CPU headers")

    (tmp_path / "guest_layout.h").write_text("#pragma once\n" + placement_defines("ps5-high144"))
    common = [
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
        *(f"-I{ROOT / 'runtime' / directory}" for directory in ("", "cpu", "core/base", "core/boot", "core/kernel")),
        str(ROOT / "tests/ps5/ps5_query_memory.c"),
        str(ROOT / "runtime/core/kernel/svc_query.c"),
        str(ROOT / "runtime/core/memory/memory.c"),
    ]
    for checked in (False, True):
        exe = tmp_path / ("checked" if checked else "desktop")
        command = common.copy()
        if checked:
            command += [
                "-DMKW_PS5_QUERY_SPAN_FIXTURE",
                "-fno-builtin-memcpy",
                "-Wl,--wrap=memcpy",
                "-pthread",
                str(ROOT / "tests/ps5/ps5_guest_span_backing.c"),
                str(ROOT / "runtime/platform/ps5_guest_span.c"),
                str(ROOT / "runtime/platform/ps5_mixed.c"),
            ]
        subprocess.run([*command, "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=15)
