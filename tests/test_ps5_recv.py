"""Host tests for runtime/platform/ps5_recv.c: path rejection, SHA-256 verification, and incremental receive."""

import hashlib
import json
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _compile_helper(tmp_path: Path) -> Path:
    exe = tmp_path / "ps5_recv_test"
    src = tmp_path / "harness.c"
    src.write_text(
        """
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ps5_recv.h"

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "paths")) {
        const char *bad[] = {
            "", "/", "/eboot.bin", "eboot.bin/", "..", "../x", "a/../b", "a/../../b",
            ".", "./a", "a/./b", "a//b", "a\\\\b", "a\\nb", "a\\rb",
            ".pitstop-manifest.tsv", "exefs/main.pitstop.part",
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            if (ps5_recv_validate_path(bad[i])) {
                fprintf(stderr, "accepted bad path: %s\\n", bad[i]);
                return 1;
            }
        }
        const char *good[] = {"eboot.bin", "exefs/main", "romfs/System/a.bin", "spirv/0123.spv", "mkw.sym"};
        for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
            if (!ps5_recv_validate_path(good[i])) {
                fprintf(stderr, "rejected good path: %s\\n", good[i]);
                return 1;
            }
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "serve")) {
        const char *root = argv[2];
        const char *port_file = argv[3];
        if (!ps5_recv_start(root, 0, "0.1.0-test", "deadbeef01234567", 0)) return 2;
        Ps5RecvStatus st;
        ps5_recv_status(&st);
        FILE *f = fopen(port_file, "w");
        fprintf(f, "%u\\n", (unsigned)st.port);
        fclose(f);
        for (int i = 0; i < 600; i++) {
            ps5_recv_status(&st);
            if (st.state == PS5_RECV_DONE) break;
            usleep(10000);
        }
        ps5_recv_stop();
        return st.state == PS5_RECV_DONE ? 0 : 3;
    }
    if (argc >= 4 && !strcmp(argv[1], "status")) {   // the PC builder's step reports (PIT1T) until its failure
        if (!ps5_recv_start(argv[2], 0, "0.1.0-test", "deadbeef01234567", 0)) return 2;
        Ps5RecvStatus st;
        ps5_recv_status(&st);
        FILE *f = fopen(argv[3], "w");
        fprintf(f, "%u\\n", (unsigned)st.port);
        fclose(f);
        for (int i = 0; i < 1000 && !st.pc_failed; i++) {
            usleep(10000);
            ps5_recv_status(&st);
        }
        printf("%u %u %u %u %u %d|%s\\n", st.pc_index, st.pc_count, st.pc_failed, st.pc_progress, st.pc_total, st.state,
               st.pc_label);
        ps5_recv_stop();
        return st.pc_failed ? 0 : 3;
    }
    return 9;
}
""",
        encoding="utf-8",
    )
    subprocess.run(
        [
            "clang",
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'runtime/platform'}",
            f"-I{ROOT / 'runtime/core/base'}",
            str(src),
            str(ROOT / "runtime/platform/ps5_recv.c"),
            str(ROOT / "runtime/core/base/sha256.c"),
            "-lpthread",
            "-o",
            str(exe),
        ],
        check=True,
    )
    return exe


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("unexpected EOF")
        buf.extend(chunk)
    return bytes(buf)


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_recv_paths_checksum_and_incremental(tmp_path: Path) -> None:
    exe = _compile_helper(tmp_path)
    subprocess.run([str(exe), "paths"], check=True, timeout=10)

    title = tmp_path / "PPSA99193"
    title.mkdir()
    port_file = tmp_path / "port.txt"
    env = dict(os.environ, MKW_PS5_RECV_PORT="0")
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=env)
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            assert proc.poll() is None
            time.sleep(0.01)
        port = int(port_file.read_text().strip())

        # 1. Path traversal attempt over wire must fail and write nothing outside title/.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"PIT1I")
            info_len = struct.unpack("<I", _recv_exact(s, 4))[0]
            info = json.loads(_recv_exact(s, info_len).decode("utf-8"))
            assert info["title_id"] == "PPSA99193"
            assert info["has_game_code"] == 0

            bad_rel = b"../escaped.bin"
            s.sendall(b"PIT1P" + struct.pack("<BH", 1, len(bad_rel)) + bad_rel + struct.pack("<Q", 3) + b"\0" * 32 + b"abc")
            resp = _recv_exact(s, 3)
            assert resp[0:1] == b"E"
            err_len = struct.unpack("<H", resp[1:3])[0]
            assert b"outside title folder" in _recv_exact(s, err_len)
        assert not (tmp_path / "escaped.bin").exists()

        # 2. Bad SHA-256 must fail and leave no partial/target file.
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            rel = b"exefs/main"
            s.sendall(b"PIT1P" + struct.pack("<BH", 1, len(rel)) + rel + struct.pack("<Q", 4) + b"\0" * 32 + b"NSO0")
            resp = _recv_exact(s, 3)
            assert resp[0:1] == b"E"
            err_len = struct.unpack("<H", resp[1:3])[0]
            assert b"SHA-256" in _recv_exact(s, err_len)
        assert not (title / "exefs/main").exists()

        # 3. Valid transfer + DONE closes the listener.
        files = {
            "eboot.bin": b"fake-eboot-v1",
            "exefs/main": b"NSO0fake",
            "romfs/a.bin": b"romfs-bytes",
            "spirv/0001.spv": b"spv1",
        }
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"PIT1B" + struct.pack("<QQ", len(files), sum(len(v) for v in files.values())))
            assert _recv_exact(s, 1) == b"K"
            for rel_s, data in files.items():
                rel = rel_s.encode("utf-8")
                sha = hashlib.sha256(data).digest()
                s.sendall(b"PIT1P" + struct.pack("<BH", 1, len(rel)) + rel + struct.pack("<Q", len(data)) + sha + data)
                assert _recv_exact(s, 1) == b"K"
            s.sendall(b"PIT1D")
            assert _recv_exact(s, 1) == b"K"

        assert proc.wait(timeout=5) == 0
        for rel_s, data in files.items():
            assert (title / rel_s).read_bytes() == data
        assert (title / ".pitstop-manifest.tsv").is_file()

        # 4. Restarting receiver and running STAT_BATCH skips unchanged files and asks only for changed eboot.bin.
        port_file.unlink()
        proc2 = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=env)
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            assert proc2.poll() is None
            time.sleep(0.01)
        port2 = int(port_file.read_text().strip())
        files2 = dict(files, **{"eboot.bin": b"fake-eboot-v2"})
        with socket.create_connection(("127.0.0.1", port2), timeout=5) as s:
            items = list(files2.items())
            pkt = bytearray(b"PIT1S" + struct.pack("<I", len(items)))
            for rel_s, data in items:
                rel = rel_s.encode("utf-8")
                pkt.extend(struct.pack("<H", len(rel)) + rel + struct.pack("<Q", len(data)) + hashlib.sha256(data).digest())
            s.sendall(pkt)
            ans = list(_recv_exact(s, len(items)))
            assert ans == [0, 1, 1, 1]
            # Send only eboot.bin
            rel = b"eboot.bin"
            data = files2["eboot.bin"]
            s.sendall(b"PIT1B" + struct.pack("<QQ", 1, len(data)))
            assert _recv_exact(s, 1) == b"K"
            s.sendall(b"PIT1P" + struct.pack("<BH", 1, len(rel)) + rel + struct.pack("<Q", len(data)) + hashlib.sha256(data).digest() + data)
            assert _recv_exact(s, 1) == b"K"
            s.sendall(b"PIT1D")
            assert _recv_exact(s, 1) == b"K"
        assert proc2.wait(timeout=5) == 0
        assert (title / "eboot.bin").read_bytes() == b"fake-eboot-v2"
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_recv_builder_status(tmp_path: Path) -> None:
    """The PC builder's steps reach the placeholder (send_ps5.StatusNotifier -> PIT1T), its failure last, and the
    receiver announces it takes them."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import send_ps5

    exe = _compile_helper(tmp_path)
    title = tmp_path / "PPSA99193"
    title.mkdir()
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "status", str(title), str(port_file)], stdout=subprocess.PIPE, text=True,
                            env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        port = int(port_file.read_text().strip())
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"PIT1I")
            info = json.loads(_recv_exact(s, struct.unpack("<I", _recv_exact(s, 4))[0]).decode("utf-8"))
            assert info["status"] == 1
        notifier = send_ps5.StatusNotifier(f"127.0.0.1:{port}")
        for line in ("step check", "step shaders", "progress 5 10", "error failed spec.py failed: unparsed\nline"):
            notifier.event(line.split())
        notifier.flush()
        out, _ = proc.communicate(timeout=15)
        assert proc.returncode == 0, out
        nums, label = out.strip().split("|", 1)
        assert nums.split() == ["8", "9", "1", "5", "10", "1"]   # shaders of 9, failed, 5/10, still waiting
        assert label == "spec.py failed: unparsed line"
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_send_reports_phases(tmp_path: Path) -> None:
    """send_directory says what it is doing before the bytes flow (checksums, the PS5's answer), then the transfer."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import send_ps5

    exe = _compile_helper(tmp_path)
    title, stage = tmp_path / "PPSA99193", tmp_path / "stage"
    title.mkdir()
    for rel, data in {"eboot.bin": b"e" * 10, "exefs/main": b"m" * 20, "romfs/a.bin": b"r" * 30}.items():
        (stage / rel).parent.mkdir(parents=True, exist_ok=True)
        (stage / rel).write_bytes(data)
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        phases, progress = [], []
        send_ps5.send_directory(f"127.0.0.1:{port_file.read_text().strip()}", stage,
                                on_phase=lambda p, d, t: phases.append((p, d, t)),
                                on_progress=lambda bd, bt, fd, ft: progress.append((bd, bt, fd, ft)))
        assert proc.wait(timeout=5) == 0
        assert ("hash", 3, 3) in phases and ("check", 3, 3) in phases and ("send", 0, 60) in phases
        assert [p[0] for p in phases].index("send") == len(phases) - 1
        assert progress[-1] == (60, 60, 3, 3)
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_recv_resumes_large_file(tmp_path: Path) -> None:
    """A large file whose send broke goes on from where the receiver's .part ends (romfs.nup is one 23 GB file)."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import send_ps5

    exe = _compile_helper(tmp_path)
    title, stage = tmp_path / "PPSA99193", tmp_path / "stage"
    title.mkdir()
    stage.mkdir()
    big = os.urandom(send_ps5.RESUME_MIN + (6 << 20))
    (stage / "romfs.nup").write_bytes(big)
    (stage / "eboot.bin").write_bytes(b"eboot")
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        port = int(port_file.read_text().strip())
        with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
            s.sendall(b"PIT1I")
            info = json.loads(_recv_exact(s, struct.unpack("<I", _recv_exact(s, 4))[0]).decode("utf-8"))
            assert info["resume"] == 1
            rel = b"romfs.nup"
            s.sendall(b"PIT1P" + struct.pack("<BH", 3, len(rel)) + rel + struct.pack("<Q", len(big))
                      + hashlib.sha256(big).digest())
            assert struct.unpack("<Q", _recv_exact(s, 8))[0] == 0
            s.sendall(big[: 30 << 20])   # then the connection breaks
        time.sleep(0.3)
        assert (title / "romfs.nup.pitstop.part").stat().st_size == 30 << 20
        progress = []
        send_ps5.send_directory(f"127.0.0.1:{port}", stage, on_progress=lambda bd, bt, fd, ft: progress.append(bd))
        assert proc.wait(timeout=10) == 0
        assert progress[0] >= 30 << 20   # it went on from the part, not from the start
        assert (title / "romfs.nup").read_bytes() == big
        assert not (title / "romfs.nup.pitstop.part").exists() and not (title / "romfs.nup.pitstop.part.want").exists()
    finally:
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_recv_nup_replaces_loose_romfs(tmp_path: Path) -> None:
    """After romfs.nup arrives, the loose romfs/ an earlier send left beside it is removed (PIT1X); a path outside
    the title folder is refused; the manifest (now hash-indexed) still answers a 20,000-file batch right."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import send_ps5

    exe = _compile_helper(tmp_path)
    title, stage = tmp_path / "PPSA99193", tmp_path / "stage"
    (title / "updates/917504/romfs/Sub").mkdir(parents=True)
    (title / "updates/917504/romfs/Sub/old.bin").write_bytes(b"old")
    (tmp_path / "outside").mkdir()
    (stage / "updates/917504").mkdir(parents=True)
    (stage / "updates/917504/romfs.nup").write_bytes(b"NUP" * 100)
    (stage / "eboot.bin").write_bytes(b"eboot")
    for i in range(20000):
        (stage / "spirv").mkdir(exist_ok=True)
        (stage / f"spirv/{i:05d}.spv").write_bytes(b"%d" % i)
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        port = int(port_file.read_text().strip())
        with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
            bad = b"../outside"
            s.sendall(b"PIT1X" + struct.pack("<H", len(bad)) + bad)
            assert _recv_exact(s, 1) == b"E"
        assert (tmp_path / "outside").is_dir()
        t0 = time.monotonic()
        sent, skipped, _ = send_ps5.send_directory(f"127.0.0.1:{port}", stage)
        assert proc.wait(timeout=30) == 0
        assert sent == 20002 and skipped == 0
        assert not (title / "updates/917504/romfs").exists()
        assert (title / "updates/917504/romfs.nup").read_bytes() == b"NUP" * 100
        assert time.monotonic() - t0 < 60
    finally:
        if proc.poll() is None:
            proc.kill()
