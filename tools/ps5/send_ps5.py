"""Send a staged PPSA99193 package directory to the Pit Stop placeholder TCP receiver (PIT1).

Why: FTP requires running a separate FTP server on the PS5 and issuing hundreds of thousands of per-file commands for
23 GB of RomFS. The Pit Stop placeholder screen listens on port 19913 only while game files are missing/outdated,
checks batches of files against its persistent SHA-256 manifest, streams missing/changed files over one TCP connection
with SHA-256 verification and atomic renames, and closes the listener immediately on completion.

Large files (romfs.nup) go by FTP when the console runs an FTP server (port 2121, else 1337): the title's own writes
drop to ~2 MiB/s after its first ~2 GB, the FTP server's do not (measured 23/11/2026). The receiver still checks
their SHA-256 and puts them in place (PIT1A). Without an FTP server they go over PIT1 as before.

Usage:
    python3 tools/ps5/send_ps5.py --host 192.168.1.50 --stage build/ps5-work/stage/PPSA99193
"""

from __future__ import annotations

import argparse
import calendar
import contextlib
import ftplib
import hashlib
import json
import os
import secrets
import socket
import struct
import sys
import threading
import time
from pathlib import Path
from typing import Callable

DEFAULT_PORT = 19913
CHUNK_SIZE = 256 * 1024          # hashing reads
SEND_CHUNK = 1024 * 1024         # file data reads: the receiver's pipeline slot (ps5_recv.c PIPE_SLOT_SIZE)
SOCK_BUF = 4 * 1024 * 1024       # send buffer: Windows' default kept too little in flight to the console
STAT_BATCH = 4096
ACK_EVERY_FILES = 128
ACK_SIZE_THRESHOLD = 256 * 1024
RESUME_MIN = 64 * 1024 * 1024   # files this large go on where a broken send left them (receivers with "resume")
CACHE_NAME = ".pitstop-client-sha256.tsv"
# Files this large go by FTP when the receiver can take them (PIT1A): the title's own writes run at ~20 MiB/s for its
# first ~2 GB, then at ~2 MiB/s (23/11/2026: 3 GiB in 554 s by PIT1, 151 s by FTP to the same console). eboot.bin,
# sce_module/ and sce_sys/ stay on PIT1, last (send_rank).
FTP_MIN = 256 * 1024 * 1024
FTP_PORTS = (2121, 1337)   # ftpsrv's port, then etaHEN's
FTP_SUFFIX = ".pitstop-ftp"   # ps5_recv.c FTP_SUFFIX
TITLE_DIRS = ("/data/homebrew/PPSA99193",)   # where the title folder may be when /user/app/PPSA99193/mount.lnk is not


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


def send_rank(rel: str, size: int) -> int:
    """The order of a send. Large files first: most of the time is theirs, so the speed and time left mean something
    at once. What the title starts from last, eboot.bin at the very end: a send that breaks leaves the placeholder's
    own eboot.bin, sce_module/ and sce_sys/ untouched, so it still starts and takes the rest (a broken send left a
    title that crashed at launch, with no way back to the receiver, 18/11/2026)."""
    if rel == "eboot.bin":
        return 3
    if rel.startswith(("sce_module/", "sce_sys/")):
        return 2
    return 0 if size >= RESUME_MIN else 1


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
            if rel in (".pitstop-manifest.tsv", CACHE_NAME) or rel.endswith((".pitstop.part", ".nup.part")):
                continue
            if rel.startswith("spirv/") and (stage_dir / "spirv.nup").is_file():
                continue   # the translator's folder: spirv.nup is what the title reads
            raw.append((rel, p))

    raw.sort(key=lambda item: (send_rank(item[0], item[1].stat().st_size), item[0]))

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


TITLE_VIEW_FREE = 16 << 30   # what statvfs answers inside the title, for /app0 and /data alike


def usable_free(info: dict) -> int | None:
    """The console's free bytes, or None when its receiver cannot tell (a full disk still fails the write). Inside the
    title statvfs answers exactly 16.00 GiB for /app0 and for /data, on a console with 231 GiB free: the placeholders
    of 18-20/11/2026 sent that as "free_real", and a 23 GB romfs.nup was refused or had the PS5 delete folders to make
    room (20/11/2026). Exactly that figure is no measurement."""
    if not info.get("free_real"):
        return None
    free = int(info.get("free_bytes", 0))
    return None if free == TITLE_VIEW_FREE else free


