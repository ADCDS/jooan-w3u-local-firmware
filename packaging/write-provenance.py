#!/usr/bin/env python3
"""Write deterministic release provenance without private filesystem paths."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--toolchain-root", type=Path, required=True)
    parser.add_argument("--oem-rootfs", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    repo = arguments.repo.resolve()
    build = arguments.build_root.resolve()
    target_contract = json.loads(arguments.target.read_text(encoding="utf-8"))
    compatibility = target_contract["compatibility_hashes"]
    checked_abi: dict[str, str] = {}
    for device_path, expected in sorted(compatibility.items()):
        if not device_path.startswith("/lib/"):
            continue
        local = arguments.oem_rootfs / device_path.lstrip("/")
        if not local.is_file():
            raise ValueError(f"required OEM ABI input is missing: {device_path}")
        actual = digest(local)
        if actual != expected:
            raise ValueError(
                f"OEM ABI input {device_path} has SHA-256 {actual}, expected {expected}"
            )
        checked_abi[device_path] = actual

    compiler = arguments.toolchain_root / "bin/mips-linux-gnu-gcc"
    compiler_version = subprocess.check_output(
        [str(compiler), "--version"], text=True
    ).splitlines()[0]
    source_commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
    ).strip()
    outputs: dict[str, str] = {}
    target_root = build / "target"
    for path in sorted(target_root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            outputs[path.relative_to(target_root).as_posix()] = digest(path)

    provenance = {
        "build_inputs": {
            "compiler_sha256": digest(compiler),
            "compiler_version": compiler_version,
            "dropbear_source_sha256": "e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81",
            "mbedtls_commit": "1c54b5410fd48d6bcada97e30cac417c5c7eea67",
            "oem_abi_sha256": checked_abi,
            "target_contract_sha256": digest(arguments.target),
        },
        "outputs": outputs,
        "schema_version": 1,
        "source_commit": source_commit,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
