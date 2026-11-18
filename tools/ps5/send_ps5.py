"""Send a staged PPSA99193 package directory to the Pit Stop placeholder TCP receiver (PIT1).

Why: FTP requires running a separate FTP server on the PS5 and issuing hundreds of thousands of per-file commands for
23 GB of RomFS. The Pit Stop placeholder screen listens on port 19913 only while game files are missing/outdated,
checks batches of files against its persistent SHA-256 manifest, streams missing/changed files over one TCP connection
with SHA-256 verification and atomic renames, and closes the listener immediately on completion.

Usage:
    python3 tools/ps5/send_ps5.py --host 192.168.1.50 --stage build/ps5-work/stage/PPSA99193
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time
from pathlib import Path
from typing import Callable

DEFAULT_PORT = 19913
CHUNK_SIZE = 256 * 1024
STAT_BATCH = 4096
ACK_EVERY_FILES = 128
ACK_SIZE_THRESHOLD = 256 * 1024
CACHE_NAME = ".pitstop-client-sha256.tsv"


class TransferError(RuntimeError):
    """Raised when the PS5 receiver rejects a request or the connection fails."""


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise TransferError("PS5 closed the connection unexpectedly")
        buf.extend(chunk)
    return bytes(buf)


def _expect_ack(sock: socket.socket) -> None:
    tag = _recv_exact(sock, 1)
    if tag == b"K":
        return
    if tag == b"E":
        (mlen,) = struct.unpack("<H", _recv_exact(sock, 2))
        msg = _recv_exact(sock, mlen).decode("utf-8", errors="replace") if mlen else "receiver error"
        raise TransferError(msg)
    raise TransferError(f"unexpected receiver reply byte {tag!r}")


def _sha256_file(path: Path) -> tuple[int, int, bytes]:
    st = path.stat()
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(CHUNK_SIZE)
            if not chunk:
                break
            h.update(chunk)
    return st.st_size, st.st_mtime_ns, h.digest()


def _load_hash_cache(cache_path: Path | None) -> dict[str, tuple[int, int, bytes]]:
    cache: dict[str, tuple[int, int, bytes]] = {}
    if not cache_path or not cache_path.is_file():
        return cache
    for line in cache_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = line.split("\t")
        if len(parts) != 4:
            continue
        try:
            cache[parts[0]] = (int(parts[1]), int(parts[2]), bytes.fromhex(parts[3]))
        except ValueError:
            continue
    return cache


def _save_hash_cache(cache_path: Path | None, entries: dict[str, tuple[int, int, bytes]]) -> None:
    if not cache_path:
        return
    tmp = cache_path.with_suffix(".tsv.tmp")
    lines = [f"{rel}\t{sz}\t{mt}\t{digest.hex()}\n" for rel, (sz, mt, digest) in sorted(entries.items())]
    tmp.write_text("".join(lines), encoding="utf-8")
    tmp.replace(cache_path)


def collect_stage_files(
    stage_dir: Path,
    cache_path: Path | None = None,
    on_hash: Callable[[int, int], None] | None = None,
) -> list[tuple[str, Path, int, bytes]]:
    """Collect (rel_posix, path, size, sha256_bytes) for all regular files under stage_dir.

    Uses cache_path (size + mtime_ns -> sha256) so repeated rebuilds do not re-hash 23 GB of unchanged RomFS.
    Excludes eboot.bin from early streaming by sorting it last so a partial drop cannot leave an unbacked eboot.bin.
    """
    raw: list[tuple[str, Path]] = []
    for root, dirs, files in os.walk(stage_dir):
        dirs.sort()
        files.sort()
        rpath = Path(root)
        for name in files:
            p = rpath / name
            if p.is_symlink() or not p.is_file():
                continue
            rel = p.relative_to(stage_dir).as_posix()
            if rel in (".pitstop-manifest.tsv", CACHE_NAME) or rel.endswith(".pitstop.part"):
                continue
            raw.append((rel, p))

    # Send metadata/assets/shaders before eboot.bin so eboot.bin lands last once all data is on the console.
    raw.sort(key=lambda item: (1 if item[0] == "eboot.bin" else 0, item[0]))

    old_cache = _load_hash_cache(cache_path)
    new_cache: dict[str, tuple[int, int, bytes]] = {}
    out: list[tuple[str, Path, int, bytes]] = []
    total = len(raw)
    for idx, (rel, path) in enumerate(raw, 1):
        st = path.stat()
        key = str(path.resolve())
        cached = old_cache.get(key)
        if cached and cached[0] == st.st_size and cached[1] == st.st_mtime_ns:
            digest = cached[2]
        else:
            _, _, digest = _sha256_file(path)
        new_cache[key] = (st.st_size, st.st_mtime_ns, digest)
        out.append((rel, path, st.st_size, digest))
        if on_hash and (idx % 256 == 0 or idx == total):
            on_hash(idx, total)
    _save_hash_cache(cache_path, new_cache)
    return out


def parse_host_port(host_spec: str, default_port: int = DEFAULT_PORT) -> tuple[str, int]:
    spec = host_spec.strip()
    if ":" in spec and spec.count(":") == 1:
        host, port_str = spec.rsplit(":", 1)
        return host.strip(), int(port_str)
    return spec, default_port


def query_info(host: str, port: int = DEFAULT_PORT, timeout: float = 10.0) -> dict:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(b"PIT1I")
        raw_hdr = _recv_exact(sock, 4)
        if raw_hdr[:1] == b"E":
            mlen = struct.unpack("<H", raw_hdr[1:3])[0]
            msg = (raw_hdr[3:4] + _recv_exact(sock, max(0, mlen - 1))).decode("utf-8", errors="replace")
            raise TransferError(msg)
        (n,) = struct.unpack("<I", raw_hdr)
        payload = _recv_exact(sock, n).decode("utf-8")
        return json.loads(payload)


def send_directory(
    host_spec: str,
    stage_dir: Path,
    cache_path: Path | None = None,
    on_progress: Callable[[int, int, int, int], None] | None = None,
    timeout: float = 30.0,
) -> tuple[int, int, int]:
    """Send staged package files to the Pit Stop placeholder TCP receiver.

    Returns (files_sent, files_skipped, bytes_sent).
    """
    host, port = parse_host_port(host_spec)
    entries = collect_stage_files(stage_dir, cache_path=cache_path)
    total_files = len(entries)

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        sock.sendall(b"PIT1I")
        (info_len,) = struct.unpack("<I", _recv_exact(sock, 4))
        info = json.loads(_recv_exact(sock, info_len).decode("utf-8"))
        if info.get("title_id") != "PPSA99193":
            raise TransferError(f"unexpected console title {info.get('title_id')!r}")

        need: list[tuple[str, Path, int, bytes]] = []
        skipped = 0
        for start in range(0, total_files, STAT_BATCH):
            batch = entries[start : start + STAT_BATCH]
            pkt = bytearray(b"PIT1S")
            pkt += struct.pack("<I", len(batch))
            for rel, _, sz, digest in batch:
                rel_b = rel.encode("utf-8")
                pkt += struct.pack("<H", len(rel_b)) + rel_b + struct.pack("<Q", sz) + digest
            sock.sendall(pkt)
            first = _recv_exact(sock, 1)
            if first == b"E":
                (mlen,) = struct.unpack("<H", _recv_exact(sock, 2))
                msg = _recv_exact(sock, mlen).decode("utf-8", errors="replace")
                raise TransferError(msg)
            ans = first + (_recv_exact(sock, len(batch) - 1) if len(batch) > 1 else b"")
            for item, have in zip(batch, ans):
                if have == 1:
                    skipped += 1
                else:
                    need.append(item)

        send_files = len(need)
        send_bytes = sum(item[2] for item in need)
        free_bytes = int(info.get("free_bytes", 0))
        if free_bytes and send_bytes > free_bytes:
            raise TransferError(
                f"not enough free space on PS5 ({send_bytes / (1024**3):.2f} GiB needed, "
                f"{free_bytes / (1024**3):.2f} GiB free)"
            )

        sock.sendall(b"PIT1B" + struct.pack("<QQ", send_files, send_bytes))
        _expect_ack(sock)

        bytes_sent = 0
        for idx, (rel, path, sz, digest) in enumerate(need, 1):
            is_last = idx == send_files
            ack = 1 if (is_last or rel == "eboot.bin" or sz >= ACK_SIZE_THRESHOLD or idx % ACK_EVERY_FILES == 0) else 0
            rel_b = rel.encode("utf-8")
            hdr = b"PIT1P" + struct.pack("<BH", ack, len(rel_b)) + rel_b + struct.pack("<Q", sz) + digest
            sock.sendall(hdr)
            with path.open("rb") as f:
                rem = sz
                while rem > 0:
                    chunk = f.read(min(CHUNK_SIZE, rem))
                    if not chunk:
                        raise TransferError(f"local file shrank while sending {rel}")
                    sock.sendall(chunk)
                    rem -= len(chunk)
                    bytes_sent += len(chunk)
                    if on_progress and (rem == 0 or bytes_sent % (4 * 1024 * 1024) < CHUNK_SIZE):
                        on_progress(bytes_sent, send_bytes, idx, send_files)
            if sz == 0 and on_progress:
                on_progress(bytes_sent, send_bytes, idx, send_files)
            if ack:
                _expect_ack(sock)

        sock.sendall(b"PIT1D")
        _expect_ack(sock)
        if on_progress:
            on_progress(send_bytes, send_bytes, send_files, send_files)
        return send_files, skipped, send_bytes


# The PC builder's steps as the placeholder shows them (the launcher's names: launcher_setup.cpp kPs5Steps).
STATUS_STEPS = {
    "check": "Check the copy", "nup": "Unpack .nup", "exefs": "Copy exefs", "romfs": "RomFS",
    "generate": "Generate the game code", "compile": "Compile the game code", "link": "Link and sign eboot.bin",
    "shaders": "Translate the shaders", "deploy": "Send to PS5",
}  # fmt: skip
STATUS_ORDER = list(STATUS_STEPS)


class StatusNotifier:
    """The build's step and progress on the PS5's Game files missing screen (PIT1T), while the PC works.

    Why: the build takes 15-20 minutes before anything is sent, and the TV only said "Waiting for connection" all that
    time. Each update is one short connection from a thread of its own, at most once a second (a new step or a failure
    at once), with a 2 s timeout: a PS5 that is off, busy or older never slows the build. An older placeholder does not
    announce "status" in its PIT1I reply and gets nothing (it would answer an unknown command with an error screen)."""

    def __init__(self, host_spec: str) -> None:
        self.host, self.port = parse_host_port(host_spec)
        self.state = {"index": 0, "label": "", "progress": 0, "total": 0, "failed": "", "step": ""}
        self.cond = threading.Condition()
        self.dirty = self.urgent = False
        self.supported: bool | None = None   # None: not asked yet
        self.retry_at = 0.0
        threading.Thread(target=self._loop, name="pitstop-status", daemon=True).start()

    def event(self, words: list[str]) -> None:
        """One emit() line of prepare_ps5.py: step / skip / progress / error / done."""
        if not words:
            return
        kind, rest = words[0], words[1:]
        with self.cond:
            st = self.state
            if kind == "step" and rest and rest[0] in STATUS_STEPS:
                st.update(step=rest[0], index=STATUS_ORDER.index(rest[0]) + 1, label=STATUS_STEPS[rest[0]],
                          progress=0, total=0, failed="")  # fmt: skip
                self.urgent = True
            elif kind == "progress" and len(rest) >= 2 and st["step"] != "deploy":   # the transfer shows its own
                try:
                    st["progress"], st["total"] = int(rest[0]), int(rest[1])
                except ValueError:
                    return
            elif kind == "error":
                st["failed"] = " ".join(rest[1:])[:200] or "the build failed"
                self.urgent = True
            else:
                return
            self.dirty = True
            self.cond.notify()

    def flush(self, timeout: float = 3.0) -> None:
        """Wait until the last update went out (or could not): the failure before the builder exits."""
        end = time.monotonic() + timeout
        with self.cond:
            while self.dirty and self.supported is not False and time.monotonic() < end:
                self.cond.wait(0.1)

    def _loop(self) -> None:
        last = 0.0
        while True:
            with self.cond:
                while not self.dirty:
                    self.cond.wait()
                wait = 0.0 if self.urgent else max(0.0, last + 1.0 - time.monotonic())
            if wait:
                time.sleep(wait)
            with self.cond:
                st, self.dirty, self.urgent = dict(self.state), False, False
            if time.monotonic() >= self.retry_at:
                try:
                    self._send(st)
                except (OSError, TransferError, ValueError):
                    self.retry_at = time.monotonic() + 10.0   # unreachable: try again in a while
            last = time.monotonic()
            with self.cond:
                self.cond.notify_all()

    def _send(self, st: dict) -> None:
        with socket.create_connection((self.host, self.port), timeout=2.0) as sock:
            if self.supported is None:
                sock.sendall(b"PIT1I")
                (n,) = struct.unpack("<I", _recv_exact(sock, 4))
                self.supported = bool(json.loads(_recv_exact(sock, n).decode("utf-8")).get("status"))
            if not self.supported:
                return
            label = (st["failed"] or st["label"]).encode("utf-8")[:200]
            sock.sendall(b"PIT1T" + struct.pack("<BBBII", st["index"], len(STATUS_ORDER), 1 if st["failed"] else 0,
                                                 min(st["progress"], 0xFFFFFFFF), min(st["total"], 0xFFFFFFFF))
                         + struct.pack("<H", len(label)) + label)  # fmt: skip
            _expect_ack(sock)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", required=True, help="PS5 IP address (optionally host:port, default port 19913)")
    parser.add_argument("--stage", type=Path, required=True, help="staged PPSA99193 directory")
    parser.add_argument("--cache", type=Path, help="optional SHA-256 TSV cache file")
    args = parser.parse_args()

    stage = args.stage.resolve()
    if not stage.is_dir():
        print(f"error: staged directory does not exist: {stage}", file=sys.stderr)
        return 1

    def progress(bdone: int, btot: int, fdone: int, ftot: int) -> None:
        print(f"\rSending {fdone}/{ftot} files ({bdone / (1024**2):.1f}/{btot / (1024**2):.1f} MiB)", end="", flush=True)

    sent, skipped, total_bytes = send_directory(args.host, stage, cache_path=args.cache, on_progress=progress)
    print(f"\nDone: sent {sent} files ({total_bytes / (1024**2):.1f} MiB), skipped {skipped} unchanged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
