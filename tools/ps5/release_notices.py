#!/usr/bin/env python3
"""The licence notices and the corresponding source of a PS5 release (release_ps5.py).

Why: every PS5 eboot links GPL-3.0-or-later code: the payload SDK fork's platform layer and headers (PS5_PayloadSDK)
and PS5_Vulkan's C runtime start-up, link recipe, AGC import stubs and generated libc.prx. The placeholder title and
the builder's ps5-bundle/ are therefore distributed under GPL-3.0-or-later as a whole, with the licence texts of every
part and their complete source (GPL-3.0 section 6). mkw-recomp's own code stays MIT. PS5_Vulkan's own
ps5/tools/stage-notices.py lists the template's parts but not mkw-recomp, the static libraries the eboot links (opus,
libvpx, zstd) or zlib in ps5-native-tool, and needs a PS5_LLVM checkout for LLVM's text: this lists them all.

  stage(dest)          writes dest/LEGAL.txt and dest/licenses/ (README.txt, components.json, each part's texts)
  source_archives(out) writes out/source/<part>-<rev>.tar.gz for each part whose source the release must carry, with
                       out/source/SOURCES.txt; a checkout with uncommitted changes is archived as its working tree
                       (git stash create: a commit of the tracked files, the checkout left untouched) and marked so

Run by release_ps5.py; alone, `python3 tools/ps5/release_notices.py DEST` stages the notices into DEST.
"""

from __future__ import annotations

import gzip
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "build/ps5-game"
STACK = ROOT / "build/ps5-stack"
DEPS = STACK / "deps"
HERE = Path(__file__).resolve().parent


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


def checkout(repo: Path) -> dict:
    """A git checkout's revision, its public remote and whether its tracked files differ from it."""
    rev = git(repo, "rev-parse", "HEAD")
    dirty = bool(git(repo, "status", "--porcelain", "--untracked-files=no"))
    remotes = git(repo, "remote").split()
    remote = git(repo, "remote", "get-url", "origin" if "origin" in remotes else remotes[0]) if remotes else ""
    return dict(kind="git", path=str(repo), remote=remote.removesuffix(".git"), revision=rev, dirty=dirty)


