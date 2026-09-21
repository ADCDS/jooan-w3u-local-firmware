#!/usr/bin/env python3
"""Enforce the exact compressed-controller persistent layout and reserves."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


DEFAULT_CAP = 192 * 1024
DEFAULT_RESERVE = 64 * 1024


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
                raise ValueError(f"persistent layout contains symlink: {path}")
            if not path.is_file():
                continue
            size = path.stat().st_size
            total += size
            files.append((size, path.relative_to(root).as_posix()))
    return total, sorted(files, reverse=True)


def contract_values(path: Path) -> tuple[int, int, int, int, list[str]]:
    target = json.loads(path.read_text(encoding="utf-8"))
    contract = target["persistent_contract"]
    cap = int(contract["logical_regular_file_cap_bytes"])
    reserve = int(contract["final_free_reserve_bytes"])
    state_config = int(contract["state_config_regular_file_reserve_bytes"])
    external = int(contract["external_regular_file_reserve_bytes"])
    transient_cap = int(contract["maintenance_regular_file_cap_bytes"])
    transient_reserve = int(contract["maintenance_final_free_reserve_bytes"])
    layout = list(contract["layout"])
    if cap != DEFAULT_CAP or reserve != DEFAULT_RESERVE:
        raise ValueError(
            f"target persistent contract must retain cap={DEFAULT_CAP}, reserve={DEFAULT_RESERVE}"
        )
    if state_config < 12 * 1024 or external < 2 * 1024:
        raise ValueError("persistent dynamic/external reservations are not realistic")
    if transient_cap != DEFAULT_CAP or transient_reserve < 56 * 1024:
        raise ValueError("maintenance contract must remain 192 KiB / at least 56 KiB")
    return cap, reserve, state_config, external, layout


def validate_layout(root: Path, layout: list[str]) -> list[str]:
    errors: list[str] = []
    allowed_files = {item for item in layout if not item.endswith("/")}
    required_directories = {item.rstrip("/") for item in layout if item.endswith("/")}
    for relative in sorted(allowed_files):
        path = root / relative
        if not path.is_file() or path.is_symlink():
            errors.append(f"missing required regular file: {relative}")
    for relative in sorted(required_directories):
        path = root / relative
        if not path.is_dir() or path.is_symlink():
            errors.append(f"missing required directory: {relative}/")
    for path in sorted(root.rglob("*")):
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            if relative not in allowed_files and not any(
                relative.startswith(directory + "/") for directory in required_directories
            ):
                errors.append(f"undeclared persistent file: {relative}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?")
    parser.add_argument("--target", type=Path, default=Path("packaging/targets/ja-a12.json"))
    parser.add_argument("--require", action="store_true")
    parser.add_argument("--allow-missing", action="store_true")
    arguments = parser.parse_args()
    repo = Path(subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    build_root = Path(os.environ.get("BUILD_ROOT", repo / "build"))
    supplied = arguments.root or os.environ.get("PERSISTENT_ROOT")
    root = Path(supplied).resolve() if supplied else (build_root / "persistent-layout").resolve()
    target = arguments.target
    if not target.is_absolute():
        target = repo / target
    cap, reserve, state_config, external, layout = contract_values(target.resolve())
    if not root.is_dir():
        message = f"persistent-size: MISSING required installed-layout model at {root}"
        if arguments.allow_missing and not arguments.require:
            print(message)
            return 0
        print(message, file=sys.stderr)
        return 2
    errors = validate_layout(root, layout)
    try:
        total, files = apparent_size(root)
    except ValueError as error:
        errors.append(str(error))
        total, files = 0, []
    projected = total + state_config + external
    print(
        f"persistent-size: static={total} state-config-reserve={state_config} "
        f"external-reserve={external} projected={projected} cap={cap} "
        f"final-free-reserve={reserve} root={root}"
    )
    for size, name in files:
        print(f"  {size:8d}  {name}")
    if projected > cap:
        errors.append(f"logical regular-file cap exceeded by {projected - cap} bytes")
    if errors:
        print("persistent-size gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("persistent-size: PASS (64 KiB final device free-space reserve is mandatory)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"persistent-size gate error: {error}", file=sys.stderr)
        raise SystemExit(2)
