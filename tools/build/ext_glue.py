"""Extensions linked into the runtime (runtime/core/ext, docs/project/extensions.md): the sources of each --ext folder
and the table defaults.c gives runtime/core/ext/ext.c.

Why: every extension defines the same entry point, mkw_ext_init, so that one source builds both as a library (MKW_EXT)
and linked in. Linked in, each folder's objects are compiled with -Dmkw_ext_init=mkw_ext_init__<name> and the table
lists those names. Used by tools/build/build.py and tools/ps5/build_game.py; not run on its own.
"""

import re
from pathlib import Path


def ext_name(folder: Path) -> str:
    """A C identifier from the folder's name, unique per folder name."""
    return re.sub(r"\W", "_", folder.resolve().name)


def ext_sources(folders: list[Path]) -> list[tuple[Path, str]]:
    """(source, -D flag) for every .c and .cpp of each folder, recursively."""
    out = []
    names = set()
    for folder in folders:
        if not folder.is_dir():
            raise SystemExit(f"--ext {folder}: not a folder")
        name = ext_name(folder)
        if name in names:
            raise SystemExit(f"--ext {folder}: another extension folder has the name {name}")
        names.add(name)
        srcs = sorted(p for pattern in ("*.c", "*.cpp") for p in folder.rglob(pattern))
        if not srcs:
            raise SystemExit(f"--ext {folder}: no .c or .cpp files")
        out += [(src, f"-Dmkw_ext_init=mkw_ext_init__{name}") for src in srcs]
    return out


def ext_table(folders: list[Path]) -> str:
    """The C lines of the table ext.c reads (rt_ext_linked*), empty when no folder is given."""
    names = [ext_name(f) for f in folders]
    lines = ['#include "mkw_ext.h"']
    lines += [f"int mkw_ext_init__{n}(const MkwExtApi *api);" for n in names]
    inits = ", ".join(f"mkw_ext_init__{n}" for n in names) or "0"
    labels = ", ".join(f'"{n}"' for n in names) or "0"
    lines += [
        f"const MkwExtInit rt_ext_linked[] = {{{inits}}};",
        f"const char *const rt_ext_linked_names[] = {{{labels}}};",
        f"const int rt_ext_linked_count = {len(names)};",
        "",
    ]
    return "\n".join(lines)