def parts() -> list[dict]:
    """Every part of the PS5 release: licence, copyright, where it is, its texts and its source."""
    title = WORK / "title"
    template = STACK / "PS5_VulkanTemplate"
    vulkan, mesa, sdk = STACK / "PS5_Vulkan", STACK / "PS5_Mesa", STACK / "PS5_PayloadSDK"
    zlib = vulkan / ".deps/native/zlib/zlib-1.3.2"
    own = checkout(ROOT)
    own["remote"] = "this release (source/)"   # the fork's public address is the release page itself
    t = checkout(title)
    t["based_on"] = dict(remote="https://github.com/mihawk-99/PS5_VulkanTemplate", revision=git(template, "rev-parse", "HEAD"))
    return [
        dict(id="mkw-recomp", name="mkw-recomp: the runtime (kernel, SDK replacements, NVN2 over Vulkan), the PS5 "
             "platform layer, the Pit Stop overlay and the PC builder (recomp/, sm86/, tools/)",
             licence="MIT", copyright=["Copyright (c) 2026 Adrià and the mkw-recomp contributors"],
             artifacts=["eboot.bin (loader-inventory.o)", "setup/src/"], texts=[("git", ROOT, "LICENSE", "LICENSE")],
             source=own, archive=True),
        dict(id="title", name="the PS5 Vulkan Template title foundation (Sascha Willems' Vulkan examples base, the PS5 "
             "layer) with Dear ImGui, libktx, tinygltf, volk, the Vulkan headers and glm",
             licence="MIT; Apache-2.0 (libktx); Apache-2.0 OR MIT (the Vulkan headers)",
             copyright=["Copyright (c) 2016-2026 Sascha Willems", "Copyright (C) 2026 Mihawk", "and each library's authors"],
             artifacts=["eboot.bin", "sce_sys/"],
             texts=[("git", title, "LICENSE.md", "LICENSE.md"),
                    ("git", title, "external/imgui/LICENSE.txt", "imgui-LICENSE.txt"),
                    ("git", title, "external/ktx/LICENSE.md", "ktx-LICENSE.md"),
                    ("git", title, "external/ktx/NOTICE.md", "ktx-NOTICE.md"),
                    ("git", title, "external/tinygltf/LICENSE", "tinygltf-LICENSE"),
                    ("git", title, "ps5/third_party/volk/LICENSE.md", "volk-LICENSE.md")],
             source=t, archive=True),
        dict(id="platform", name="the PS5 platform layer (libps5platform.a, crt), the SDK headers and the system "
             "library stubs (sdk_lib/*.so), from the PS5 payload SDK fork, with musl's regex and dlmalloc",
             licence="GPL-3.0-or-later; musl's regex MIT; dlmalloc public domain",
             copyright=["Copyright (C) John Törnblom and the ps5-payload-dev contributors", "Copyright (C) 2026 Mihawk"],
             artifacts=["eboot.bin", "ps5-bundle/lib/libps5platform.a", "ps5-bundle/sdk_lib/", "ps5-bundle/sdk_include/"],
             texts=[("git", sdk, "LICENSE", "GPL-3.0.txt"),
                    ("git", sdk, "platform/src/regex/COPYRIGHT.musl", "musl-regex-COPYRIGHT.txt"),
                    ("git", sdk, "platform/src/dlmalloc/SOURCE", "dlmalloc-SOURCE.txt")],
             source=checkout(sdk) | {"upstream": "https://github.com/ps5-payload-dev/sdk/releases/tag/v0.42"},
             archive=True),
        dict(id="ps5-vulkan", name="PS5_Vulkan: the C runtime start-up (app_crt.o), the link recipe (ld/), the AGC "
             "import stubs, ps5-native-tool and the generated sce_module/libc.prx",
             licence="GPL-3.0-or-later", copyright=["Copyright (C) 2026 Mihawk", "Copyright (C) 2026 BlackBearReloaded "
                                                    "(libc.prx generator, ps5-native-app-boilerplate)"],
             artifacts=["eboot.bin", "sce_module/libc.prx", "ps5-bundle/lib/", "ps5-bundle/ld/", "ps5-bundle/bin/"],
             texts=[("git", vulkan, "LICENSE", "GPL-3.0.txt"), ("git", vulkan, "NOTICE.md", "NOTICE.md")],
             source=checkout(vulkan), archive=True),
        dict(id="radv", name="RADV, Mesa's Vulkan driver (with ACO, NIR and Mesa's Vulkan runtime), from the PS5_Mesa fork",
             licence="MIT, with other licences stated per file (Mesa-license.rst)",
             copyright=["Copyright the Mesa contributors (per-file notices in the source)"],
             artifacts=["eboot.bin", "ps5-bundle/lib/libvulkan_radeon.ps5.a"],
             texts=[("git", mesa, "docs/license.rst", "Mesa-license.rst"), ("tree", mesa, "licenses", "licenses")],
             source=checkout(mesa), archive=True),
        dict(id="llvm-runtime", name="LLVM libc++, libc++abi, libunwind and compiler-rt builtins (linked into eboot.bin)",
             licence="Apache-2.0 WITH LLVM-exception",
             copyright=["Copyright (c) 2003-2026 University of Illinois at Urbana-Champaign and the LLVM project contributors"],
             artifacts=["eboot.bin", "ps5-bundle/lib/libc++*.a, libunwind.a, libclang_rt.builtins-x86_64.a"],
             texts=[("file", HERE / "licenses/llvm-LICENSE.TXT", None, "LICENSE.TXT")],
             source=dict(kind="fixed", revision="LLVM 21 as shipped in the PS5 payload SDK v0.42",
                         remote="https://github.com/llvm/llvm-project"), archive=False),
        dict(id="opus", name="Opus 1.5.2 (libopus.a)", licence="BSD-3-Clause", copyright=["Copyright the Opus authors "
             "(Xiph.Org, Skype, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, "
             "Erik de Castro Lopo, Mozilla, Amazon)"], artifacts=["eboot.bin", "ps5-bundle/lib/libopus.a"],
             texts=[("file", DEPS / "src/opus/COPYING", None, "COPYING")],
             source=dict(kind="fixed", revision="v1.5.2", remote="https://gitlab.xiph.org/xiph/opus"), archive=False),
        dict(id="libvpx", name="libvpx 1.14.1 (libvpx.a)", licence="BSD-3-Clause, with the WebM patent grant",
             copyright=["Copyright (c) 2010, The WebM Project authors"], artifacts=["eboot.bin", "ps5-bundle/lib/libvpx.a"],
             texts=[("file", DEPS / "src/libvpx/LICENSE", None, "LICENSE"), ("file", DEPS / "src/libvpx/PATENTS", None, "PATENTS")],
             source=dict(kind="fixed", revision="v1.14.1", remote="https://chromium.googlesource.com/webm/libvpx"),
             archive=False),
        dict(id="zstd", name="Zstandard 1.5.7 (libzstd.a)", licence="BSD-3-Clause (dual-licensed with GPL-2.0; taken under BSD)",
             copyright=["Copyright (c) Meta Platforms, Inc. and affiliates"], artifacts=["eboot.bin", "ps5-bundle/lib/libzstd.a"],
             texts=[("file", DEPS / "zstd-1.5.7/LICENSE", None, "LICENSE")],
             source=dict(kind="fixed", revision="v1.5.7", remote="https://github.com/facebook/zstd"), archive=False),
        dict(id="zlib", name="zlib 1.3.2 (in ps5-native-tool)", licence="Zlib",
             copyright=["Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler"], artifacts=["ps5-bundle/bin/"],
             texts=[("file", zlib / "LICENSE", None, "LICENSE")],
             source=dict(kind="fixed", revision="1.3.2", remote="https://zlib.net"), archive=False),
    ]


