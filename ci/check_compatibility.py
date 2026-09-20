#!/usr/bin/env python3
"""Verify exact compatibility inputs against a sha256sum-style manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


LINE = re.compile(r"^([0-9a-fA-F]{64})[ \t]+(?:\*?)(.+)$")


def target_errors(target_path: Path, device_manifest: Path | None) -> list[str]:
    errors: list[str] = []
    try:
        target = json.loads(target_path.read_text(encoding="utf-8"))
        hashes = target["compatibility_hashes"]
        required_sonames = target["abi_contract"]["required_sonames"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        return [f"invalid target contract {target_path}: {error}"]
    if not isinstance(hashes, dict) or not hashes:
        return ["target compatibility_hashes is empty or invalid"]
    generated: list[str] = []
    for path in sorted(hashes):
        digest = hashes[path]
        if not isinstance(path, str) or not path.startswith("/") or ".." in Path(path).parts:
            errors.append(f"unsafe target compatibility path: {path!r}")
            continue
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            errors.append(f"invalid target compatibility SHA-256: {path}")
            continue
        generated.append(f"{digest}  {path}")
    for soname in required_sonames:
        if soname.startswith("libmbed") and f"/lib/{soname}" not in hashes:
            errors.append(f"required dynamic ABI is not hash-gated: {soname}")
    if device_manifest is not None:
        if not device_manifest.is_file():
            errors.append(f"generated device compatibility manifest missing: {device_manifest}")
        elif device_manifest.read_text(encoding="utf-8").splitlines() != generated:
            errors.append("generated device compatibility manifest drifted from target JSON")
    return errors


def check(root: Path, manifest: Path) -> list[str]:
    errors: list[str] = []
    seen: set[str] = set()
    for line_number, raw in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = LINE.match(line)
        if not match:
            errors.append(f"{manifest}:{line_number}: malformed hash entry")
            continue
        expected, relative = match.groups()
        relative = relative.strip()
        candidate = (root / relative).resolve()
        try:
            candidate.relative_to(root.resolve())
        except ValueError:
            errors.append(f"{manifest}:{line_number}: path escapes repository: {relative}")
            continue
        if relative in seen:
            errors.append(f"{manifest}:{line_number}: duplicate path: {relative}")
            continue
        seen.add(relative)
        if not candidate.is_file() or candidate.is_symlink():
            errors.append(f"{relative}: pinned compatibility input is missing or a symlink")
            continue
        actual = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if actual != expected.lower():
            errors.append(f"{relative}: sha256 {actual}, expected {expected.lower()}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", nargs="?", default="ci/compatibility-hashes.sha256")
    parser.add_argument("--root")
    parser.add_argument("--target", default="packaging/targets/ja-a12.json")
    parser.add_argument("--device-manifest")
    args = parser.parse_args()
    root = Path(args.root or subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True
    ).strip()).resolve()
    manifest = (
        (root / args.manifest).resolve()
        if not Path(args.manifest).is_absolute()
        else Path(args.manifest)
    )
    target_path = (root / args.target).resolve() if not Path(args.target).is_absolute() else Path(args.target)
    device_manifest = Path(args.device_manifest).resolve() if args.device_manifest else None
    errors = check(root, manifest) + target_errors(target_path, device_manifest)
    if errors:
        print("compatibility-hash gate failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    entries = sum(1 for line in manifest.read_text().splitlines()
                  if line.strip() and not line.lstrip().startswith("#"))
    print(f"compatibility-hashes: PASS ({entries} pinned inputs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
