#!/usr/bin/env python3
"""Write a folder's files as one NUP v1 package, stored raw, for the PS5 title to read the RomFS from in place.

Why: the RomFS is ~206,000 files (23 GB). Sent one by one to the PS5, each costs a create, a checksum and a rename
there, and a USB copy of so many small files is slow too. The runtime reads a .nup package's RomFS in place
(runtime/sdk/fs/nup.c, pack.c, romfs.c: a raw file is read at its offset, never unpacked), so the PC builder writes
romfs.nup and the PS5 gets one file. Files are stored raw (codec 0): a zstd frame (codec 1) is decoded whole on every
read, a cost in memory the PS5 cannot spare (its long sessions already run short).

Format: NUP v1 as runtime/sdk/fs/nup.c and tools/ps5/nup_reader.py read it (the MIT specification at
https://github.com/a55uka/nup, SPEC.md, written from scratch): a 128-byte superblock, the table of contents (one
base package whose exefs/ directory is empty and whose romfs/ directory is the folder), then the files' bytes, each
16-byte aligned. The table of contents is sized first and written last, once every file's SHA-256 is known, so each
file is read once.

Usage: python3 tools/ps5/nup_writer.py SRC_DIR OUT.nup [--title-id HEX] [--version N]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import uuid
from pathlib import Path
from typing import Callable

HEADER = struct.Struct("<4sHHQQ32sQQII16s32s")   # 0x80
TOC_HEAD = struct.Struct("<4sIQQQQQ")            # 0x30
PKG = struct.Struct("<QIIIIBBHIQI20s")          # 0x40
DIR = struct.Struct("<IIIIQ")                    # 0x18
FILE = struct.Struct("<IIQQQ32sBBH12s")         # 0x50
CHUNK = 1 << 20   # 8 MiB reads failed in WSL's 9p client for a Windows drive (an order-4 kernel allocation, 18/11/2026)
MKW_TITLE_ID = 0x0100152000022000   # informational only: the readers use the last package whatever its id


def _align(n: int, a: int = 16) -> int:
    return (n + a - 1) // a * a


def write_nup(src: Path, out: Path, title_id: int = MKW_TITLE_ID, version: int = 0,
              on_progress: Callable[[int, int], None] | None = None) -> int:  # fmt: skip
    """Package every regular file under src as the romfs/ of one base package in out; the number of files.
    on_progress(bytes_done, bytes_total) while the data is copied."""
    # The tree, depth first, each directory's children contiguous in the child list (dirs, then files, by name).
    dirs: list[list] = []      # [name, first_child, child_count]
    files: list[list] = []     # [name, size, offset, sha256, path]
    children: list[int] = []
    names = bytearray()
    name_at: dict[str, tuple[int, int]] = {}

    def name(s: str) -> tuple[int, int]:
        if s not in name_at:
            b = s.encode("utf-8")
            name_at[s] = (len(names), len(b))
            names.extend(b)
        return name_at[s]

    pending: list[tuple[int, Path]] = []   # (dir index, folder) whose children are still to list

    def new_dir(nm: str) -> int:
        dirs.append([nm, 0, 0])
        return len(dirs) - 1

    root, exefs, romfs = new_dir(""), new_dir("exefs"), new_dir("romfs")
    # root's children: exefs and romfs (slots 0-1); exefs stays empty: the title loads exefs/ from its folder
    dirs[root][1:] = [0, 2]
    children += [exefs, romfs]   # dir indices (< dir count)
    pending.append((romfs, src))
    file_nodes: list[tuple[int, int]] = []   # (slot in children, file index): fixed up once the dir count is known
    while pending:
        d, folder = pending.pop(0)
        entries = sorted(os.scandir(folder), key=lambda e: e.name)
        subs = [e for e in entries if e.is_dir(follow_symlinks=False)]
        regs = [e for e in entries if e.is_file(follow_symlinks=False)]
        dirs[d][1], dirs[d][2] = len(children), len(subs) + len(regs)
        for e in subs:
            i = new_dir(e.name)
            children.append(i)
            pending.append((i, Path(e.path)))
        for e in regs:
            file_nodes.append((len(children), len(files)))
            children.append(-1)
            files.append([e.name, e.stat().st_size, 0, b"", Path(e.path)])
    for slot, fi in file_nodes:
        children[slot] = len(dirs) + fi
    for d in dirs:
        if d[0]:
            name(d[0])
    for f in files:
        name(f[0])

    toc_size = TOC_HEAD.size + PKG.size + len(dirs) * DIR.size + len(files) * FILE.size + 4 * len(children)
    str_off = toc_size
    toc_size += len(names)
    blob = _align(HEADER.size + toc_size)
    total = sum(f[1] for f in files)
    done = 0
    tmp = out.with_name(out.name + ".part")
    with tmp.open("wb") as fo:
        fo.truncate(blob)
        fo.seek(blob)
        pos = blob
        for f in files:   # the data, in the order of the table: each file read once, hashed as it is copied
            h = hashlib.sha256()
            if f[1]:
                f[2] = pos
                with f[4].open("rb") as fi:
                    while chunk := fi.read(CHUNK):
                        h.update(chunk)
                        fo.write(chunk)
                        done += len(chunk)
                        if on_progress:
                            on_progress(done, total)
                pad = _align(pos + f[1]) - (pos + f[1])
                fo.write(b"\0" * pad)
                pos += f[1] + pad
            f[3] = h.digest()
        file_size = pos
        toc = bytearray(TOC_HEAD.pack(b"TOC\x01", 1, len(dirs), len(files), str_off, len(names), 0))
        toc += PKG.pack(title_id, root, exefs, romfs, version, 0, 0, 0, 0, 0, 0, b"")
        for nm, first, count in dirs:
            off, ln = name(nm) if nm else (0, 0)
            toc += DIR.pack(off, ln, first, count, 0)
        for nm, size, offset, sha, _ in files:
            off, ln = name(nm)
            toc += FILE.pack(off, ln, size, offset, size, sha, 0, 0, 0, b"")
        toc += struct.pack(f"<{len(children)}I", *children)
        toc += names
        assert len(toc) == toc_size
        fo.seek(0)
        fo.write(HEADER.pack(b"NUP\x01", HEADER.size, 1, HEADER.size, toc_size, hashlib.sha256(toc).digest(), blob,
                             file_size, 1, 0, uuid.uuid4().bytes, b""))  # fmt: skip
        fo.write(toc)
        fo.flush()
        os.fsync(fo.fileno())
    os.replace(tmp, out)
    return len(files)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--title-id", type=lambda s: int(s, 16), default=MKW_TITLE_ID)
    ap.add_argument("--version", type=int, default=0)
    args = ap.parse_args()
    n = write_nup(args.src, args.out, args.title_id, args.version)
    print(f"{args.out}: {n} files", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
