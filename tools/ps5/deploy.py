"""Upload the built PS5 smoke title with FTP staging and SHA-256 verification.

Use `python3 tools/ps5/deploy.py CONSOLE [app] [--query-memory | --dualsense | --alias-subrange] [--dry-run]`.
Set PS5_FTP_PORT for a console using a port other than 2121. After publishing, the param.json of a title that
ShadowMountPlus has registered is copied over its copy in /user/app/<title>/sce_sys, which the launcher reads.
"""

import argparse
import ftplib
import hashlib
import json
import os
import sys
from pathlib import Path
from uuid import uuid4

from build_game import TITLE_ID as GAME_TITLE_ID, WORK as GAME_WORK
from build_smoke import ALIAS_WORK, DUALSENSE_WORK, PAGE_TABLE_WORK, QUERY_WORK, TITLE_ID, WORK

HOMEBREW = "/data/homebrew"
REGISTERED_APPS = "/user/app"  # ShadowMountPlus's copies of registered titles (refresh_registered_param)
DESTINATION = f"{HOMEBREW}/{TITLE_ID}"
TEMP_PREFIX = f"{HOMEBREW}/.mkw-smoke-{TITLE_ID}"
PUBLISH_LOCK = f"{TEMP_PREFIX}-publish"
TRANSFER_BLOCK = 256 * 1024
FTP_REJECTED = (ftplib.error_perm, ftplib.error_temp)
FTP_INTERRUPTED = ftplib.all_errors + (KeyboardInterrupt,)


class UncertainPublish(RuntimeError):
    """Keep the old backup and staging paths when promotion or rollback is unconfirmed."""


PUBLISH_INTERRUPTED = FTP_INTERRUPTED + (UncertainPublish,)
STAGE_REJECTED = FTP_REJECTED + (ValueError,)
DEPLOY_ERRORS = ftplib.all_errors + (ValueError, UncertainPublish)


def package_paths(source: Path, title_id: str = TITLE_ID) -> tuple[list[Path], list[Path]]:
    if source.name != title_id:
        raise ValueError(f"expected title folder {title_id}: {source}")
    if not source.is_dir() or source.is_symlink():
        raise ValueError(f"build the title first: {source}")
    files, folders = [], []
    for entry in sorted(source.rglob("*")):
        if entry.is_symlink() or "\r" in entry.name or "\n" in entry.name:
            raise ValueError(f"unsupported package entry: {entry}")
        relative = entry.relative_to(source)
        if entry.is_dir():
            folders.append(relative)
        elif entry.is_file():
            files.append(relative)
        else:
            raise ValueError(f"unsupported package entry: {entry}")
    required = [Path(name) for name in ("eboot.bin", "sce_sys/param.json", "sce_sys/icon0.png", "sce_module/libc.prx")]
    if any(path not in files or not (source / path).stat().st_size for path in required):
        raise ValueError(f"incomplete package: {source}")
    param = json.loads((source / "sce_sys/param.json").read_text(encoding="utf-8"))
    if not isinstance(param, dict) or param.get("titleId") != title_id:
        raise ValueError(f"expected title ID {title_id} in {source / 'sce_sys/param.json'}")
    return files, folders


def upload_file(ftp: ftplib.FTP, local: Path, remote: str) -> None:
    expected = hashlib.sha256()
    with local.open("rb") as file:
        while block := file.read(TRANSFER_BLOCK):
            expected.update(block)
        file.seek(0)
        ftp.storbinary(f"STOR {remote}", file, blocksize=TRANSFER_BLOCK)
    received = hashlib.sha256()
    ftp.retrbinary(f"RETR {remote}", received.update, blocksize=TRANSFER_BLOCK)
    if expected.digest() != received.digest():
        raise ValueError(f"SHA-256 mismatch: {remote}")


def folder_exists(ftp: ftplib.FTP, path: str) -> bool:
    try:
        ftp.cwd(path)
    except ftplib.error_perm as error:
        if str(error).startswith("550"):
            return False
        raise
    ftp.cwd(HOMEBREW)
    return True


def remove_publish_lock(ftp: ftplib.FTP) -> None:
    try:
        ftp.rmd(PUBLISH_LOCK)
    except ftplib.all_errors as error:
        print(f"deploy: could not remove publish lock {PUBLISH_LOCK}: {error}", file=sys.stderr)


def restore(ftp: ftplib.FTP, backup: str) -> None:
    try:
        if folder_exists(ftp, DESTINATION):
            raise UncertainPublish("destination is occupied after the rejected promotion")
        ftp.rename(backup, DESTINATION)
    except ftplib.all_errors as error:
        raise UncertainPublish(f"rollback could not be confirmed: {error}") from error
    print(f"Restored previous title at {DESTINATION}", flush=True)


def publish(ftp: ftplib.FTP, stage: str, backup: str) -> None:
    # Hold the lock only across publication so separate uploads can stage in parallel.
    try:
        ftp.mkd(PUBLISH_LOCK)
    except FTP_REJECTED as error:
        raise ValueError(f"could not acquire publish lock {PUBLISH_LOCK}: {error}") from error
    except FTP_INTERRUPTED as error:
        raise UncertainPublish(f"could not confirm {PUBLISH_LOCK} ({error}). Check it and {stage}") from error
    try:
        had_previous = folder_exists(ftp, DESTINATION)
        if had_previous:
            print(f"Saving previous title as {backup}", flush=True)
            ftp.rename(DESTINATION, backup)
        try:
            ftp.rename(stage, DESTINATION)
        except FTP_REJECTED:
            if had_previous:
                restore(ftp, backup)
            raise
    except FTP_REJECTED:
        remove_publish_lock(ftp)
        raise
    except PUBLISH_INTERRUPTED as error:
        raise UncertainPublish(
            f"publication state needs inspection ({error}). Check {DESTINATION}, {backup}, and {stage} "
            f"before removing {PUBLISH_LOCK}"
        ) from error
    remove_publish_lock(ftp)
    if had_previous:
        print(f"Previous title retained at {backup}")


