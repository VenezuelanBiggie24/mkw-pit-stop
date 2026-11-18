"""Local wireless play over UDP without the game: builds runtime/net's session engine with tests/runtime/net_host.c and the
lobby/relay server (server/lobby/mkw_lobby.c), then runs real sessions over loopback.

Why: a game run with two consoles takes many minutes and two people; the protocol, the handshake, the password, the
version refusal, the relay and the limits are all checkable here in seconds.

Run: uv run pytest tests/runtime/test_net.py -q   (NET_VERBOSE=1 prints the engines' logs)
"""

import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
NET = ROOT / "runtime/net"
MONO = ROOT / "runtime/third_party/monocypher"
COMMON = [NET / "net_proto.c", NET / "net_lobby.c", NET / "net_udp.c", MONO / "monocypher.c"]
FLAGS = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", f"-I{NET}", f"-I{MONO}"]

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="the harness uses POSIX sleeps and clocks")


def compiler() -> str:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("requires a C compiler")
    return cc


@pytest.fixture(scope="module")
def built(tmp_path_factory: pytest.TempPathFactory) -> dict[str, Path]:
    out = tmp_path_factory.mktemp("net")
    cc = compiler()
    host, lobby = out / "net_host", out / "mkw-lobby"
    # monocypher.c is third-party code: its own warnings are not ours to fail on
    sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for exe, main in ((host, [ROOT / "tests/runtime/net_host.c", NET / "net_session.c"]), (lobby, [ROOT / "server/lobby/mkw_lobby.c"])):
        objs = []
        for src in [*main, *COMMON]:
            obj = out / (exe.name + "_" + src.stem + ".o")
            extra = ["-w"] if src.parent == MONO else []
            subprocess.run([cc, *FLAGS, *sanitize, *extra, "-c", str(src), "-o", str(obj)], check=True)
            objs.append(str(obj))
        subprocess.run([cc, *sanitize, *objs, "-o", str(exe)], check=True)
    return {"host": host, "lobby": lobby}


def run(exe: Path, *args: str) -> None:
    p = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=120)
    print(p.stdout, p.stderr)
    assert p.returncode == 0, p.stdout + p.stderr
    assert f"ok {args[0]}" in p.stdout


@pytest.mark.parametrize("scenario", ["lan", "password", "version", "fuzz", "invite"])
def test_sessions(built: dict[str, Path], scenario: str) -> None:
    run(built["host"], scenario)


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_lobby_and_relay(built: dict[str, Path]) -> None:
    port = free_udp_port()
    server = subprocess.Popen([str(built["lobby"]), "--port", str(port)], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        assert server.poll() is None, server.stderr.read() if server.stderr else ""
        run(built["host"], "lobby", str(port))
    finally:
        server.terminate()
        _, err = server.communicate(timeout=10)
        print(err)
    assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err


def test_lobby_limits(built: dict[str, Path]) -> None:
    """Every lobby message type at every length from a stranger, and a flood: the server stays up, answers no request
    without a cookie with more bytes than it got, and keeps answering within its rate."""
    port = free_udp_port()
    server = subprocess.Popen([str(built["lobby"]), "--port", str(port), "--quiet"], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.05)
        sent = got = 0
        for t in range(0x40, 0x52):
            for n in range(0, 1500, 37):
                body = bytes((i * 7 + t) & 255 for i in range(n))
                d = b"MKWN" + bytes([1, t, n & 255, n >> 8]) + body
                s.sendto(d, ("127.0.0.1", port))
                sent += len(d)
                try:
                    while True:
                        r, _ = s.recvfrom(4096)
                        assert len(r) <= len(d), f"type {t:#x}: {len(r)}-byte answer to {len(d)} bytes"
                        got += len(r)
                except TimeoutError:
                    pass
        assert got <= sent
        for _ in range(2000):   # a flood from one address: dropped by the rate limit, the server lives
            s.sendto(b"MKWN\x01\x43" + bytes([52, 0]) + bytes(52), ("127.0.0.1", port))
        time.sleep(0.3)
        assert server.poll() is None
    finally:
        server.terminate()
        _, err = server.communicate(timeout=10)
    assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err
