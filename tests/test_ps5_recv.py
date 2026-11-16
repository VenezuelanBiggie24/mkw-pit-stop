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