def _no_space(need: int, free: int) -> str:
    return f"not enough free space on PS5 ({need / 2**30:.2f} GiB needed, {max(free, 0) / 2**30:.2f} GiB free)"


def timing_line(sent: int, wall: float, t_read: float, t_send: float, info: dict) -> str:
    """One line on where a send's time went, both ends, for the setup log: players saw ~1 MB/s where FTP to the same
    console is far faster (r/PS5_Jailbreak, 23/11/2026), and nothing said whether the network, the console's disk or
    this PC was the cause. On the PS5: waiting on the network, waiting for a free buffer (its disk or hash behind),
    time in write() and in SHA-256, its receive buffer and the folder it writes into."""
    mib = sent / 2**20
    line = (f"send timing: {mib:.0f} MiB in {wall:.1f} s ({mib / wall if wall else 0:.1f} MiB/s); "
            f"PC: disk reads {t_read:.1f} s, sending {t_send:.1f} s")  # fmt: skip
    t = info.get("timing")
    if not t:
        return line + "; PS5: this receiver does not report timing"
    return line + (
        f"; PS5 ({info.get('root', '?')}): network wait {t['net_ms'] / 1000:.1f} s, waiting for its disk"
        f" {t['slot_ms'] / 1000:.1f} s, write() {t['write_ms'] / 1000:.1f} s, SHA-256 {t['hash_ms'] / 1000:.1f} s,"
        f" of {t['send_ms'] / 1000:.1f} s; receive buffer {t['rcvbuf'] // 1024} KiB"
    )


class FtpTarget:
    """The console's FTP server, writing into the title's folder: the folder from /user/app/PPSA99193/mount.lnk (where
    the PS5 mounts the title from), proven by a probe file the receiver must find there (PIT1A flag 1)."""

    def __init__(self, ftp: ftplib.FTP, folder: str) -> None:
        self.ftp, self.folder = ftp, folder.rstrip("/")

    def path(self, rel: str) -> str:
        return f"{self.folder}/{rel}{FTP_SUFFIX}"

    def remove(self, rel: str) -> None:
        """This builder's own upload of rel, by its name only (never a folder: the 22/11/2026 FTP cleanup incident)."""
        with contextlib.suppress(ftplib.error_perm):
            self.ftp.delete(self.path(rel))

    def upload(self, rel: str, path: Path, on_bytes: Callable[[int], None]) -> None:
        parts = rel.split("/")[:-1]
        for i in range(len(parts)):   # the receiver made them too (PIT1A); a server may not show them yet
            with contextlib.suppress(ftplib.error_perm):
                self.ftp.mkd(f"{self.folder}/{'/'.join(parts[: i + 1])}")
        with path.open("rb") as f:
            # APPE into the empty file the receiver made: one the FTP server makes itself, the title does not see
            # (23/11/2026)
            self.ftp.storbinary(f"APPE {self.path(rel)}", f, blocksize=SEND_CHUNK, callback=lambda b: on_bytes(len(b)))

    def close(self) -> None:
        with contextlib.suppress(*ftplib.all_errors):
            self.ftp.quit()


def default_ftp_ports() -> tuple[int, ...]:
    """FTP_PORTS, or PS5_FTP_PORT's one port as deploy.py reads it (0: large files go by PIT1 too)."""
    env = os.environ.get("PS5_FTP_PORT", "").strip()
    if env.isdigit():
        return (int(env),) if int(env) else ()
    return FTP_PORTS


def open_ftp(host: str, port: int, timeout: float = 5.0) -> ftplib.FTP:
    f = ftplib.FTP()
    f.connect(host, port, timeout=timeout)
    f.login()
    f.voidcmd("TYPE I")
    f.timeout = 60.0   # the data connections of a 23 GB upload: the console's disk may pause
    f.sock.settimeout(60.0)
    return f


def title_dirs(ftp: ftplib.FTP) -> list[str]:
    """Where the title folder may be for FTP: mount.lnk's (where the PS5 mounts it from), then TITLE_DIRS."""
    out: list[str] = []
    with contextlib.suppress(*ftplib.all_errors, UnicodeDecodeError):
        lines: list[str] = []
        ftp.retrlines("RETR /user/app/PPSA99193/mount.lnk", lines.append)
        link = "".join(lines).strip().rstrip("/")
        if link.startswith("/") and link not in out:
            out.append(link)
    return out + [d for d in TITLE_DIRS if d not in out]


