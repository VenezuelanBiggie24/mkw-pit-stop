"""The PS5 builder's install over the console's FTP server when the Pit Stop receiver is not running
(tools/ps5/send_ps5.py send_directory_ftp), against a fake FTP server over a local folder: a first install, a second
send that skips what did not change, a file the receiver checked before (its manifest), stale exefs modules removed by
name, and nothing else ever deleted."""

from __future__ import annotations

import ftplib
import hashlib
import os
from pathlib import Path
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ps5"))

import send_ps5  # noqa: E402


class FakeFtp:
    """The ftplib calls send_directory_ftp makes, over console/ (the console's root), as ftpsrv answers them."""

    def __init__(self, console: Path, deleted: list[str]) -> None:
        self.root, self.deleted = console, deleted

    def _p(self, path: str) -> Path:
        return self.root / path.lstrip("/")

    def sendcmd(self, cmd: str) -> str:
        verb, _, arg = cmd.partition(" ")
        if verb == "MLST":
            p = self._p(arg)
            if not p.exists():
                raise ftplib.error_perm("550 No such file or directory")
            stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime(p.stat().st_mtime))
            return f"250-Listing\n type={'dir' if p.is_dir() else 'file'};size={p.stat().st_size};modify={stamp}; {arg}\n250 End"
        if cmd.startswith("SITE CHMOD"):
            return "200 OK"
        if verb == "AVBL":
            return "213 999999999999"
        raise ftplib.error_perm(f"500 {verb}")

    def mkd(self, path: str) -> str:
        self._p(path).mkdir()
        return path

    def mlsd(self, path: str):
        p = self._p(path)
        if not p.is_dir():
            raise ftplib.error_perm("550 No such file or directory")
        for e in sorted(p.iterdir()):
            stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime(e.stat().st_mtime))
            yield e.name, {"type": "dir" if e.is_dir() else "file", "size": str(e.stat().st_size), "modify": stamp}

    def retrlines(self, cmd: str, callback) -> str:
        p = self._p(cmd.split(" ", 1)[1])
        if not p.is_file():
            raise ftplib.error_perm("550 No such file or directory")
        for line in p.read_text(encoding="utf-8").splitlines():
            callback(line)
        return "226 OK"

    def storbinary(self, cmd: str, f, blocksize: int = 8192, callback=None) -> str:
        p = self._p(cmd.split(" ", 1)[1])
        with p.open("wb") as out:
            while block := f.read(blocksize):
                out.write(block)
                if callback:
                    callback(block)
        return "226 OK"

    def size(self, path: str) -> int:
        return self._p(path).stat().st_size

    def rename(self, a: str, b: str) -> str:
        os.replace(self._p(a), self._p(b))
        return "250 OK"

    def delete(self, path: str) -> str:
        p = self._p(path)
        if not p.is_file():
            raise ftplib.error_perm("550 No such file")
        p.unlink()
        self.deleted.append(path)
        return "250 OK"

    def quit(self) -> str:
        return "221 Bye"


def _stage(tmp_path: Path, version: str) -> Path:
    stage = tmp_path / f"stage-{version}"
    files = {"eboot.bin": b"eboot " + version.encode(), "sce_sys/param.json": b"{}", "spirv.nup": b"spirv",
             "romfs.nup": b"romfs " + version.encode() * 1000, "exefs/main": b"main " + version.encode(),
             "exefs/sdk": b"sdk"}
    if version == "1.8.0":
        files["exefs/subsdk1"] = b"only in 1.8.0"
    for rel, data in files.items():
        (stage / rel).parent.mkdir(parents=True, exist_ok=True)
        (stage / rel).write_bytes(data)
    return stage


