#!/usr/bin/env python3
"""Find the PS5 on the local network and say what it holds, for the launcher's PS5 setup wizard.

Why: the wizard's first question is the console. Typing its address was the step new players got wrong, and nothing
said before a 20-minute build whether the PS5 was reachable, a first install or an upgrade. This answers both in a few
seconds, from the same requests the builder makes (send_ps5.py: the receiver's PIT1I, else the console's FTP server).

  find          every address on this PC's local /24 networks with Pit Stop's receiver (19913) or an FTP server
                (2121, 1337) open, one 'found' line each: found<TAB>ip<TAB>receiver|ftp<TAB>what it is
  check HOST    what that console holds: 'kind<TAB>receiver|ftp|none', 'line<TAB>text' for the player and one
                'game<TAB>version<TAB>folder' per game copy found there

Lines only, tab separated (launcher_console.cpp reads them). Nothing is written or deleted on the console.

Usage: python3 tools/ps5/console_probe.py find | check 192.168.1.50
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import contextlib
import ipaddress
import socket
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent / "prepare", HERE.parent / "build", HERE.parents[1] / "recomp"):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

from send_ps5 import (  # noqa: E402
    DEFAULT_PORT, FTP_PORTS, TransferError, ftp_server, ftp_title_folder, query_info,
)
from versions import VERSIONS  # noqa: E402

SCAN_TIMEOUT = 0.6   # a console on the same network answers a connect in a few ms; Wi-Fi at worst ~100 ms


def version_name(build_id: str) -> str:
    v = next((v for v in VERSIONS if v.main_build_id == build_id), None)
    return v.name if v else "unknown version"


def local_networks() -> list[ipaddress.IPv4Network]:
    """This PC's private IPv4 /24 networks (a home router's), not loopback, link-local or a VM's NAT."""
    addrs: set[str] = set()
    with contextlib.suppress(OSError):
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    with contextlib.suppress(OSError), socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 9))   # no packet: only which local address routes out
        addrs.add(s.getsockname()[0])
    nets = []
    for a in sorted(addrs):
        ip = ipaddress.IPv4Address(a)
        if ip.is_private and not ip.is_loopback and not ip.is_link_local:
            net = ipaddress.IPv4Network(f"{a}/24", strict=False)
            if net not in nets:
                nets.append(net)
    return nets


def _open(ip: str, port: int) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=SCAN_TIMEOUT):
            return True
    except OSError:
        return False


def describe_receiver(info: dict) -> str:
    if info.get("has_game_code"):
        return f"Pit Stop {info.get('port_version') or ''} ready to receive".replace("  ", " ")
    return "Pit Stop placeholder, waiting for the PC"


def find() -> int:
    nets = local_networks()
    hosts = [str(h) for n in nets for h in n.hosts()]
    ports = (DEFAULT_PORT, *FTP_PORTS)
    with ThreadPoolExecutor(max_workers=192) as pool:
        hits = [(ip, port) for (ip, port), ok in zip(((h, p) for h in hosts for p in ports),
                                                    pool.map(lambda hp: _open(*hp), ((h, p) for h in hosts for p in ports)))
                if ok]  # fmt: skip
    seen: set[str] = set()
    for ip, port in sorted(hits, key=lambda x: (x[1] != DEFAULT_PORT, ipaddress.IPv4Address(x[0]))):
        if ip in seen:
            continue
        if port == DEFAULT_PORT:
            try:
                print(f"found\t{ip}\treceiver\t{describe_receiver(query_info(ip, port, timeout=3.0))}", flush=True)
            except (OSError, TransferError, ValueError):
                continue
        else:
            print(f"found\t{ip}\tftp\tFTP server on port {port} (Pit Stop not open)", flush=True)
        seen.add(ip)
    if not nets:
        print("note\tno local network found on this PC", flush=True)
    return 0


def game_copies(ftp, folder: str) -> tuple[dict[str, int], list[str]]:
    """The title folder's own files and its game copies (the folder itself, updates/<version>/), as the title looks
    (ps5_recv.c add_game_tree): a few listings, not the whole tree (radv-shader-cache/ alone made a check take 12 s
    from Windows, 23/11/2026)."""
    def files(path: str) -> dict[str, int]:
        with contextlib.suppress(Exception):
            return {n: int(f.get("size", 0)) for n, f in ftp.mlsd(path) if f.get("type") == "file"} | {
                n + "/": 0 for n, f in ftp.mlsd(path) if f.get("type") == "dir"}
        return {}

    top = files(folder)
    dirs = ["."] + [f"updates/{n.rstrip('/')}" for n in files(f"{folder}/updates") if n.endswith("/")]
    games = []
    for d in dirs:
        base = folder if d == "." else f"{folder}/{d}"
        here = top if d == "." else files(base)
        if "exefs/" in here and ("romfs.nup" in here or "romfs/" in here) and "main" in files(f"{base}/exefs"):
            games.append(d)
    return top, games


def check(host: str) -> int:
    try:
        info = query_info(host, DEFAULT_PORT, timeout=3.0)
    except (OSError, TransferError, ValueError):
        info = None
    if info is not None:
        print("kind\treceiver")
        print(f"line\t{describe_receiver(info)} at {host}.")
        games = info.get("games")
        for g in games or []:
            print(f"game\t{version_name(g.get('build_id', ''))}\t{g.get('dir') or '.'}")
        if games == [] or (games is None and not info.get("game_files_ready")):
            print("line\tIt has no game files yet: this is a first install.")
        else:
            print("line\tIts game files, saves and settings stay: only what changed is sent.")
        return 0
    found = ftp_server(host, FTP_PORTS)
    if not found:
        print("kind\tnone")
        print(f"line\tNo answer from {host}.")
        return 0
    ftp, port = found
    try:
        folder, exists = ftp_title_folder(ftp)
        listing, games = (game_copies(ftp, folder) if exists else ({}, []))
        print("kind\tftp")
        print(f"line\tPit Stop is not open on the PS5, but its FTP server answers (port {port}): the builder installs "
              "over FTP. Close Pit Stop on the PS5 before the send; nothing shows on the TV.")
        for g in games:
            bid = ""
            with contextlib.suppress(Exception):
                path = f"{folder}/{g + '/' if g != '.' else ''}exefs/main"
                conn = ftp.transfercmd(f"RETR {path}")
                head = b""
                while len(head) < 0x48:
                    chunk = conn.recv(0x48 - len(head))
                    if not chunk:
                        break
                    head += chunk
                conn.close()
                with contextlib.suppress(Exception):
                    ftp.voidresp()
                if head[:4] == b"NSO0":
                    bid = head[0x40:0x48].hex()
            print(f"game\t{version_name(bid) if bid else 'unknown version'}\t{g}")
        if "eboot.bin" in listing and games:
            print(f"line\tPit Stop is installed in {folder} with game files: only what changed is sent.")
        elif "eboot.bin" in listing:
            print(f"line\tThe Pit Stop placeholder is in {folder}, without game files: this is a first install.")
        else:
            print(f"line\tPit Stop is not installed yet: a first install into {folder}.")
    finally:
        with contextlib.suppress(Exception):
            ftp.quit()
    return 0


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "find":
        return find()
    if len(sys.argv) >= 3 and sys.argv[1] == "check":
        return check(sys.argv[2].strip())
    print(__doc__.strip().splitlines()[-1], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