def _adopt_answer(sock: socket.socket) -> str | None:
    """None when the receiver goes on (at PIT1A) or put the file in place (after 'U'), else why not ('p' bytes: it is
    still hashing)."""
    while True:
        tag = _recv_exact(sock, 1)
        if tag == b"p":
            continue
        if tag == b"K":
            return None
        if tag in (b"N", b"E"):
            (mlen,) = struct.unpack("<H", _recv_exact(sock, 2))
            msg = _recv_exact(sock, mlen).decode("utf-8", errors="replace") if mlen else "refused"
            if tag == b"E":
                raise TransferError(msg)
            return msg
        raise TransferError(f"unexpected receiver reply byte {tag!r}")


def adopt(sock: socket.socket, target: FtpTarget, rel: str, path: Path, sz: int, digest: bytes,
          on_bytes: Callable[[int], None], probe: bool = False) -> str | None:
    """One file by FTP, checked and put in place by the receiver (PIT1A). None when done, else why not (the caller
    sends it by PIT1P); a broken PIT1 connection raises."""
    target.remove(rel)   # an earlier try's, before the receiver starts reading the name
    rel_b, dir_b = rel.encode("utf-8"), target.folder.encode("utf-8")
    sock.sendall(b"PIT1A" + struct.pack("<BH", 1 if probe else 0, len(rel_b)) + rel_b + struct.pack("<Q", sz) + digest
                 + struct.pack("<H", len(dir_b)) + dir_b)  # fmt: skip
    why = _adopt_answer(sock)   # 'N': the receiver does not see that folder as its own
    if why:
        return why
    try:
        target.upload(rel, path, on_bytes)
    except (*ftplib.all_errors, OSError) as e:
        sock.sendall(b"C")
        _expect_ack(sock)
        with contextlib.suppress(*ftplib.all_errors):
            target.remove(rel)
        return f"FTP upload failed: {e}"
    sock.sendall(b"U")
    why = _adopt_answer(sock)
    if why:
        with contextlib.suppress(*ftplib.all_errors):
            target.remove(rel)
    return why


def find_ftp(sock: socket.socket, host: str, ports: tuple[int, ...], work: Path,
             opener: Callable[[str, int], ftplib.FTP] = open_ftp) -> FtpTarget | None:
    """The console's FTP server and the title's folder in it, proven by a probe the receiver finds; None (with why on
    stderr) when there is none, and the send goes all by PIT1."""
    probe = work / ".pitstop-ftp-probe"
    for port in ports:
        try:
            ftp = opener(host, port)
        except (*ftplib.all_errors, OSError):
            continue
        tried = []
        for folder in title_dirs(ftp):
            data = secrets.token_bytes(64)
            probe.write_bytes(data)
            target = FtpTarget(ftp, folder)
            try:
                why = adopt(sock, target, probe.name, probe, len(data), hashlib.sha256(data).digest(), lambda n: None,
                            probe=True)  # fmt: skip
            finally:
                probe.unlink(missing_ok=True)
            if why is None:
                return target
            tried.append(f"{folder}: {why}")
        print(f"send_ps5: FTP on port {port} answers, but the title folder was not found there ({'; '.join(tried)})",
              file=sys.stderr)  # fmt: skip
        with contextlib.suppress(*ftplib.all_errors):
            ftp.quit()
    return None


