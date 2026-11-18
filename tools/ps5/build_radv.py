"""Rebuild the unpatched PS5_Mesa RADV archive libvulkan_radeon.ps5.a when missing or stale.

Run `python3 tools/ps5/build_radv.py --stack build/ps5-stack` directly, or let
`build_smoke.py` / `build_game.py` invoke `ensure_radv()` when the staged RADV
archive is missing or out of date.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def _provenance_info(provenance: Path) -> dict[str, str]:
    if not provenance.is_file():
        return {}
    return dict(line.split(": ", 1) for line in provenance.read_text().splitlines() if ": " in line)


def ensure_radv(vulkan: Path, mesa_revision: str, radv_sdk: str) -> tuple[Path, str]:
    radv = vulkan / ".deps/native/radv-release"
    archive = radv / "lib/libvulkan_radeon.ps5.a"
    provenance = radv / "PROVENANCE.txt"

    patches = sorted((Path(__file__).resolve().parent).glob("ps5_mesa-*.patch"))
    patch_digest: str | None = None
    if patches:
        import hashlib

        h = hashlib.sha256()
        for p in patches:
            h.update(p.name.encode("utf-8"))
            h.update(b"\0")
            h.update(p.read_bytes())
        patch_digest = h.hexdigest()

    info = _provenance_info(provenance)
    if (
        archive.is_file()
        and info.get("revision") == mesa_revision
        and info.get("sdk") == radv_sdk
        and info.get("assertions") == "off"
        and (info.get("patch sha256") == patch_digest if patch_digest else "patch sha256" not in info)
        and info.get("archive sha256") == sha256(archive)
    ):
        return archive, info["archive sha256"]

    work_src = vulkan / ".deps/work/radv-src"
    work_build = vulkan / ".deps/work/radv-build-ps5-release"
    env = dict(os.environ)
    env["PS5_MESA_REVISION"] = mesa_revision
    applied_marker = work_src / ".applied-patches"
    if work_src.is_dir() and applied_marker.is_file() and applied_marker.read_text().strip() != (patch_digest or ""):
        shutil.rmtree(work_src, ignore_errors=True)
        shutil.rmtree(work_build, ignore_errors=True)
    if not (work_src / ".revision").is_file():
        # build-radv.sh skips everything, the source tree included, while the installed archive's provenance names
        # this revision: without the tree the patches had nowhere to go (16/11/2026, two patches added).
        provenance.unlink(missing_ok=True)

    stack_meson = vulkan.parent / "host/python-tools/bin/meson"
    if stack_meson.is_file():
        env.setdefault("MESON", str(stack_meson))
    ran_build_sh = False
    if not (work_src / ".revision").is_file() or not (work_build / "build.ninja").is_file():
        subprocess.run(["bash", str(vulkan / "tools/build-radv.sh"), "release"], cwd=vulkan, env=env, check=True)
        ran_build_sh = True

    if patches and (not applied_marker.is_file() or applied_marker.read_text().strip() != patch_digest):
        for p in patches:
            already = subprocess.run(
                ["patch", "-d", str(work_src), "-p1", "-R", "--dry-run", "--silent", "-i", str(p)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if already.returncode != 0:
                subprocess.run(["patch", "-d", str(work_src), "-p1", "-N", "--batch", "-i", str(p)], check=True)
        applied_marker.write_text(f"{patch_digest}\n")
        ran_build_sh = False

    if not ran_build_sh:
        r = subprocess.run(["ninja", "-C", str(work_build), "src/amd/vulkan/libvulkan_radeon.a"], env=env)
        if r.returncode != 0:
            # Stale cross-file path from another checkout location: wipe and reconfigure via build-radv.sh.
            (radv / "PROVENANCE.txt").unlink(missing_ok=True)
            shutil.rmtree(work_build, ignore_errors=True)
            subprocess.run(["bash", str(vulkan / "tools/build-radv.sh"), "release"], cwd=vulkan, env=env, check=True)

    built = work_build / "src/amd/vulkan/libvulkan_radeon.a"
    archive.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, archive)
    digest = sha256(archive)
    sdk_rev = (vulkan / ".deps/native/ps5-payload-sdk/.ps5-sdk-revision").read_text().strip()
    prov_lines = [
        "RADV for the PlayStation 5, built by tools/ps5/build_radv.py",
        f"revision: {mesa_revision}",
        "assertions: off",
        f"sdk: {sdk_rev}",
    ]
    if patch_digest:
        prov_lines.append(f"patch sha256: {patch_digest}")
    prov_lines.append(f"archive sha256: {digest}")
    provenance.write_text("\n".join(prov_lines) + "\n")
    kind = "patched" if patch_digest else "unpatched"
    print(f"==> [radv] built {kind} {archive} (archive {digest[:12]})")
    return archive, digest


def main() -> None:
    from build_smoke import MESA, RADV_SDK

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stack", type=Path, required=True, help="directory containing the pinned PS5 checkouts")
    args = parser.parse_args()
    ensure_radv(args.stack.resolve() / "PS5_Vulkan", MESA, RADV_SDK)


if __name__ == "__main__":
    main()
