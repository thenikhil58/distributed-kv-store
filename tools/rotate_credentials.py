#!/usr/bin/env python3
"""Atomically activate a pre-staged credential bundle and reload Raft servers."""

import argparse
import os
from pathlib import Path
import signal
import stat
import subprocess
import tempfile


PRIVATE_FILES = ("key.pem", "auth.conf", "node.token")


def validate_bundle(path):
    if stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise ValueError("credential directory must have mode 0700 or stricter")
    for name in ("ca.pem", "cert.pem", "key.pem", "auth.conf", "node.token"):
        candidate = path / name
        if not candidate.is_file() or candidate.is_symlink():
            raise ValueError(f"credential bundle must contain a regular {name}")
    for name in PRIVATE_FILES:
        if stat.S_IMODE((path / name).stat().st_mode) & 0o077:
            raise ValueError(f"{name} must have mode 0600 or stricter")
    crl_directory = path / "crls"
    if not crl_directory.is_dir():
        raise ValueError("credential bundle must contain a crls directory")
    if stat.S_IMODE(crl_directory.stat().st_mode) & 0o022:
        raise ValueError("crls directory must not be group/world writable")


def validate_process(pid):
    if pid <= 1:
        raise ValueError(f"refusing unsafe process ID: {pid}")
    proc = Path("/proc") / str(pid)
    if proc.exists():
        executable = (proc / "exe").resolve(strict=True)
        if executable.name != "server":
            raise ValueError(f"process {pid} is not the Raft server executable")
        if proc.stat().st_uid != os.getuid():
            raise ValueError(f"process {pid} belongs to a different user")
    else:
        result = subprocess.run(
            ["ps", "-o", "comm=", "-o", "uid=", "-p", str(pid)],
            check=False, capture_output=True, text=True,
        )
        fields = result.stdout.split()
        if result.returncode != 0 or len(fields) < 2:
            raise ValueError(f"could not verify process {pid}")
        if Path(fields[0]).name != "server" or int(fields[1]) != os.getuid():
            raise ValueError(f"process {pid} is not this user's Raft server")
    os.kill(pid, 0)


def activate_bundle(current_link, new_directory, pids):
    link = Path(current_link).absolute()
    if not link.is_symlink():
        raise ValueError(f"current credential path must be a symlink: {link}")
    parent = link.parent.resolve(strict=True)
    old_target = link.resolve(strict=True)
    new_target = Path(new_directory).resolve(strict=True)
    if old_target.parent != parent or new_target.parent != parent:
        raise ValueError("new credential directory must be a sibling of the current target")
    if new_target == old_target:
        raise ValueError("new credential directory is already active")
    validate_bundle(new_target)
    for pid in pids:
        validate_process(pid)

    with tempfile.TemporaryDirectory(prefix=".credential-rotation-", dir=parent) as staging:
        temporary_link = Path(staging) / link.name
        os.symlink(os.path.relpath(new_target, parent), temporary_link)
        os.replace(temporary_link, link)

    for pid in pids:
        os.kill(pid, signal.SIGHUP)
    return old_target, new_target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--current-link", required=True, help="active credentials symlink")
    parser.add_argument("--new-directory", required=True, help="pre-staged sibling credential directory")
    parser.add_argument("--pid", type=int, action="append", default=[],
                        help="Raft server PID to signal with SIGHUP; repeat per node")
    args = parser.parse_args()
    if not args.pid:
        parser.error("provide at least one --pid for every running Raft server")
    try:
        old_target, new_target = activate_bundle(
            args.current_link, args.new_directory, args.pid
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"Activated credential bundle {new_target}; previous bundle was {old_target}.")


if __name__ == "__main__":
    main()
