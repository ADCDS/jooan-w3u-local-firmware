"""Deterministic host-side SquashFS and IronMan package construction."""

from __future__ import annotations

from dataclasses import asdict
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from .trailer import PackageInfo, assemble_package, inspect_package


def validate_stage(stage: Path) -> None:
    if not stage.is_dir():
        raise ValueError(f"payload stage is not a directory: {stage}")
    upgrade = stage / "upgrade.sh"
    if not upgrade.is_file():
        raise ValueError(f"payload stage is missing upgrade.sh: {stage}")
    if not os.access(upgrade, os.X_OK):
        raise ValueError(f"payload upgrade.sh is not executable: {upgrade}")


def make_squashfs(
    stage: Path,
    output: Path,
    *,
    compression: str = "xz",
    block_size: str = "128K",
) -> None:
    """Build a reproducible SquashFS with conservative host resource use."""
    validate_stage(stage)
    executable = shutil.which("mksquashfs")
    if executable is None:
        raise RuntimeError("mksquashfs is required (install squashfs-tools)")

    environment = os.environ.copy()
    environment.update({"SOURCE_DATE_EPOCH": "0", "LC_ALL": "C", "TZ": "UTC"})
    command = [
        executable,
        str(stage),
        str(output),
        "-comp",
        compression,
        "-b",
        block_size,
        "-noappend",
        "-no-progress",
        "-all-root",
        "-no-xattrs",
        "-processors",
        "1",
    ]
    result = subprocess.run(
        command,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "mksquashfs failed:\n" + result.stdout.rstrip()
        )


def _atomic_write(path: Path, data: bytes, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def build_package(
    stage: Path,
    output: Path,
    *,
    model_token: str,
    firmware_version: str,
    compression: str = "xz",
    block_size: str = "128K",
) -> tuple[PackageInfo, int]:
    """Build one SquashFS-backed IronMan package atomically."""
    stage = stage.resolve()
    output = output.resolve()
    validate_stage(stage)
    if output.is_relative_to(stage):
        raise ValueError("package output must not be inside its payload stage")

    with tempfile.TemporaryDirectory(prefix="jooan-ironman-") as temporary_dir:
        squashfs = Path(temporary_dir) / "payload.sqfs"
        make_squashfs(
            stage,
            squashfs,
            compression=compression,
            block_size=block_size,
        )
        package, padding_size = assemble_package(
            squashfs.read_bytes(),
            model_token=model_token,
            firmware_version=firmware_version,
        )

    info = inspect_package(package, expected_model=model_token)
    _atomic_write(output, package)
    _atomic_write(
        output.with_name(output.name + ".sha256"),
        f"{info.package_sha256}  {output.name}\n".encode("ascii"),
    )
    return info, padding_size


def write_json(path: Path, value: object) -> None:
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    _atomic_write(path.resolve(), encoded)


def package_manifest_entry(info: PackageInfo, padding_size: int) -> dict[str, object]:
    entry = asdict(info)
    entry["trailer_padding_size"] = padding_size
    return entry