def send_directory(
    host_spec: str,
    stage_dir: Path,
    cache_path: Path | None = None,
    on_progress: Callable[[int, int, int, int], None] | None = None,
    timeout: float = 30.0,
    on_phase: Callable[[str, int, int], None] | None = None,
    on_plan: Callable[[int, int, int, int], None] | None = None,
    ftp_ports: tuple[int, ...] = FTP_PORTS,
    ftp_opener: Callable[[str, int], ftplib.FTP] = open_ftp,
) -> tuple[int, int, int]:
    """Send staged package files to the Pit Stop placeholder TCP receiver.

    on_plan(files, bytes, kept_files, kept_bytes) says, once the console's files are compared, what goes and what
    stays (prepare_ps5.py tells the player whether this is a first install or an upgrade).
    on_phase(phase, done, total) reports the steps before the bytes flow, which took minutes in silence on a first
    send (18/11/2026): "hash" (files checksummed here, each 23 GB first time, cached after), "check" (files the PS5
    was asked about), then "send" once with the bytes to go (on_progress follows the transfer).
    Files of FTP_MIN and up go by the console's FTP server on ftp_ports when one answers (empty: never).
    Returns (files_sent, files_skipped, bytes_sent).
    """
    host, port = parse_host_port(host_spec)
    entries = collect_stage_files(stage_dir, cache_path=cache_path,
                                  on_hash=(lambda d, t: on_phase("hash", d, t)) if on_phase else None)
    total_files = len(entries)

    def connect() -> tuple[socket.socket, dict]:
        s = socket.create_connection((host, port), timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, SOCK_BUF)
        except OSError:
            pass   # the default buffer still works
        s.sendall(b"PIT1I")
        (n,) = struct.unpack("<I", _recv_exact(s, 4))
        return s, json.loads(_recv_exact(s, n).decode("utf-8"))

    sock, info = connect()
    with contextlib.ExitStack() as stack:
        stack.callback(lambda: sock.close())
        if info.get("title_id") != "PPSA99193":
            raise TransferError(f"unexpected console title {info.get('title_id')!r}")
        can_resume = bool(info.get("resume"))
        can_remove = bool(info.get("remove"))
        can_adopt = bool(info.get("ftp"))

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
            if on_phase:
                on_phase("check", min(start + STAT_BATCH, total_files), total_files)

        send_files = len(need)
        send_bytes = sum(item[2] for item in need)
        if on_plan:   # (files to send, their bytes, files the console has already, their bytes): an upgrade or not
            on_plan(send_files, send_bytes, skipped, sum(e[2] for e in entries) - send_bytes)
        # a package replaces the loose folder an earlier send left beside it (romfs/: 23 GB kept twice otherwise)
        def remove_loose() -> None:
            nonlocal sock
            for rel, *_ in entries:
                if rel == "spirv.nup" or rel == "romfs.nup" or rel.endswith("/romfs.nup"):
                    loose = rel[: -len(".nup")].encode("utf-8")   # romfs/ or spirv/ beside its package
                    sock.sendall(b"PIT1X" + struct.pack("<H", len(loose)) + loose)
                    try:
                        _expect_ack(sock)
                    except TransferError as e:
                        # Only space is lost: the title reads the package. Files an FTP upload left there are not the
                        # title's to delete ("Operation not permitted", 20/11/2026), and the receiver closes the
                        # connection after an error, so the send goes on over a new one.
                        print(f"send_ps5: the PS5 kept {loose.decode()}/ ({e}); delete it by FTP to free its space",
                              file=sys.stderr)
                        sock.close()
                        sock, _ = connect()

        free_bytes = usable_free(info)
        resumable = sum(item[2] for item in need if can_resume and item[2] >= RESUME_MIN)
        removed = False
        if free_bytes is not None and send_bytes > free_bytes and can_remove:
            remove_loose()   # it may be what fills the console: gone first, then counted again
            removed = True
            sock.sendall(b"PIT1I")
            (n,) = struct.unpack("<I", _recv_exact(sock, 4))
            free_bytes = usable_free(json.loads(_recv_exact(sock, n).decode("utf-8")))
        # a resumable file may be partly there already: it is checked once the receiver says how much (below)
        if free_bytes is not None and send_bytes - resumable > free_bytes:
            raise TransferError(_no_space(send_bytes - resumable, free_bytes))

        sock.sendall(b"PIT1B" + struct.pack("<QQ", send_files, send_bytes))
        _expect_ack(sock)
        if on_phase:
            on_phase("send", 0, send_bytes)

        bytes_sent = skipped_bytes = 0   # skipped: what a resumed file already had on the console
        t_read = t_send = 0.0   # this PC's disk reads and socket sends (timing_line)
        buf = bytearray(SEND_CHUNK)
        view = memoryview(buf)
        next_report = 0
        ftp_sent = 0   # bytes the FTP server wrote (timing_line counts the receiver's own)
        big = [i for i, item in enumerate(need) if item[2] >= FTP_MIN and send_rank(item[0], item[2]) < 2]
        target = None
        if big and can_adopt and ftp_ports:
            target = find_ftp(sock, host, ftp_ports, cache_path.parent if cache_path else stage_dir.parent, ftp_opener)
        if target:
            stack.callback(target.close)
            by_ftp: set[int] = set()
            for i in big:
                rel, path, sz, digest = need[i]
                t0, base = time.monotonic(), bytes_sent

                def ftp_bytes(n: int, idx: int = i + 1) -> None:
                    nonlocal bytes_sent, next_report
                    bytes_sent += n
                    if on_progress and bytes_sent >= next_report:
                        next_report = bytes_sent + 4 * 1024 * 1024
                        on_progress(bytes_sent, send_bytes, idx, send_files)

                why = adopt(sock, target, rel, path, sz, digest, ftp_bytes)
                if why:
                    bytes_sent = base
                    print(f"send_ps5: {rel} goes by PIT1 instead of FTP: {why}", file=sys.stderr)
                    continue
                by_ftp.add(i)
                ftp_sent += sz
                dt = time.monotonic() - t0
                print(f"send_ps5: {rel} by FTP to {target.folder}, {sz / 2**20:.0f} MiB in {dt:.1f} s"
                      f" ({sz / 2**20 / dt if dt else 0:.1f} MiB/s)", file=sys.stderr)  # fmt: skip
            # the rest by PIT1P, in their order; the last one still answers (its ack ends the send)
            need = [item for i, item in enumerate(need) if i not in by_ftp]
        t_pit1 = time.monotonic()
        for idx, (rel, path, sz, digest) in enumerate(need, send_files - len(need) + 1):
            is_last = idx == send_files
            ack = 1 if (is_last or rel == "eboot.bin" or sz >= ACK_SIZE_THRESHOLD or idx % ACK_EVERY_FILES == 0) else 0
            resume = can_resume and sz >= RESUME_MIN
            rel_b = rel.encode("utf-8")
            hdr = b"PIT1P" + struct.pack("<BH", ack | (2 if resume else 0), len(rel_b)) + rel_b + struct.pack("<Q", sz) + digest
            sock.sendall(hdr)
            start = 0
            if resume:   # the receiver answers where its .part of this very file ends (0: from the start)
                first = _recv_exact(sock, 1)
                if first == b"E":
                    (mlen,) = struct.unpack("<H", _recv_exact(sock, 2))
                    raise TransferError(_recv_exact(sock, mlen).decode("utf-8", errors="replace"))
                (start,) = struct.unpack("<Q", first + _recv_exact(sock, 7))
                if start > sz:
                    raise TransferError(f"receiver resumes {rel} past its end")
                bytes_sent += start
                skipped_bytes += start
                left = send_bytes - bytes_sent
                if free_bytes is not None and left > free_bytes - (bytes_sent - skipped_bytes):
                    raise TransferError(_no_space(left, free_bytes - (bytes_sent - skipped_bytes)))
            with path.open("rb", buffering=0) as f:
                f.seek(start)
                rem = sz - start
                while rem > 0:
                    t0 = time.monotonic()
                    n = f.readinto(view[: min(SEND_CHUNK, rem)])
                    t1 = time.monotonic()
                    if not n:
                        raise TransferError(f"local file shrank while sending {rel}")
                    sock.sendall(view[:n])
                    t_send += time.monotonic() - t1
                    t_read += t1 - t0
                    rem -= n
                    bytes_sent += n
                    if on_progress and (rem == 0 or bytes_sent >= next_report):
                        next_report = bytes_sent + 4 * 1024 * 1024
                        on_progress(bytes_sent, send_bytes, idx, send_files)
            if sz == 0 and on_progress:
                on_progress(bytes_sent, send_bytes, idx, send_files)
            if ack:
                _expect_ack(sock)

        if can_remove and not removed:
            remove_loose()
        if send_bytes > ftp_sent:
            # the console's side of the same send (ps5_recv.c "timing"; an older receiver has none); FTP's files have
            # their own line above, so this one counts from after them (t_pit1), and a send all by FTP has none
            sock.sendall(b"PIT1I")
            (n,) = struct.unpack("<I", _recv_exact(sock, 4))
            ps5 = json.loads(_recv_exact(sock, n).decode("utf-8"))
            print(timing_line(bytes_sent - skipped_bytes - ftp_sent, time.monotonic() - t_pit1, t_read, t_send, ps5),
                  file=sys.stderr)  # fmt: skip
        sock.sendall(b"PIT1D")
        _expect_ack(sock)
        if on_progress:
            on_progress(send_bytes, send_bytes, send_files, send_files)
        return send_files, skipped, send_bytes


