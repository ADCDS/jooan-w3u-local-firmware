#!/usr/bin/env python3
"""Generate device-consumable contracts from the single target JSON."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def load_target(path: Path) -> dict[str, object]:
    target = json.loads(path.read_text(encoding="utf-8"))
    for key in (
        "target_id",
        "device",
        "ironman",
        "compatibility_hashes",
        "abi_contract",
        "persistent_contract",
        "migration_contract",
        "release_authenticity",
        "update_policy",
    ):
        if key not in target:
            raise ValueError(f"target contract is missing {key}")
    return target


def compatibility(target: dict[str, object]) -> str:
    hashes = target["compatibility_hashes"]
    if not isinstance(hashes, dict) or not hashes:
        raise ValueError("target compatibility_hashes must be a non-empty object")
    lines = []
    for path in sorted(hashes):
        digest = hashes[path]
        if not isinstance(path, str) or not path.startswith("/") or ".." in Path(path).parts:
            raise ValueError(f"unsafe compatibility path: {path!r}")
        if not isinstance(digest, str) or len(digest) != 64:
            raise ValueError(f"invalid compatibility SHA-256 for {path}")
        int(digest, 16)
        lines.append(f"{digest.lower()}  {path}")
    return "\n".join(lines) + "\n"


def persistent(target: dict[str, object]) -> str:
    contract = target["persistent_contract"]
    if not isinstance(contract, dict):
        raise ValueError("persistent_contract must be an object")
    layout = contract.get("layout")
    if not isinstance(layout, list) or not layout:
        raise ValueError("persistent layout must be a non-empty array")
    lines = [
        "JOOAN-PERSISTENT-CONTRACT-V1",
        f"root={contract['root']}",
        f"logical_regular_file_cap_bytes={int(contract['logical_regular_file_cap_bytes'])}",
        f"final_free_reserve_bytes={int(contract['final_free_reserve_bytes'])}",
        f"state_config_regular_file_reserve_bytes={int(contract['state_config_regular_file_reserve_bytes'])}",
        f"external_regular_file_reserve_bytes={int(contract['external_regular_file_reserve_bytes'])}",
        f"transient_regular_file_cap_bytes={int(contract['transient_regular_file_cap_bytes'])}",
        f"transient_final_free_reserve_bytes={int(contract['transient_final_free_reserve_bytes'])}",
    ]
    lines.extend(f"layout={item}" for item in layout)
    return "\n".join(lines) + "\n"


def migration(target: dict[str, object]) -> str:
    contract = target["migration_contract"]
    if not isinstance(contract, dict):
        raise ValueError("migration_contract must be an object")
    states = contract.get("states")
    if not isinstance(states, list) or states != [
        "none",
        "legacy-validated",
        "expanded-product-0.1-validated",
        "keys-preserved",
        "headroom-reclaiming",
        "headroom-reclaimed",
        "controller-published",
        "failclosed-hook-published",
        "activated",
        "expanded-controller-retired",
        "runtime-staged",
        "legacy-retired",
    ]:
        raise ValueError("migration states are not the version-1 state machine")
    lines = [
        "JOOAN-MIGRATION-CONTRACT-V1",
        f"journal={contract['journal']}",
        f"legacy_root={contract['legacy_root']}",
    ]
    hashes = contract.get("expanded_0_1_sha256")
    if not isinstance(hashes, dict) or not hashes:
        raise ValueError("migration contract has no pinned expanded-product hashes")
    for path in sorted(hashes):
        digest = hashes[path]
        if not isinstance(digest, str) or len(digest) != 64:
            raise ValueError(f"invalid expanded migration hash for {path}")
        int(digest, 16)
        lines.append(f"expanded_sha256={digest}  {path}")
    lines.extend(f"state={state}" for state in states)
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("compatibility", "persistent", "migration", "public-key"))
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    target = load_target(arguments.target)
    if arguments.kind == "compatibility":
        value = compatibility(target)
    elif arguments.kind == "persistent":
        value = persistent(target)
    elif arguments.kind == "migration":
        value = migration(target)
    else:
        value = str(target["release_authenticity"]["public_key_sec1"]) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(value, encoding="utf-8")
    else:
        sys.stdout.write(value)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
