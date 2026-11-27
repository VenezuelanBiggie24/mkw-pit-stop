"""Unattended PS5 benchmark: launch the title with MKW_BENCH, wait for its result, kill it on a hang, keep the log.

Why: a benchmark run on the console needed hands on it (launch from the home screen, close the app, copy the log). The
titlectl payload (tools/ps5/titlectl.c, sent once to the ELF loader on port 9021) starts and stops the title, FTP
(port 2121) swaps mkw.ini and reads the log, and the runtime's MKW_BENCH (runtime/core/diag/bench.c) drives the race
and exits by itself at the end with "bench: result ..." in the log (a Grand Prix by default, its four races split
into segments where the game saves) and "bench: memresult ..." (runtime/core/diag/bench_mem.c: free and resident
memory, VRAM, page-table units, governor passes). --mode ko is the memory test: a Knockout Tour, rally after rally, for
MKW_BENCH_MAX seconds (2400 unless --set BENCH_MAX=...; --set BENCH_RALLIES=N ends it after N rallies).

One run: back up mkw.ini, append the benchmark keys, kill any running copy, launch, follow mkw.log over FTP (new bytes
only) until the result line or the process ends, kill it if the log stops growing for --stall seconds or the run
passes --timeout, save the log as build/run/ps5/<name>-<time>-mkw.log, put mkw.ini back. --runs N repeats it and
prints the results side by side.

Usage (python3 on this box, uv run elsewhere):
  uv run tools/ps5/bench.py ctl build                  # cross-compile build/ps5-titlectl/titlectl.elf
  uv run tools/ps5/bench.py ctl send HOST              # send it to the ELF loader (once per console boot)
  uv run tools/ps5/bench.py ctl HOST ping|status|launch|kill
  uv run tools/ps5/bench.py run HOST [--mode gp|tt|ko] [--runs N] [--name bench] [--set KEY=VALUE ...]
  uv run tools/ps5/bench.py run HOST --mode ko --set BENCH_MAX=3600   # an hour of Knockout Tour, memory report
"""

from __future__ import annotations

import argparse
import ftplib
import io
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TITLE = "PPSA99193"
REMOTE_DIR = f"/data/homebrew/{TITLE}"
REMOTE_INI = f"{REMOTE_DIR}/mkw.ini"
REMOTE_LOG = f"{REMOTE_DIR}/logs/mkw.log"
FTP_PORT = 2121
ELF_PORT = 9021
CTL_PORT = 9031  # titlectl.c TITLECTL_PORT
ELF = ROOT / "build/ps5-titlectl/titlectl.elf"
SDK = Path(os.environ.get("PS5_PAYLOAD_SDK", Path.home() / "ps5-payload-sdk/ps5-payload-sdk"))
HUNG = re.compile(r"hang watch: no present for (?:[3-9]\d|\d{3,})\.\d s \(frame \d+\)")
RESULT = re.compile(r"bench: result (.*)")
MEMRESULT = re.compile(r"bench: memresult (.*)")
DEFAULT_MAX = {"tt": 300, "gp": 1800, "ko": 2400}  # bench.c's MKW_BENCH_MAX defaults