def clean_stage(ftp: ftplib.FTP, files: list[str], folders: list[str]) -> None:
    try:
        for path in reversed(files):
            try:
                ftp.delete(path)
            except ftplib.error_perm as error:
                # A failed transfer may never have created its file.
                if not str(error).startswith("550"):
                    raise
        for path in reversed(folders):
            ftp.rmd(path)
    except ftplib.all_errors as error:
        print(f"deploy: staging cleanup was incomplete at {folders[0]}: {error}", file=sys.stderr)


def refresh_registered_param(ftp: ftplib.FTP, source: Path, title_id: str) -> None:
    """ShadowMountPlus keeps its own copy of sce_sys once a title is registered, in /user/app/<title>, and the launcher
    reads that copy: a changed param.json (run 5: amm.pagetableMemorySizeInMib) did nothing until it was copied there by
    hand. The idea is OcarinaPS5's tools/deploy.sh; this is our own code. Nothing to do before the title is registered."""
    remote = f"{REGISTERED_APPS}/{title_id}/sce_sys/param.json"
    try:
        listed = ftp.nlst(f"{REGISTERED_APPS}/{title_id}/sce_sys")
    except ftplib.error_perm:
        return
    if not any(Path(name).name == "param.json" for name in listed):
        return
    upload_file(ftp, source / "sce_sys/param.json", remote)
    print(f"Refreshed the registered copy {remote}", flush=True)


def deploy(address: str, port: int, source: Path, files: list[Path], folders: list[Path], title_id: str) -> None:
    nonce = uuid4().hex
    stage, backup = f"{TEMP_PREFIX}-stage-{nonce}", f"{TEMP_PREFIX}-backup-{nonce}"
    ftp = ftplib.FTP(timeout=30)
    try:
        ftp.connect(address, port)
        ftp.login()
        print(f"Staging {len(files)} files at {stage}", flush=True)
        ftp.mkd(stage)
        made_folders, started_files = [stage], []
        try:
            for folder in folders:
                remote = f"{stage}/{folder.as_posix()}"
                ftp.mkd(remote)
                made_folders.append(remote)
            for file in files:
                remote = f"{stage}/{file.as_posix()}"
                started_files.append(remote)
                upload_file(ftp, source / file, remote)
            print(f"Verified SHA-256 for all {len(files)} files", flush=True)
            publish(ftp, stage, backup)
        except STAGE_REJECTED:
            clean_stage(ftp, started_files, made_folders)
            raise
        refresh_registered_param(ftp, source, title_id)
    finally:
        ftp.close()
    print(f"Published {DESTINATION}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("console", help="console IP address or hostname")
    parser.add_argument("mode", choices=("app",), nargs="?", default="app")
    optional = parser.add_mutually_exclusive_group()
    optional.add_argument("--query-memory", dest="work", action="store_const", const=QUERY_WORK,
                          help="select the optional QueryMemory build")
    optional.add_argument("--dualsense", dest="work", action="store_const", const=DUALSENSE_WORK,
                          help="select the optional DualSense input build")
    optional.add_argument("--alias-subrange", dest="work", action="store_const", const=ALIAS_WORK,
                          help="select the optional alias-subrange build")
    optional.add_argument("--page-table", dest="work", action="store_const", const=PAGE_TABLE_WORK,
                          help="select the optional page-table census build")
    optional.add_argument("--game", dest="game", action="store_true",
                          help="select the loader-stage game build (PPSA99193)")
    parser.set_defaults(work=WORK)
    parser.add_argument("--dry-run", action="store_true", help="show the deployment plan without connecting")
    args = parser.parse_args()
    if args.game:
        global DESTINATION, TEMP_PREFIX, PUBLISH_LOCK
        title_id = GAME_TITLE_ID
        args.work = GAME_WORK
        DESTINATION = f"{HOMEBREW}/{title_id}"
        TEMP_PREFIX = f"{HOMEBREW}/.mkw-game-{title_id}"
        PUBLISH_LOCK = f"{TEMP_PREFIX}-publish"
    else:
        title_id = TITLE_ID
    try:
        port = int(os.environ.get("PS5_FTP_PORT", "2121"))
    except ValueError:
        parser.error("PS5_FTP_PORT must be an integer from 1 to 65535")
    if not 1 <= port <= 65535:
        parser.error("PS5_FTP_PORT must be from 1 to 65535")
    source = args.work / "title/dist" / title_id
    try:
        files, folders = package_paths(source, title_id)
        if args.dry_run:
            print(f"Would upload {len(files)} files from {source} to {args.console} on FTP port {port}")
            print(f"Would stage under {TEMP_PREFIX}-stage-<unique>, verify SHA-256, then publish {DESTINATION}")
            print(f"An existing title would be saved at {TEMP_PREFIX}-backup-<unique>")
            print(f"{REGISTERED_APPS}/{title_id}/sce_sys/param.json would be refreshed if it exists (ShadowMountPlus)")
        else:
            deploy(args.console, port, source, files, folders, title_id)
    except DEPLOY_ERRORS as error:
        print(f"deploy: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("deploy: interrupted", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
