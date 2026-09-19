#!/usr/bin/env python3
"""Keep the installed persistent payload at or below 180 KiB."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


LIMIT = 180 * 1024
CANDIDATES = (
    "build/persistent",
    "build/rootfs/opt/open",
    "staging/opt/open",
    "packaging/rootfs/opt/open",
    "rootfs/opt/open",
)


def apparent_size(root: Path) -> tuple[int, list[tuple[int, str]]]:
    total = 0
    files: list[tuple[int, str]] = []
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        filenames.sort()
        base = Path(directory)
        for name in filenames:
            path = base / name
            if path.is_symlink():
                size = len(os.readlink(path).encode("utf-8"))
            elif path.is_file():
                size = path.stat().st_size
            else:
                continue
            total += size
            files.append((size, path.relative_to(root).as_posix()))
    return total, sorted(files, reverse=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?")
    parser.add_argument("--limit", type=int, default=LIMIT)
    parser.add_argument("--require", action="store_true")
    args = parser.parse_args()
    repo = Path(subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    supplied = args.root or os.environ.get("PERSISTENT_ROOT")
    target = Path(supplied).resolve() if supplied else next(
        (repo / item for item in CANDIDATES if (repo / item).is_dir()), None
    )
    if target is None or not target.is_dir():
        message = "persistent-size: SKIP (no staging root; set PERSISTENT_ROOT)"
        if args.require:
            print(message, file=sys.stderr)
            return 2
        print(message)
        return 0
    total, files = apparent_size(target)
    print(f"persistent-size: {total} bytes / {args.limit} bytes at {target}")
    for size, name in files[:10]:
        print(f"  {size:8d}  {name}")
    if total > args.limit:
        print(f"persistent-size gate failed by {total - args.limit} bytes", file=sys.stderr)
        return 1
    print("persistent-size: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