# ---- The whole title by FTP, when the receiver is not running
# A working install starts the game and never listens, and a first install needed the placeholder uploaded by hand: the
# console's FTP server (ftpsrv 2121, etaHEN 1337) runs either way, so the builder installs over it instead. What the
# receiver did that FTP cannot: SHA-256 on the console (here only each file's size is checked after upload; TCP's own
# checksums guard the bytes) and the TV's progress screen. ftpsrv (1.16, 23/11/2026) makes files and folders 0777,
# overwrites on STOR, replaces on RNTO, and AVBL answers the drive's real free space (230 GB, where the title's statvfs
# says 16 GiB). Never a folder deleted, never a delete by pattern (the 22/11/2026 incident): only named files, ours
# (.pitstop-ftp uploads) or stale ones in an exefs/ we are replacing (1.0.0 has no subsdk1: one left would be loaded).

FTP_RECORD_NAME = "ftp-sent.tsv"   # beside the send cache: what this builder put on which console, to skip it next time
FTP_DEFAULT_TITLE = "/data/homebrew/PPSA99193"
REGISTERED_SCE_SYS = "/user/app/PPSA99193/sce_sys"   # ShadowMountPlus's copy the home screen reads (deploy.py)


def receiver_up(host_spec: str, timeout: float = 3.0) -> bool:
    """Whether the Pit Stop receiver answers (the title on its receiver screen)."""
    try:
        query_info(*parse_host_port(host_spec), timeout=timeout)
        return True
    except (OSError, TransferError, ValueError):
        return False


