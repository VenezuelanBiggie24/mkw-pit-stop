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

ROOT = Path(__file__).resolve().parents[2]


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
    if (argc >= 5 && !strcmp(argv[1], "restore")) {   // after a ProsperoStore update: store root, title id, app dir
        printf("%d\\n", ps5_store_restore(argv[2], argv[3], argv[4]));
        return 0;
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


def test_ps5_recv_info_lists_game_copies(tmp_path: Path) -> None:
    """PIT1I names each game copy in the title folder and its exefs/main build id (the builder tells the player what
    the console holds: an upgrade keeps it); a main that is not an NSO has no id, a folder without a RomFS is no copy."""
    exe = _compile_helper(tmp_path)
    title = tmp_path / "PPSA99193"

    def nso(build_id: str) -> bytes:
        return b"NSO0" + bytes(0x3C) + bytes.fromhex(build_id) + bytes(8)

    (title / "exefs").mkdir(parents=True)
    (title / "exefs/main").write_bytes(nso("9e244438c3d8ef1c"))
    (title / "romfs.nup").write_bytes(b"n")
    (title / "updates/917504/exefs").mkdir(parents=True)
    (title / "updates/917504/exefs/main").write_bytes(nso("d00e4f399e411d06"))
    (title / "updates/917504/romfs").mkdir()
    (title / "updates/1/exefs").mkdir(parents=True)
    (title / "updates/1/exefs/main").write_bytes(b"x")   # no RomFS beside it: not a copy
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], stdout=subprocess.PIPE, text=True,
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
        assert info["games"] == [{"dir": "updates/917504", "build_id": "d00e4f399e411d06"},
                                 {"dir": "", "build_id": "9e244438c3d8ef1c"}]
    finally:
        proc.kill()


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_send_reports_phases(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """send_directory says what it is doing before the bytes flow (checksums, the PS5's answer), then the transfer,
    and at the end where its time went on both ends (send_ps5.timing_line)."""
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
        err = capsys.readouterr().err
        assert "send timing: 0 MiB in" in err and f"PS5 ({title}): network wait" in err and "receive buffer" in err
    finally:
        if proc.poll() is None:
            proc.kill()
    older = send_ps5.timing_line(10 << 20, 5.0, 1.0, 4.0, {"title_id": "PPSA99193"})
    assert older == "send timing: 10 MiB in 5.0 s (2.0 MiB/s); PC: disk reads 1.0 s, sending 4.0 s; PS5: this receiver does not report timing"


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


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
@pytest.mark.skipif(hasattr(os, "geteuid") and os.geteuid() == 0, reason="root deletes the read-only folder anyway")
def test_ps5_send_survives_a_folder_it_cannot_remove(tmp_path: Path) -> None:
    """A loose spirv/ the title may not delete (files an FTP upload left, 20/11/2026: "Operation not permitted") costs
    only its space: the receiver answers the removal with an error and closes the connection, the sender says so,
    connects again and finishes the send."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import send_ps5

    exe = _compile_helper(tmp_path)
    title, stage = tmp_path / "PPSA99193", tmp_path / "stage"
    (title / "spirv").mkdir(parents=True)
    (title / "spirv/old.spv").write_bytes(b"old")
    (title / "spirv").chmod(0o555)   # its file cannot be unlinked
    stage.mkdir()
    (stage / "spirv.nup").write_bytes(b"NUP" * 100)
    (stage / "eboot.bin").write_bytes(b"eboot")
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        port = int(port_file.read_text().strip())
        sent, skipped, _ = send_ps5.send_directory(f"127.0.0.1:{port}", stage)
        assert proc.wait(timeout=30) == 0   # the receiver got PIT1D over the second connection
        assert sent == 2 and skipped == 0
        assert (title / "spirv.nup").read_bytes() == b"NUP" * 100
        assert (title / "spirv/old.spv").exists()
    finally:
        (title / "spirv").chmod(0o755)
        if proc.poll() is None:
            proc.kill()


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_send_order_and_modes(tmp_path: Path) -> None:
    """Big files first, what the title starts from last, eboot.bin at the very end; the shaders go as spirv.nup, not
    the translator's folder; received files are executable, as an FTP upload leaves them (a 0644 libc.prx is the
    difference found when a title stopped starting after a send)."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import send_ps5

    exe = _compile_helper(tmp_path)
    title, stage = tmp_path / "PPSA99193", tmp_path / "stage"
    (title / "spirv").mkdir(parents=True)
    (title / "spirv/old.spv").write_bytes(b"old")
    files = {"eboot.bin": b"e", "sce_module/libc.prx": b"l", "sce_sys/param.json": b"{}", "mkw.sym": b"s",
             "spirv.nup": b"N" * 100, "spirv/a.spv": b"loose", "spirv/a.done": b"x", "romfs.nup.part": b"half"}
    for rel, data in files.items():
        (stage / rel).parent.mkdir(parents=True, exist_ok=True)
        (stage / rel).write_bytes(data)
    with open(stage / "updates.nup", "wb") as f:
        f.truncate(send_ps5.RESUME_MIN)
    names = [e[0] for e in send_ps5.collect_stage_files(stage)]
    assert names == ["updates.nup", "mkw.sym", "spirv.nup", "sce_module/libc.prx", "sce_sys/param.json", "eboot.bin"]
    (stage / "updates.nup").unlink()
    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        send_ps5.send_directory(f"127.0.0.1:{port_file.read_text().strip()}", stage)
        assert proc.wait(timeout=10) == 0
        assert (title / "sce_module/libc.prx").stat().st_mode & 0o777 == 0o777
        assert (title / "sce_module").stat().st_mode & 0o777 == 0o777
        assert (title / "spirv.nup").is_file() and not (title / "spirv").exists()   # the loose folder went
    finally:
        if proc.poll() is None:
            proc.kill()


