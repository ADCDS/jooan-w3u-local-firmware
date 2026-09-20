#!/usr/bin/env python3
"""Upload a validated IronMan package through the JA-A12 OEM installer.

This transport is intentionally limited to the exact unauthenticated OEM
bootstrap.  Once jooan-local is active, subsequent releases are uploaded
through its authenticated HTTPS update API instead.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import sys
import time
import uuid

REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY))

from src.ironman.signing import verify_bytes  # noqa: E402
from src.ironman.trailer import inspect_package  # noqa: E402

FIELD_NAME = "filename"
CGI_PATH = "/cgi-bin/upload_app.cgi"


def multipart(package: Path, boundary: str) -> bytes:
    if any(character in package.name for character in ('"', "\r", "\n")):
        raise ValueError("package filename is not safe for multipart upload")
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{FIELD_NAME}"; '
        f'filename="{package.name}"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode()
    return head + package.read_bytes() + f"\r\n--{boundary}--\r\n".encode()


def verify_release_index(
    package: Path, manifest_path: Path, signature_path: Path, target_path: Path
) -> tuple[str, int]:
    target = json.loads(target_path.read_text(encoding="utf-8"))
    manifest_data = manifest_path.read_bytes()
    verify_bytes(
        manifest_data,
        signature_path.read_bytes(),
        target["release_authenticity"]["public_key_sec1"],
    )
    manifest = json.loads(manifest_data)
    if manifest.get("release_ready") is not True:
        raise ValueError("release index is not signed/release-ready")
    signed_target = manifest.get("target")
    if not isinstance(signed_target, dict):
        raise ValueError("release index has no signed target contract")
    target_checks = (
        signed_target.get("target_id") == target.get("target_id"),
        signed_target.get("device", {}).get("model") == target.get("device", {}).get("model"),
        signed_target.get("ironman", {}).get("model_token")
        == target.get("ironman", {}).get("model_token"),
        signed_target.get("compatibility_hashes") == target.get("compatibility_hashes"),
        signed_target.get("abi_contract") == target.get("abi_contract"),
        manifest.get("authenticity", {}).get("key_id")
        == target.get("release_authenticity", {}).get("key_id"),
    )
    if not all(target_checks):
        raise ValueError("signed release target/ABI contract does not match this repository")
    entry = manifest.get("packages", {}).get("install")
    if not isinstance(entry, dict) or entry.get("artifact_kind") != "install":
        raise ValueError("release index has no install artifact")
    data = package.read_bytes()
    if entry.get("file") != package.name:
        raise ValueError("package filename does not match signed release index")
    if entry.get("package_size") != len(data):
        raise ValueError("package size does not match signed release index")
    if entry.get("package_sha256") != hashlib.sha256(data).hexdigest():
        raise ValueError("package SHA-256 does not match signed release index")
    info = inspect_package(data, expected_model=target["ironman"]["model_token"])
    if info.model_token != entry.get("model_token"):
        raise ValueError("package model does not match signed release index")
    release_version = manifest.get("release_version")
    release_sequence = entry.get("release_sequence")
    if not isinstance(release_version, str) or not release_version:
        raise ValueError("signed release version is missing")
    if not isinstance(release_sequence, int) or release_sequence < 1:
        raise ValueError("signed release sequence is invalid")
    return release_version, release_sequence


def wait_for_https(
    host: str,
    port: int,
    timeout: float,
    expected_version: str,
    expected_sequence: int,
) -> bool:
    deadline = time.monotonic() + timeout
    context = ssl._create_unverified_context()
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=3) as raw:
                with context.wrap_socket(raw, server_hostname=host) as tls:
                    certificate = tls.getpeercert(binary_form=True)
            fingerprint = hashlib.sha256(certificate).hexdigest()
            connection = http.client.HTTPSConnection(
                host, port, timeout=5, context=context
            )
            connection.request("GET", "/api/v1/setup/status")
            response = connection.getresponse()
            body = response.read()
            connection.close()
            try:
                status = json.loads(body)
            except (UnicodeDecodeError, json.JSONDecodeError):
                status = {}
            if (
                response.status == 200
                and status.get("release_version") == expected_version
                and status.get("release_sequence") == expected_sequence
            ):
                print(f"[+] HTTPS ready; certificate SHA-256 {fingerprint}")
                return True
            if response.status == 200:
                print(
                    "[*] HTTPS answered but signed release identity is not active "
                    f"(got {status.get('release_version')!r}/"
                    f"{status.get('release_sequence')!r})"
                )
        except (OSError, ssl.SSLError, http.client.HTTPException):
            pass
        time.sleep(2)
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--pkg", type=Path, required=True)
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--no-wait", action="store_true")
    parser.add_argument("--https-port", type=int, default=443)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--manifest-signature", type=Path)
    parser.add_argument(
        "--target",
        type=Path,
        default=REPOSITORY / "packaging/targets/ja-a12.json",
    )
    args = parser.parse_args()

    package = args.pkg.resolve()
    manifest = (args.manifest or package.parent / "manifest.json").resolve()
    manifest_signature = (
        args.manifest_signature or package.parent / "manifest.json.sig"
    ).resolve()
    expected_version, expected_sequence = verify_release_index(
        package,
        manifest,
        manifest_signature,
        args.target.resolve(),
    )
    boundary = f"----jooan{uuid.uuid4().hex}"
    body = multipart(package, boundary)
    connection = http.client.HTTPConnection(
        args.host, args.port, timeout=args.timeout
    )
    print(
        f"[*] POST http://{args.host}:{args.port}{CGI_PATH} "
        f"({len(body)} bytes)"
    )
    accepted = False
    try:
        connection.request(
            "POST",
            CGI_PATH,
            body=body,
            headers={
                "Content-Type": f"multipart/form-data; boundary={boundary}",
                "Content-Length": str(len(body)),
                "Connection": "close",
            },
        )
        response = connection.getresponse()
        payload = response.read().decode("utf-8", "replace")
        accepted = "Done...rebooting" in payload
        if not accepted:
            print(f"[-] unexpected OEM response HTTP {response.status}: {payload}")
            return 2
    except (socket.timeout, TimeoutError, ConnectionResetError, OSError) as error:
        # This camera commonly reboots after consuming the request but before
        # GoAhead emits a valid response.  Post-boot HTTPS is the final proof.
        print(f"[*] OEM connection ended after upload: {type(error).__name__}: {error}")
        if args.no_wait:
            print("[-] cannot prove that the camera accepted the upload without post-boot verification")
            return 2
        accepted = True
    finally:
        connection.close()

    if not accepted:
        return 2
    if args.no_wait:
        print("[-] upload was sent, but no post-boot release identity was verified")
        return 2
    print(
        "[*] waiting for signed release identity "
        f"{expected_version} sequence {expected_sequence}"
    )
    if wait_for_https(
        args.host,
        args.https_port,
        args.timeout,
        expected_version,
        expected_sequence,
    ):
        return 0
    print("[-] package upload completed but HTTPS did not become healthy")
    return 3


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