def test_ftp_install_first_then_upgrade(tmp_path: Path) -> None:
    console = tmp_path / "console"
    (console / "data/homebrew").mkdir(parents=True)
    (console / "data/homebrew/keep.txt").write_bytes(b"not ours")
    deleted: list[str] = []
    opener = lambda host, port: FakeFtp(console, deleted)  # noqa: E731
    record = tmp_path / "work/ftp-sent.tsv"
    record.parent.mkdir()
    plans = []

    def send(stage: Path) -> tuple[int, int, int]:
        return send_ps5.send_directory_ftp("ps5", stage, cache_path=tmp_path / "work/cache.tsv",
                                           on_plan=lambda *a: plans.append(a), ftp_ports=(2121,), ftp_opener=opener,
                                           record_path=record)

    s180 = _stage(tmp_path, "1.8.0")
    title = console / "data/homebrew/PPSA99193"
    # a first install: the folder is made, every file goes, no .pitstop-ftp left
    assert send(s180)[:2] == (7, 0)
    for p in s180.rglob("*"):
        if p.is_file():
            assert (title / p.relative_to(s180)).read_bytes() == p.read_bytes()
    assert not list(title.rglob("*.pitstop-ftp"))
    assert plans[-1][2] == 0
    # nothing changed: nothing goes
    assert send(s180)[:2] == (0, 7)
    # another version: what changed goes, a module 1.0.0 lacks is removed by name, nothing else is deleted
    s100 = _stage(tmp_path, "1.0.0")
    sent, skipped, _ = send(s100)
    assert (sent, skipped) == (3, 3)   # eboot.bin, romfs.nup, exefs/main changed; sce_sys, spirv.nup, exefs/sdk did not
    assert deleted == ["/data/homebrew/PPSA99193/exefs/subsdk1"]
    assert (console / "data/homebrew/keep.txt").is_file()


def test_ftp_install_trusts_the_receivers_manifest(tmp_path: Path) -> None:
    """A file the receiver checked (its .pitstop-manifest.tsv: rel, size, mtime_ns, SHA-256) and unchanged since is not
    sent again; one whose time no longer matches is."""
    console = tmp_path / "console"
    title = console / "data/homebrew/PPSA99193"
    stage = _stage(tmp_path, "1.8.0")
    rows = []
    for rel in ("romfs.nup", "spirv.nup"):
        (title / rel).parent.mkdir(parents=True, exist_ok=True)
        (title / rel).write_bytes((stage / rel).read_bytes())
        st = (title / rel).stat()
        mtime = st.st_mtime_ns if rel == "romfs.nup" else st.st_mtime_ns - 60 * 10**9   # spirv.nup: changed since
        rows.append(f"{rel}\t{st.st_size}\t{mtime}\t{hashlib.sha256((stage / rel).read_bytes()).hexdigest()}\n")
    (title / ".pitstop-manifest.tsv").write_text("".join(rows), encoding="utf-8")
    deleted: list[str] = []
    sent, skipped, _ = send_ps5.send_directory_ftp("ps5", stage, ftp_ports=(2121,),
                                                   ftp_opener=lambda h, p: FakeFtp(console, deleted))
    assert (sent, skipped) == (6, 1)
    assert deleted == []


def test_ftp_install_needs_a_server(tmp_path: Path) -> None:
    def refuse(host: str, port: int):
        raise ConnectionRefusedError(111, "refused")

    with pytest.raises(send_ps5.TransferError, match="no Pit Stop receiver"):
        send_ps5.send_directory_ftp("ps5", _stage(tmp_path, "1.8.0"), ftp_ports=(2121, 1337), ftp_opener=refuse)


def test_ftp_install_size_mismatch_leaves_the_old_file(tmp_path: Path) -> None:
    console = tmp_path / "console"
    title = console / "data/homebrew/PPSA99193"
    title.mkdir(parents=True)
    (title / "eboot.bin").write_bytes(b"the old one")

    class Short(FakeFtp):
        def size(self, path: str) -> int:
            return super().size(path) - 1

    with pytest.raises(send_ps5.TransferError, match="arrived on the PS5"):
        send_ps5.send_directory_ftp("ps5", _stage(tmp_path, "1.8.0"), ftp_ports=(2121,),
                                    ftp_opener=lambda h, p: Short(console, []))
    assert (title / "eboot.bin").read_bytes() == b"the old one"
    assert not list(title.rglob("*.pitstop-ftp"))