def _store_layout(tmp_path: Path) -> tuple[Path, Path, Path, Path]:
    """A console after ProsperoStore's clean update: the zip's folder in homebrew/, the old one in previous/."""
    data = tmp_path / "data"
    store = data / "prosperostore"
    target = data / "homebrew" / "PPSA99193"
    kept = store / "previous" / "PPSA99193"
    (store / "receipts").mkdir(parents=True)
    (store / "receipts" / "PPSA99193.json").write_text(
        json.dumps({"schema": 1, "titleId": "PPSA99193", "location": str(data / "homebrew"),
                    "contentVersion": "01.002.001"}, separators=(",", ":")) + "\n", encoding="utf-8")
    for root, eboot in ((target, b"new placeholder"), (kept, b"old built game")):
        (root / "sce_sys").mkdir(parents=True)
        (root / "eboot.bin").write_bytes(eboot)
        (root / "sce_sys" / "param.json").write_text(eboot.decode(), encoding="utf-8")
    (kept / "romfs.nup").write_bytes(b"romfs")
    (kept / "spirv.nup").write_bytes(b"spirv")
    (kept / "exefs").mkdir()
    (kept / "exefs" / "main").write_bytes(b"main")
    (kept / "profile").mkdir()
    (kept / "profile" / "save.dat").write_bytes(b"save")
    (kept / "mkw.ini").write_text("fps = 60\n", encoding="utf-8")
    (kept / ".pitstop-manifest.tsv").write_text("", encoding="utf-8")
    return store, target, kept, tmp_path / "app0"


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
def test_ps5_store_restore(tmp_path: Path) -> None:
    exe = _compile_helper(tmp_path)
    store, target, kept, app0 = _store_layout(tmp_path / "c")
    app0.symlink_to(target)   # the running title's /app0, ShadowMountPlus's view of the same folder

    def restore(app: Path) -> int:
        out = subprocess.run([str(exe), "restore", str(store), "PPSA99193", str(app)],
                             check=True, capture_output=True, text=True)
        return int(out.stdout)

    assert restore(app0) == 6
    for name in ("romfs.nup", "spirv.nup", "exefs/main", "profile/save.dat", "mkw.ini", ".pitstop-manifest.tsv"):
        assert (target / name).exists() and not (kept / name).exists(), name
    # the new release's own files stay; the old ones are left in previous/
    assert (target / "eboot.bin").read_bytes() == b"new placeholder"
    assert (kept / "eboot.bin").read_bytes() == b"old built game"
    assert (kept / "sce_sys" / "param.json").exists()
    assert restore(app0) == 0   # a folder in use is never filled again

    # the release's own entries never come from the old folder, even where the new one lacks them (on the console lstat
    # missed the new eboot.bin and the old one was renamed over it)
    store, target, kept, app0 = _store_layout(tmp_path / "own")
    (kept / "LEGAL.txt").write_text("old", encoding="utf-8")
    (kept / "pitstop-runtime.json").write_text("old", encoding="utf-8")
    app0.symlink_to(target)
    assert restore(app0) == 6
    assert not (target / "LEGAL.txt").exists() and not (target / "pitstop-runtime.json").exists()
    assert (kept / "LEGAL.txt").exists() and (kept / "eboot.bin").read_bytes() == b"old built game"

    # a folder that is not the running one (installed by hand elsewhere) and a title the store did not install
    store, target, kept, app0 = _store_layout(tmp_path / "other")
    other = tmp_path / "other" / "hand"
    other.mkdir()
    (other / "eboot.bin").write_bytes(b"a hand-installed copy")
    assert restore(other) == 0 and (kept / "romfs.nup").exists()
    (store / "receipts" / "PPSA99193.json").unlink()
    app0.symlink_to(target)
    assert restore(app0) == 0 and (kept / "romfs.nup").exists()