LEGAL = """Mario Kart World: Pit Stop - licences and legal use
===================================================

This release contains no game: no game code, no game files, no console firmware and no keys. It builds the PS5
title from your own dump of Mario Kart World, on your own PC. Do not share the title folder it builds: it holds the
game's code and files.

Licences: mkw-recomp's own code is MIT. The PS5 title links the PS5 payload SDK fork's platform layer and PS5_Vulkan's
start-up code, which are GPL-3.0-or-later, so the placeholder title, the ps5-bundle/ and every title built with them
are under GPL-3.0-or-later as a whole (licenses/platform/GPL-3.0.txt). The complete source of every GPL part, and of
mkw-recomp and the title foundation, is attached to the same GitHub release (source/, listed in SOURCES.txt). Every
part, its licence, copyright and source is listed in licenses/README.txt.

Mario Kart World is (C) Nintendo. Not affiliated with or endorsed by Nintendo or Sony Interactive Entertainment.
"PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc. Vulkan is a registered trademark of the
Khronos Group Inc.; the RADV port is not a conformant product.
"""


def _write_text(spec: tuple, dest: Path) -> None:
    kind, a, b, _ = spec
    dest.parent.mkdir(parents=True, exist_ok=True)
    if kind == "file":
        shutil.copy2(a, dest)
    elif kind == "git":
        dest.write_bytes(subprocess.run(["git", "-C", str(a), "show", f"HEAD:{b}"], capture_output=True, check=True).stdout)
    else:   # a folder of a repository at HEAD
        dest.mkdir(parents=True, exist_ok=True)
        tar = subprocess.run(["git", "-C", str(a), "archive", "HEAD", b], capture_output=True, check=True).stdout
        subprocess.run(["tar", "-x", "-C", str(dest), "--strip-components", str(len(Path(b).parts))], input=tar, check=True)


def _where(src: dict) -> str:
    if src["kind"] != "git":
        return f'{src["remote"]} at {src["revision"]}'
    s = f'{src["remote"] or "(local)"} at {src["revision"]}' + (" with local changes" if src.get("dirty") else "")
    if "based_on" in src:
        s += f' (based on {src["based_on"]["remote"]} at {src["based_on"]["revision"]})'
    return s


def stage(dest: Path, ps: list[dict] | None = None) -> None:
    """LEGAL.txt and licenses/ into dest (a title folder, a ps5-bundle/ or a builder package's root)."""
    ps = ps or parts()
    out = dest / "licenses"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    lines = [LEGAL, "The parts", "=========", ""]
    for p in ps:
        for spec in p["texts"]:
            _write_text(spec, out / p["id"] / spec[3])
        src = "source/" + archive_name(p) if p["archive"] else _where(p["source"])
        lines += [p["name"], f'  licence:   {p["licence"]}', f'  copyright: {"; ".join(p["copyright"])}',
                  f'  in:        {", ".join(p["artifacts"])}', f'  texts:     licenses/{p["id"]}/',
                  f'  source:    {src} ({_where(p["source"])})' if p["archive"] else f"  source:    {src}", ""]
    (out / "README.txt").write_text("\n".join(lines), encoding="utf-8")
    data = [{k: v for k, v in p.items() if k != "texts"} for p in ps]
    (out / "components.json").write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    (dest / "LEGAL.txt").write_text(LEGAL, encoding="utf-8")


def archive_name(p: dict) -> str:
    return f'{p["id"]}-{p["source"]["revision"][:12]}{"-modified" if p["source"].get("dirty") else ""}.tar.gz'


def source_archives(out: Path, ps: list[dict] | None = None) -> list[Path]:
    """The source of every part the release must carry, as out/source/*.tar.gz, with SOURCES.txt."""
    ps = ps or parts()
    src_dir = out / "source"
    shutil.rmtree(src_dir, ignore_errors=True)
    src_dir.mkdir(parents=True)
    made, lines = [], ["The source of this release's parts (licenses/README.txt says which licence each is under).", ""]
    for p in ps:
        if not p["archive"]:
            lines.append(f'{p["id"]}: {_where(p["source"])}')
            continue
        repo, rev = Path(p["source"]["path"]), p["source"]["revision"]
        # a checkout with local changes: its tracked working tree as a commit object, the checkout left as it is
        tree = git(repo, "stash", "create") if p["source"].get("dirty") else rev
        name = archive_name(p)
        tar = subprocess.run(["git", "-C", str(repo), "archive", "--format=tar", f"--prefix={p['id']}/", tree],
                             capture_output=True, check=True).stdout
        with gzip.GzipFile(src_dir / name, "wb", mtime=0) as gz:
            gz.write(tar)
        made.append(src_dir / name)
        lines.append(f'{p["id"]}: {name} ({_where(p["source"])})')
    (src_dir / "SOURCES.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return made + [src_dir / "SOURCES.txt"]


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    stage(Path(sys.argv[1]))