# ---- titlectl
def ctl(host: str, command: str, timeout: float = 120) -> str:
    """One titlectl command; its answer line. A launch waits up to 45 s on the console (three tries of 15 s)."""
    with socket.create_connection((host, CTL_PORT), timeout=timeout) as s:
        s.sendall(command.encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(512)
            if not chunk:
                break
            data += chunk
    return data.decode(errors="replace").strip()


def build_payload() -> Path:
    clang = SDK / "bin/prospero-clang"
    if not clang.exists():
        sys.exit(f"no PS5 payload SDK at {SDK} (set PS5_PAYLOAD_SDK)")
    ELF.parent.mkdir(parents=True, exist_ok=True)
    src = ROOT / "tools/ps5/titlectl.c"
    subprocess.run([str(clang), "-Wall", "-Wextra", "-O2", "-o", str(ELF), str(src), "-lSceSystemService",
                    "-lSceUserService"], check=True)
    print(f"built {ELF} ({ELF.stat().st_size} bytes)")
    return ELF


def send_payload(host: str) -> None:
    if not ELF.exists():
        build_payload()
    version = re.search(rb"#define TITLECTL_VERSION (\d+)", (ROOT / "tools/ps5/titlectl.c").read_bytes()).group(1).decode()
    try:
        answer = ctl(host, "ping", timeout=3)
        if answer == f"ok titlectl {version}":
            print(answer, "(already running)")
            return
        print(answer, f"(older than {version}): replacing it:", ctl(host, "exit", timeout=3))
        time.sleep(1)
    except OSError:
        pass
    with socket.create_connection((host, ELF_PORT), timeout=10) as s:
        s.sendall(ELF.read_bytes())
    for _ in range(20):
        time.sleep(0.5)
        try:
            print(ctl(host, "ping", timeout=3))
            return
        except OSError:
            pass
    sys.exit(f"titlectl does not answer on {host}:{CTL_PORT} after sending it to the ELF loader")


# ---- FTP
def ftp_open(host: str) -> ftplib.FTP:
    f = ftplib.FTP()
    f.connect(host, FTP_PORT, timeout=30)
    f.login()
    f.set_pasv(True)
    return f


def ftp_get(f: ftplib.FTP, path: str, offset: int = 0) -> bytes:
    buf = io.BytesIO()
    f.retrbinary(f"RETR {path}", buf.write, rest=offset or None)
    return buf.getvalue()


def ftp_size(f: ftplib.FTP, path: str) -> int | None:
    try:
        f.voidcmd("TYPE I")
        return f.size(path)
    except ftplib.error_perm:
        return None


def ftp_put(f: ftplib.FTP, path: str, data: bytes) -> None:
    """The console's server refuses STOR over an existing file: write <path>.new, check its size, then swap."""
    tmp = path + ".new"
    try:
        f.delete(tmp)
    except ftplib.error_perm:
        pass
    f.storbinary(f"STOR {tmp}", io.BytesIO(data))
    if ftp_size(f, tmp) != len(data):
        sys.exit(f"upload of {tmp} came out the wrong size")
    try:
        f.delete(path)
    except ftplib.error_perm:
        pass
    f.rename(tmp, path)


# ---- one run
def bench_ini(original: bytes, mode: str, sets: list[str]) -> bytes:
    lines = ["", "# tools/ps5/bench.py (removed when the run ends; later lines win)", f"BENCH = {mode}"]
    for kv in sets:
        key, _, value = kv.partition("=")
        lines.append(f"{key.strip().removeprefix('MKW_')} = {value.strip()}")
    return original.rstrip(b"\n") + ("\n".join(lines) + "\n").encode()


def run_once(host: str, name: str, mode: str, sets: list[str], timeout: float, stall: float) -> dict:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = ROOT / f"build/run/ps5/{name}-{stamp}-mkw.log"
    out.parent.mkdir(parents=True, exist_ok=True)
    f = ftp_open(host)
    try:
        original = ftp_get(f, REMOTE_INI)
    except ftplib.error_perm:
        original = b""
    (out.parent / f"{name}-{stamp}-mkw.ini.orig").write_bytes(original)
    print(f"[{name}] kill: {ctl(host, f'kill {TITLE}')}")
    old_head = ftp_get(f, REMOTE_LOG)[:256] if ftp_size(f, REMOTE_LOG) else b""
    ftp_put(f, REMOTE_INI, bench_ini(original, mode, sets))
    f.quit()
    result: dict = {"name": name, "log": str(out)}
    try:
        answer = ctl(host, f"launch {TITLE}")
        print(f"[{name}] launch: {answer}")
        if not answer.startswith("ok"):
            result["error"] = answer
            return result
        t0 = time.time()
        log, last_growth, ended = b"", time.time(), None
        f = ftp_open(host)
        while True:
            time.sleep(5)
            size = ftp_size(f, REMOTE_LOG) or 0
            if not log and size:   # the old log until the new process rotates it
                head = ftp_get(f, REMOTE_LOG)[:256]
                if head == old_head and time.time() - t0 < 60:
                    continue
            if size > len(log):
                new = ftp_get(f, REMOTE_LOG, len(log))
                log += new
                last_growth = time.time()
                for line in new.decode(errors="replace").splitlines():
                    if "bench:" in line:
                        print(f"[{name}] {line.strip()}")
            m = RESULT.search(log.decode(errors="replace"))
            alive = ctl(host, f"status {TITLE}").startswith("ok running")
            if m or not alive:
                ended = "result" if m else "exited without a result"
                if alive:
                    time.sleep(5)   # the game exits by itself right after the result line
                break
            if time.time() - last_growth > stall:
                ended = f"log silent for {stall:.0f} s"
                break
            # A hung title can keep writing (the governor, the hang watch itself): the hang watch's first report of
            # 30 s or more without a present ends it before the console goes down with it (ucmp, 21/11/2026).
            hung = HUNG.search(log.decode(errors="replace"))
            if hung:
                ended = f"hung: {hung.group(0)}"
                break
            if time.time() - t0 > timeout:
                ended = f"timeout {timeout:.0f} s"
                break
        f.quit()
        result["ended"] = ended
        if ended != "result":
            print(f"[{name}] {ended}: kill: {ctl(host, f'kill {TITLE}')}")
        f = ftp_open(host)
        log = ftp_get(f, REMOTE_LOG)   # whole file: what the game wrote after the last poll too
        f.quit()
        out.write_bytes(log)
        m = RESULT.search(log.decode(errors="replace"))
        if m:
            result.update(kv.split("=", 1) for kv in m.group(1).split())
        m = MEMRESULT.search(log.decode(errors="replace"))
        if m:
            result.update(("mem_" + k, v) for k, v in (kv.split("=", 1) for kv in m.group(1).split()))
        return result
    finally:
        f = ftp_open(host)
        ftp_put(f, REMOTE_INI, original)
        f.quit()
        print(f"[{name}] mkw.ini restored; log {out}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("ctl", help="build or send the titlectl payload, or send it a command")
    c.add_argument("args", nargs="+", help="build | send HOST | HOST ping|status|launch|kill")
    r = sub.add_parser("run", help="benchmark runs")
    r.add_argument("host")
    r.add_argument("--runs", type=int, default=1)
    r.add_argument("--name", default="bench")
    r.add_argument("--mode", choices=("tt", "gp", "ko"), default="gp",
                   help="time trial, Grand Prix or Knockout Tour, the memory test (MKW_BENCH)")
    r.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="extra mkw.ini key for the run")
    r.add_argument("--timeout", type=float,
                   help="seconds from launch before it is killed (default: the run's MKW_BENCH_MAX + 600)")
    r.add_argument("--stall", type=float, default=120, help="seconds without new log lines before it is killed")
    a = ap.parse_args()
    if a.cmd == "ctl":
        if a.args == ["build"]:
            build_payload()
        elif a.args[0] == "send" and len(a.args) == 2:
            send_payload(a.args[1])
        elif len(a.args) == 2 and a.args[1] in ("ping", "status", "launch", "kill"):
            print(ctl(a.args[0], a.args[1] if a.args[1] == "ping" else f"{a.args[1]} {TITLE}"))
        else:
            ap.error("ctl takes: build | send HOST | HOST ping|status|launch|kill")
        return 0
    bench_max = DEFAULT_MAX[a.mode]
    for kv in a.set:
        key, _, value = kv.partition("=")
        if key.strip().removeprefix("MKW_").upper() == "BENCH_MAX" and float(value or 0) > 0:
            bench_max = float(value)
    timeout = a.timeout or bench_max + 600   # the menus before the window, and the exit
    results = [run_once(a.host, f"{a.name}{i + 1 if a.runs > 1 else ''}", a.mode, a.set, timeout, a.stall)
               for i in range(a.runs)]
    keys = ["fps", "avg_ms", "low1", "low01", "worst_ms", "frames", "secs", "finished"]
    print("\nrun        ended                      " + " ".join(f"{k:>9}" for k in keys))
    for res in results:
        print(f"{res['name']:<10} {res.get('ended', res.get('error', '?'))[:26]:<26} "
              + " ".join(f"{res.get(k, '-'):>9}" for k in keys))
    mem = ["free_min", "free_trend", "resident_max", "resident_trend", "vram_max", "pt_cpu_free_min", "gov_hard",
           "relieves", "stg_deferred"]
    if any("mem_samples" in res for res in results):
        print("\nmemory, MiB (trend: MiB/min; pt: page-table units)")
        print("run        " + " ".join(f"{k:>15}" for k in mem))
        for res in results:
            print(f"{res['name']:<10} " + " ".join(f"{res.get('mem_' + k, '-'):>15}" for k in mem))
    return 0


if __name__ == "__main__":
    sys.exit(main())
