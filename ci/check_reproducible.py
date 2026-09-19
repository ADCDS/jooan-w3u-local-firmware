#!/usr/bin/env python3
"""Run an explicitly supplied lightweight build twice and compare artifacts.

The build command must honor BUILD_OUT and must write each --artifact beneath
that directory. CI should use the real release command; this tool intentionally
has no guessed default build.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def digest(path: Path) -> tuple[str, int]:
    return hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mode & 0o777


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--command", default=os.environ.get("REPRO_BUILD_CMD"))
    parser.add_argument("--artifact", action="append")
    parser.add_argument("--root")
    args = parser.parse_args()
    artifacts = args.artifact or ([os.environ["REPRO_ARTIFACT"]]
                                  if os.environ.get("REPRO_ARTIFACT") else [])
    if not args.command or not artifacts:
        print("reproducible-build: SKIP (set REPRO_BUILD_CMD and REPRO_ARTIFACT)")
        return 0
    root = Path(args.root or subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    snapshots: list[dict[str, tuple[str, int]]] = []
    with tempfile.TemporaryDirectory(prefix="jooan-repro-") as temporary:
        for run in (1, 2):
            out = Path(temporary) / f"run-{run}"
            out.mkdir()
            env = os.environ.copy()
            env.update({
                "BUILD_OUT": str(out),
                "LC_ALL": "C",
                "SOURCE_DATE_EPOCH": "0",
                "TZ": "UTC",
                "ZERO_AR_DATE": "1",
            })
            result = subprocess.run(
                ["sh", "-c", args.command], cwd=root, env=env,
                text=True, capture_output=True,
            )
            if result.returncode:
                sys.stderr.write(result.stdout)
                sys.stderr.write(result.stderr)
                print(f"reproducible-build: run {run} failed", file=sys.stderr)
                return 1
            current: dict[str, tuple[str, int]] = {}
            for relative in artifacts:
                path = out / relative
                if not path.is_file():
                    print(f"reproducible-build: missing {relative} in run {run}", file=sys.stderr)
                    return 1
                current[relative] = digest(path)
            snapshots.append(current)
    if snapshots[0] != snapshots[1]:
        print("reproducible-build gate failed:", file=sys.stderr)
        for relative in artifacts:
            print(f"  {relative}: {snapshots[0][relative]} != {snapshots[1][relative]}",
                  file=sys.stderr)
        return 1
    for relative, (sha256, mode) in snapshots[0].items():
        print(f"reproducible-build: {sha256} mode={mode:o} {relative}")
    print("reproducible-build: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
