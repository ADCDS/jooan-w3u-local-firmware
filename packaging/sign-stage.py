#!/usr/bin/env python3
"""Create and verify the signed, device-readable inner release manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "src"))

from ironman.signing import (  # noqa: E402
    ALGORITHM,
    SIGNATURE_FORMAT,
    SignatureError,
    public_key_id,
    require_matching_key,
    secure_default_key_path,
    sign_bytes,
    verify_bytes,
)


MANIFEST_NAME = "release.manifest"
SIGNATURE_NAME = "release.manifest.sig"
MAGIC = "JOOAN-SIGNED-RELEASE-V1"


def load_target(path: Path) -> dict[str, object]:
    target = json.loads(path.read_text(encoding="utf-8"))
    authenticity = target.get("release_authenticity")
    if not isinstance(authenticity, dict):
        raise ValueError("target has no release_authenticity contract")
    public_key = authenticity.get("public_key_sec1")
    key_id = authenticity.get("key_id")
    if not isinstance(public_key, str) or public_key_id(public_key) != key_id:
        raise ValueError("target release public key/key id is inconsistent")
    if authenticity.get("algorithm") != ALGORITHM:
        raise ValueError("unsupported target release signature algorithm")
    return target


def _safe_files(stage: Path) -> list[Path]:
    files: list[Path] = []
    for path in sorted(stage.rglob("*")):
        if path.name in (MANIFEST_NAME, SIGNATURE_NAME):
            continue
        if path.is_symlink():
            raise ValueError(f"signed stage contains a symlink: {path}")
        if path.is_file():
            files.append(path)
    return files


def build_manifest(
    stage: Path,
    target: dict[str, object],
    *,
    kind: str,
    release_version: str,
    release_sequence: int,
) -> bytes:
    if kind not in ("install", "uninstall"):
        raise ValueError("artifact kind must be install or uninstall")
    if release_sequence < 1:
        raise ValueError("release sequence must be positive")
    ironman = target["ironman"]
    authenticity = target["release_authenticity"]
    update_policy = target["update_policy"]
    artifacts = target["artifact_contracts"]
    if not all(isinstance(item, dict) for item in (ironman, authenticity, update_policy, artifacts)):
        raise ValueError("target release contract has invalid object types")
    contract = artifacts[kind]
    if not isinstance(contract, dict):
        raise ValueError(f"target has no artifact contract for {kind}")

    metadata = [
        MAGIC,
        f"algorithm={authenticity['algorithm']}",
        f"signature_format={authenticity['signature_format']}",
        f"key_id={authenticity['key_id']}",
        f"target_id={target['target_id']}",
        f"device_model={target['device']['model']}",
        f"model_token={ironman['model_token']}",
        f"release_version={release_version}",
        f"release_sequence={release_sequence}",
        f"minimum_sequence={update_policy['minimum_sequence']}",
        f"artifact_kind={kind}",
        f"artifact_semantics={contract['semantics']}",
        f"rollback_artifact={contract['rollback_artifact']}",
        "files-begin",
    ]
    for path in _safe_files(stage):
        relative = path.relative_to(stage).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        metadata.append(f"{digest}  {relative}")
    metadata.append("files-end")
    return ("\n".join(metadata) + "\n").encode("utf-8")


def _external_sign(command: str, manifest: Path) -> bytes:
    arguments = shlex.split(command)
    if not arguments:
        raise ValueError("empty external signer command")
    signatures = []
    with tempfile.TemporaryDirectory(prefix="jooan-signer-") as temporary:
        for attempt in (1, 2):
            output = Path(temporary) / f"signature-{attempt}.der"
            subprocess.run(arguments + [str(manifest), str(output)], check=True)
            signatures.append(output.read_bytes())
    if signatures[0] != signatures[1]:
        raise SignatureError("external signer is not deterministic")
    return signatures[0]


def sign_stage(
    stage: Path,
    target_path: Path,
    *,
    kind: str,
    release_version: str,
    release_sequence: int,
    signing_key: Path | None,
    signer_command: str | None = None,
) -> None:
    stage = stage.resolve()
    target = load_target(target_path.resolve())
    public_key = target["release_authenticity"]["public_key_sec1"]
    manifest_data = build_manifest(
        stage,
        target,
        kind=kind,
        release_version=release_version,
        release_sequence=release_sequence,
    )
    manifest = stage / MANIFEST_NAME
    signature_path = stage / SIGNATURE_NAME
    manifest.write_bytes(manifest_data)
    if signer_command:
        signature = _external_sign(signer_command, manifest)
    else:
        if signing_key is None:
            raise ValueError("a private signing key or external signer is required")
        require_matching_key(signing_key, public_key)
        signature = sign_bytes(manifest_data, signing_key)
    verify_bytes(manifest_data, signature, public_key)
    signature_path.write_bytes(signature)
    os.chmod(manifest, 0o644)
    os.chmod(signature_path, 0o644)


def parse_manifest(data: bytes) -> tuple[dict[str, str], dict[str, str]]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise ValueError("release manifest is not UTF-8") from error
    if not lines or lines[0] != MAGIC:
        raise ValueError("invalid signed release manifest magic")
    metadata: dict[str, str] = {}
    files: dict[str, str] = {}
    in_files = False
    ended = False
    for line in lines[1:]:
        if line == "files-begin":
            if in_files or ended:
                raise ValueError("duplicate files-begin marker")
            in_files = True
            continue
        if line == "files-end":
            if not in_files or ended:
                raise ValueError("invalid files-end marker")
            in_files = False
            ended = True
            continue
        if ended or not line:
            raise ValueError("invalid content in signed release manifest")
        if in_files:
            parts = line.split("  ", 1)
            if len(parts) != 2 or len(parts[0]) != 64:
                raise ValueError("malformed signed file entry")
            int(parts[0], 16)
            path = parts[1]
            candidate = Path(path)
            if candidate.is_absolute() or ".." in candidate.parts or path in files:
                raise ValueError("unsafe or duplicate signed file path")
            files[path] = parts[0]
        else:
            parts = line.split("=", 1)
            if len(parts) != 2 or not parts[0] or parts[0] in metadata:
                raise ValueError("malformed or duplicate release metadata")
            metadata[parts[0]] = parts[1]
    if in_files or not ended or not files:
        raise ValueError("incomplete signed release manifest")
    return metadata, files


def verify_stage(stage: Path, target_path: Path, expected_kind: str | None = None) -> dict[str, str]:
    stage = stage.resolve()
    target = load_target(target_path.resolve())
    manifest = stage / MANIFEST_NAME
    signature = stage / SIGNATURE_NAME
    verify_bytes(
        manifest.read_bytes(),
        signature.read_bytes(),
        target["release_authenticity"]["public_key_sec1"],
    )
    metadata, files = parse_manifest(manifest.read_bytes())
    expected = {
        "algorithm": ALGORITHM,
        "signature_format": SIGNATURE_FORMAT,
        "key_id": target["release_authenticity"]["key_id"],
        "target_id": target["target_id"],
        "device_model": target["device"]["model"],
        "model_token": target["ironman"]["model_token"],
    }
    if expected_kind is not None:
        expected["artifact_kind"] = expected_kind
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise ValueError(f"signed release metadata mismatch for {key}")
    actual_files = {
        path.relative_to(stage).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in _safe_files(stage)
    }
    if actual_files != files:
        raise ValueError("signed release file inventory does not match the stage")
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    sign = subparsers.add_parser("sign")
    sign.add_argument("--stage", type=Path, required=True)
    sign.add_argument("--target", type=Path, required=True)
    sign.add_argument("--kind", choices=("install", "uninstall"), required=True)
    sign.add_argument("--release-version", required=True)
    sign.add_argument("--release-sequence", type=int, required=True)
    source = sign.add_mutually_exclusive_group()
    source.add_argument("--signing-key", type=Path)
    source.add_argument("--signer-command")
    verify = subparsers.add_parser("verify")
    verify.add_argument("--stage", type=Path, required=True)
    verify.add_argument("--target", type=Path, required=True)
    verify.add_argument("--kind", choices=("install", "uninstall"))
    arguments = parser.parse_args()
    if arguments.command == "sign":
        key = arguments.signing_key
        if key is None and not arguments.signer_command:
            key = secure_default_key_path()
        sign_stage(
            arguments.stage,
            arguments.target,
            kind=arguments.kind,
            release_version=arguments.release_version,
            release_sequence=arguments.release_sequence,
            signing_key=key,
            signer_command=arguments.signer_command,
        )
    else:
        verify_stage(arguments.stage, arguments.target, arguments.kind)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