def ftp_server(host: str, ports: tuple[int, ...] = FTP_PORTS,
               opener: Callable[[str, int], ftplib.FTP] = open_ftp) -> tuple[ftplib.FTP, int] | None:
    for port in ports:
        try:
            return opener(host, port), port
        except (*ftplib.all_errors, OSError):
            continue
    return None


def _ftp_exists(ftp: ftplib.FTP, path: str) -> bool:
    try:
        ftp.sendcmd(f"MLST {path}")
        return True
    except ftplib.error_perm:
        return False


def ftp_title_folder(ftp: ftplib.FTP) -> tuple[str, bool]:
    """(the title folder, whether it exists): mount.lnk's (the registered title's), else the first of TITLE_DIRS that
    exists, else FTP_DEFAULT_TITLE to create (a first install: ShadowMountPlus registers it once it holds eboot.bin)."""
    for folder in title_dirs(ftp):
        if _ftp_exists(ftp, folder):
            return folder, True
    return FTP_DEFAULT_TITLE, False


def _ftp_listing(ftp: ftplib.FTP, folder: str) -> dict[str, tuple[int, str]]:
    """rel -> (size, modify UTC 'YYYYMMDDHHMMSS') of the files under folder, by MLSD (one round trip a folder)."""
    out: dict[str, tuple[int, str]] = {}
    todo = [""]
    while todo:
        rel = todo.pop()
        try:
            rows = list(ftp.mlsd(f"{folder}/{rel}".rstrip("/")))
        except ftplib.error_perm:
            continue
        for name, facts in rows:
            if name in (".", ".."):
                continue
            sub = f"{rel}/{name}" if rel else name
            if facts.get("type") == "dir":
                todo.append(sub)
            elif facts.get("type") == "file":
                out[sub] = (int(facts.get("size", -1)), facts.get("modify", ""))
    return out


def _load_ftp_record(path: Path | None, key: str) -> dict[str, tuple[int, str, str]]:
    """rel -> (size, sha256 hex, remote modify) of what this builder uploaded to key (host and folder)."""
    if not path or not path.is_file():
        return {}
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != key:
        return {}
    out = {}
    for line in lines[1:]:
        parts = line.split("\t")
        if len(parts) == 4 and parts[1].isdigit():
            out[parts[0]] = (int(parts[1]), parts[2], parts[3])
    return out


def _save_ftp_record(path: Path | None, key: str, record: dict[str, tuple[int, str, str]]) -> None:
    if not path:
        return
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(key + "\n" + "".join(f"{r}\t{s}\t{h}\t{m}\n" for r, (s, h, m) in sorted(record.items())),
                   encoding="utf-8")
    os.replace(tmp, path)


def _console_manifest(ftp: ftplib.FTP, folder: str) -> dict[str, tuple[int, int, str]]:
    """The receiver's own record (ps5_recv.c .pitstop-manifest.tsv: rel, size, mtime_ns, sha256): files it received
    and checked, so an install it made is not sent again by FTP."""
    lines: list[str] = []
    with contextlib.suppress(*ftplib.all_errors, UnicodeDecodeError):
        ftp.retrlines(f"RETR {folder}/.pitstop-manifest.tsv", lines.append)
    out = {}
    for line in lines:
        parts = line.split("\t")
        if len(parts) == 4 and parts[1].isdigit() and parts[2].isdigit():
            out[parts[0]] = (int(parts[1]), int(parts[2]), parts[3])
    return out