class _FakeFtp:
    """Stands in for the console's FTP server (send_ps5.open_ftp): writes into `folder` what is stored under `seen`,
    the title folder as FTP sees it; mount.lnk names `link`. fail_after: an upload breaks after this many bytes."""

    def __init__(self, folder: Path, seen: str, link: str, fail_after: int | None = None) -> None:
        self.folder, self.seen, self.link, self.fail_after = folder, seen, link, fail_after
        self.stored: list[str] = []

    def _local(self, remote: str) -> Path:
        import ftplib

        if not remote.startswith(self.seen + "/"):
            raise ftplib.error_perm("550 no such folder")
        return self.folder / remote[len(self.seen) + 1 :]

    def retrlines(self, cmd: str, callback) -> None:
        callback(self.link)

    def delete(self, remote: str) -> None:
        import ftplib

        p = self._local(remote)
        if not p.is_file():
            raise ftplib.error_perm("550 not there")
        p.unlink()

    def mkd(self, remote: str) -> None:
        self._local(remote).mkdir(exist_ok=True)

    def storbinary(self, cmd: str, f, blocksize: int, callback) -> None:
        import ftplib

        verb, remote = cmd.split(" ", 1)
        out = self._local(remote)
        if verb == "STOR" and out.exists():
            raise ftplib.error_perm("550 exists")   # ftpsrv does not overwrite
        self.stored.append(remote)
        n = 0
        with out.open("ab" if verb == "APPE" else "wb") as o:
            while block := f.read(blocksize):
                if self.fail_after is not None and n + len(block) > self.fail_after:
                    raise ftplib.error_temp("451 transfer aborted")
                o.write(block)
                o.flush()
                n += len(block)
                callback(block)

    def quit(self) -> None:
        pass


@pytest.mark.skipif(sys.platform != "linux" or platform.machine() != "x86_64", reason="requires Linux x86-64")
@pytest.mark.parametrize("case", ["ftp", "wrong_folder", "upload_breaks", "no_ftp"])
def test_ps5_send_large_files_by_ftp(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, case: str) -> None:
    """Large files go by the console's FTP server, the receiver checks and places them (PIT1A) and records them so the
    next send skips them; a folder the receiver does not see, a broken upload or no FTP server fall back to PIT1P."""
    sys.path.insert(0, str(ROOT / "tools/ps5"))
    import ftplib

    import send_ps5

    monkeypatch.setattr(send_ps5, "FTP_MIN", 1 << 20)
    exe = _compile_helper(tmp_path)
    title, stage, work = tmp_path / "PPSA99193", tmp_path / "stage", tmp_path / "work"
    title.mkdir()
    work.mkdir()
    other = tmp_path / "other"
    other.mkdir()
    (title / "eboot.bin").write_bytes(b"the placeholder's")   # same_folder compares it through both paths
    files = {"romfs.nup": os.urandom(5 << 20), "sub/spirv.nup": os.urandom((2 << 20) + 7), "eboot.bin": b"e" * (3 << 20),
             "exefs/main": b"m" * 100}  # fmt: skip
    for rel, data in files.items():
        (stage / rel).parent.mkdir(parents=True, exist_ok=True)
        (stage / rel).write_bytes(data)
    fakes: list[_FakeFtp] = []

    def opener(host: str, port: int) -> _FakeFtp:
        if case == "no_ftp" or port != 2121:
            raise ConnectionRefusedError("no FTP server")
        folder = other if case == "wrong_folder" else title   # FTP's view: the title folder or another one
        fake = _FakeFtp(folder, str(folder), str(folder), (1 << 20) if case == "upload_breaks" else None)
        fakes.append(fake)
        return fake

    port_file = tmp_path / "port.txt"
    proc = subprocess.Popen([str(exe), "serve", str(title), str(port_file)], env=dict(os.environ, MKW_PS5_RECV_PORT="0"))
    try:
        for _ in range(200):
            if port_file.is_file() and port_file.read_text().strip():
                break
            time.sleep(0.01)
        progress = []
        sent, skipped, total = send_ps5.send_directory(
            f"127.0.0.1:{port_file.read_text().strip()}", stage, cache_path=work / "sha.tsv", ftp_opener=opener,
            on_progress=lambda bd, bt, fd, ft: progress.append((bd, bt, fd, ft)))  # fmt: skip
        assert proc.wait(timeout=10) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
    assert (sent, skipped, total) == (4, 0, sum(map(len, files.values())))
    assert progress[-1] == (total, total, 4, 4)
    for rel, data in files.items():
        assert (title / rel).read_bytes() == data, rel
    assert not list(title.rglob("*.pitstop-ftp")) and not list(title.rglob("*.pitstop.part"))
    assert not (title / ".pitstop-ftp-probe").exists() and not (work / ".pitstop-ftp-probe").exists()
    stored = [s for f in fakes for s in f.stored if ".pitstop-ftp-probe" not in s]
    if case == "ftp":
        assert sorted(stored) == [f"{title}/romfs.nup.pitstop-ftp", f"{title}/sub/spirv.nup.pitstop-ftp"]
    elif case == "upload_breaks":
        assert len(stored) == 2   # tried both, both went by PIT1P after
    else:
        assert stored == []   # wrong_folder: refused at PIT1A, before any upload
        assert not list(other.rglob("*"))
    manifest = (title / ".pitstop-manifest.tsv").read_text()
    assert all(f"{rel}\t{len(data)}\t" in manifest for rel, data in files.items())
    assert ftplib  # imported for _FakeFtp's errors
