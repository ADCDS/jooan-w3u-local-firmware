#!/usr/bin/env python3
"""Build deterministic install and uninstall IronMan packages from stage dirs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import cast


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "src"))

from ironman.package import build_package, package_manifest_entry, write_json  # noqa: E402
from ironman.trailer import HEADER_LEN, PAYLOAD_LIMIT, TRAILER_LEN  # noqa: E402


DEFAULT_TARGET = REPOSITORY / "packaging" / "targets" / "ja-a12.json"


def load_target(path: Path) -> dict[str, object]:
    with path.open("r", encoding="utf-8") as source:
        target = json.load(source)
    ironman = target.get("ironman")
    if not isinstance(ironman, dict):
        raise ValueError("target manifest has no ironman object")
    required = {
        "model_token",
        "default_firmware_version",
        "install_filename",
        "uninstall_filename",
        "payload_limit_exclusive",
    }
    missing = sorted(required - ironman.keys())
    if missing:
        raise ValueError("target manifest is missing: " + ", ".join(missing))
    expected_format_values = {
        "header_bytes": HEADER_LEN,
        "payload_limit_exclusive": PAYLOAD_LIMIT,
        "trailer_bytes": TRAILER_LEN,
    }
    for key, expected in expected_format_values.items():
        if ironman.get(key) != expected:
            raise ValueError(
                f"target manifest {key} must be {expected}, got {ironman.get(key)!r}"
            )
    for key in ("install_filename", "uninstall_filename"):
        filename = str(ironman[key])
        if Path(filename).name != filename or filename in (".", ".."):
            raise ValueError(f"target manifest has unsafe {key}: {filename!r}")
    return target


def add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--target", type=Path, default=DEFAULT_TARGET)
    parser.add_argument("--out-dir", type=Path, default=REPOSITORY / "dist")
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--firmware-version")
    parser.add_argument("--compression", default="xz")
    parser.add_argument("--block-size", default="128K")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    release = subparsers.add_parser("release", help="build install and uninstall")
    add_common_options(release)
    release.add_argument("--install-stage", type=Path, required=True)
    release.add_argument("--uninstall-stage", type=Path, required=True)

    package = subparsers.add_parser("package", help="build one package")
    add_common_options(package)
    package.add_argument("--kind", choices=("install", "uninstall"), required=True)
    package.add_argument("--stage", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    target_path = arguments.target.resolve()
    target = load_target(target_path)
    ironman = cast(dict[str, object], target["ironman"])

    model_token = str(ironman["model_token"])
    firmware_version = (
        arguments.firmware_version
        or str(ironman["default_firmware_version"])
    )
    output_dir = arguments.out_dir.resolve()

    if arguments.command == "release":
        stages = {
            "install": arguments.install_stage,
            "uninstall": arguments.uninstall_stage,
        }
    else:
        stages = {arguments.kind: arguments.stage}

    filenames = {
        "install": str(ironman["install_filename"]),
        "uninstall": str(ironman["uninstall_filename"]),
    }
    packages: dict[str, object] = {}
    for kind in sorted(stages):
        output = output_dir / filenames[kind]
        info, padding_size = build_package(
            stages[kind],
            output,
            model_token=model_token,
            firmware_version=firmware_version,
            compression=arguments.compression,
            block_size=arguments.block_size,
        )
        entry = package_manifest_entry(info, padding_size)
        entry["file"] = output.name
        entry["sha256_file"] = output.name + ".sha256"
        packages[kind] = entry
        print(f"built {output} ({info.package_size} bytes, {info.package_sha256})")

    release_manifest = {
        "firmware_version": firmware_version,
        "packages": packages,
        "release_version": arguments.release_version,
        "schema_version": 1,
        "target": target,
    }
    manifest_path = output_dir / "manifest.json"
    write_json(manifest_path, release_manifest)
    print(f"wrote {manifest_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