def _utc_stamp(seconds: float) -> str:
    return time.strftime("%Y%m%d%H%M%S", time.gmtime(seconds))


def _stamp_seconds(stamp: str) -> float | None:
    try:
        return calendar.timegm(time.strptime(stamp[:14], "%Y%m%d%H%M%S"))
    except ValueError:
        return None


def _site_chmod(ftp: ftplib.FTP, path: str) -> None:
    """0777, so the title's receiver may replace it later (ftpsrv makes files 0777 already; another server may not)."""
    with contextlib.suppress(*ftplib.all_errors):
        ftp.sendcmd(f"SITE CHMOD 777 {path}")


def send_directory_ftp(
    host_spec: str,
    stage_dir: Path,
    cache_path: Path | None = None,
    on_progress: Callable[[int, int, int, int], None] | None = None,
    on_phase: Callable[[str, int, int], None] | None = None,
    on_plan: Callable[[int, int, int, int], None] | None = None,
    ftp_ports: tuple[int, ...] = FTP_PORTS,
    ftp_opener: Callable[[str, int], ftplib.FTP] = open_ftp,
    record_path: Path | None = None,
) -> tuple[int, int, int]:
    """The staged title to the console's FTP server, without the receiver (see above). A file is skipped when this
    builder's record (record_path) or the receiver's manifest has its SHA-256 and the server still shows that file
    unchanged. Each upload goes to <rel>.pitstop-ftp, its size is checked, and it is renamed over the old one;
    eboot.bin last. Returns (files_sent, files_skipped, bytes_sent) as send_directory does."""
    host, _ = parse_host_port(host_spec)
    found = ftp_server(host, ftp_ports, ftp_opener)
    if not found:
        raise TransferError(f"no Pit Stop receiver (port {DEFAULT_PORT}) and no FTP server "
                            f"({', '.join(map(str, ftp_ports)) or 'none'}) answered at {host}")
    ftp, port = found
    try:
        entries = collect_stage_files(stage_dir, cache_path=cache_path,
                                      on_hash=(lambda d, t: on_phase("hash", d, t)) if on_phase else None)
        folder, exists = ftp_title_folder(ftp)
        key = f"{host}:{port}:{folder}"
        record = _load_ftp_record(record_path, key)
        listing = _ftp_listing(ftp, folder) if exists else {}
        manifest = _console_manifest(ftp, folder) if exists else {}
        need, skipped = [], 0
        for rel, path, sz, digest in entries:
            have = listing.get(rel)
            ours = record.get(rel)
            theirs = manifest.get(rel)
            seconds = _stamp_seconds(have[1]) if have else None
            same = have is not None and have[0] == sz and (
                (ours is not None and ours[0] == sz and ours[1] == digest.hex() and ours[2] == have[1])
                or (theirs is not None and theirs[0] == sz and theirs[2] == digest.hex() and seconds is not None
                    and abs(theirs[1] / 1e9 - seconds) <= 2))
            if same:
                skipped += 1
            else:
                need.append((rel, path, sz, digest))
        send_bytes = sum(item[2] for item in need)
        if on_plan:
            on_plan(len(need), send_bytes, skipped, sum(e[2] for e in entries) - send_bytes)
        with contextlib.suppress(*ftplib.all_errors, ValueError):
            free = int(ftp.sendcmd(f"AVBL {folder.rsplit('/', 1)[0] or '/'}").split()[-1])
            if send_bytes > free:
                raise TransferError(_no_space(send_bytes, free))
        print(f"send_ps5: no receiver at {host}; installing by FTP (port {port}) into {folder}: {len(need)} files, "
              f"{send_bytes / 2**20:.1f} MiB, {skipped} unchanged", file=sys.stderr)  # fmt: skip

        made: set[str] = set()

        def ensure_dirs(rel: str) -> None:
            parts = rel.split("/")[:-1]
            for i in range(-1, len(parts)):
                d = "/".join([folder] + parts[: i + 1])
                if d in made:
                    continue
                if not _ftp_exists(ftp, d):
                    ftp.mkd(d)
                    _site_chmod(ftp, d)
                made.add(d)

        # a staged exefs/ replaces the console's: a module it does not have goes, by name (1.0.0 has no subsdk1)
        staged = {rel for rel, *_ in entries}
        for rel in sorted(listing):
            parent, _, name = rel.rpartition("/")
            module = name in ("main", "main.npdm", "rtld", "sdk") or (name.startswith("subsdk") and name[6:].isdigit())
            if (parent.split("/")[-1] == "exefs" and module and rel not in staged
                    and any(e.startswith(parent + "/") for e in staged)):
                with contextlib.suppress(*ftplib.all_errors):
                    ftp.delete(f"{folder}/{rel}")
                    print(f"send_ps5: removed {rel} from the PS5 (not in this version's exefs)", file=sys.stderr)
                record.pop(rel, None)

        if on_phase:
            on_phase("send", 0, send_bytes)
        sent_bytes, t0 = 0, time.monotonic()
        for idx, (rel, path, sz, digest) in enumerate(need, 1):
            ensure_dirs(rel)
            tmp, final = f"{folder}/{rel}{FTP_SUFFIX}", f"{folder}/{rel}"

            def counted(block: bytes) -> None:
                nonlocal sent_bytes
                sent_bytes += len(block)
                if on_progress:
                    on_progress(sent_bytes, send_bytes, idx, len(need))

            try:
                with path.open("rb") as f:
                    ftp.storbinary(f"STOR {tmp}", f, blocksize=SEND_CHUNK, callback=counted)
                got = ftp.size(tmp)
                if got != sz:
                    raise TransferError(f"{rel}: {got} bytes arrived on the PS5, {sz} were sent")
                ftp.rename(tmp, final)
            except BaseException:
                with contextlib.suppress(*ftplib.all_errors):
                    ftp.delete(tmp)
                raise
            _site_chmod(ftp, final)
            stamp = ""
            with contextlib.suppress(*ftplib.all_errors):
                reply = ftp.sendcmd(f"MLST {final}")
                stamp = next((f.split("=", 1)[1] for f in reply.split(";") if f.strip().startswith("modify=")), "")
            record[rel] = (sz, digest.hex(), stamp[:14])
            _save_ftp_record(record_path, key, record)   # after each file: a broken send keeps what arrived
        wall = time.monotonic() - t0
        if sent_bytes:
            print(f"send_ps5: by FTP {sent_bytes / 2**20:.0f} MiB in {wall:.1f} s "
                  f"({sent_bytes / 2**20 / wall if wall else 0:.1f} MiB/s)", file=sys.stderr)  # fmt: skip
        _save_ftp_record(record_path, key, record)
        # the home screen reads ShadowMountPlus's copy of param.json and icon0.png (deploy.py refresh_registered_sce_sys)
        if any(rel.startswith("sce_sys/") for rel, *_ in need) and _ftp_exists(ftp, f"{REGISTERED_SCE_SYS}/param.json"):
            for name in ("param.json", "icon0.png"):
                local = stage_dir / "sce_sys" / name
                if local.is_file():
                    with contextlib.suppress(*ftplib.all_errors), local.open("rb") as f:
                        ftp.storbinary(f"STOR {REGISTERED_SCE_SYS}/{name}", f)
        if on_progress:
            on_progress(send_bytes, send_bytes, len(need), len(need))
        return len(need), skipped, send_bytes
    finally:
        with contextlib.suppress(*ftplib.all_errors):
            ftp.quit()


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
    parser.add_argument("--ftp-port", type=int, action="append",
                        help=f"the console's FTP port for large files (default {', '.join(map(str, FTP_PORTS))}; 0: PIT1 only)")
    args = parser.parse_args()

    stage = args.stage.resolve()
    if not stage.is_dir():
        print(f"error: staged directory does not exist: {stage}", file=sys.stderr)
        return 1

    def progress(bdone: int, btot: int, fdone: int, ftot: int) -> None:
        print(f"\rSending {fdone}/{ftot} files ({bdone / (1024**2):.1f}/{btot / (1024**2):.1f} MiB)", end="", flush=True)

    ports = tuple(p for p in args.ftp_port if p) if args.ftp_port else default_ftp_ports()
    sent, skipped, total_bytes = send_directory(args.host, stage, cache_path=args.cache, on_progress=progress,
                                                ftp_ports=ports)  # fmt: skip
    print(f"\nDone: sent {sent} files ({total_bytes / (1024**2):.1f} MiB), skipped {skipped} unchanged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
