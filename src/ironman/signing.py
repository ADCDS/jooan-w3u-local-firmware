"""Deterministic release-manifest signing and verification.

Private keys are operator inputs and must never be stored in the repository.
Signatures use ECDSA P-256 with SHA-256, deterministic RFC 6979 nonces, low-S
normalization, and ASN.1 DER encoding.  The optional ``cryptography`` package is
required only by release maintainers and host-side verification.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path


ALGORITHM = "ecdsa-p256-sha256-rfc6979"
SIGNATURE_FORMAT = "asn1-der"
P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551


class SigningUnavailable(RuntimeError):
    """The optional host signing dependency is unavailable."""


class SignatureError(ValueError):
    """A key or signature does not satisfy the release contract."""


def _crypto():
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError as error:
        raise SigningUnavailable(
            "release signing requires the Python 'cryptography' package"
        ) from error
    return InvalidSignature, hashes, serialization, ec


def parse_public_key(public_key_hex: str):
    _, _, _, ec = _crypto()
    try:
        encoded = bytes.fromhex(public_key_hex)
    except ValueError as error:
        raise SignatureError("release public key is not hexadecimal") from error
    if len(encoded) != 65 or encoded[0] != 0x04:
        raise SignatureError("release public key must be uncompressed P-256 SEC1")
    try:
        return ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), encoded)
    except ValueError as error:
        raise SignatureError("release public key is not a valid P-256 point") from error


def public_key_bytes(public_key) -> bytes:
    _, _, serialization, _ = _crypto()
    return public_key.public_bytes(
        serialization.Encoding.X962,
        serialization.PublicFormat.UncompressedPoint,
    )


def public_key_id(public_key_hex: str) -> str:
    public_key = parse_public_key(public_key_hex)
    return hashlib.sha256(public_key_bytes(public_key)).hexdigest()


def load_private_key(path: Path):
    _, _, serialization, ec = _crypto()
    path = path.resolve()
    mode = path.stat().st_mode & 0o777
    if mode & 0o077:
        raise SignatureError(
            f"release signing key permissions must be 0600 or stricter, got {mode:04o}"
        )
    private = serialization.load_pem_private_key(path.read_bytes(), password=None)
    if not isinstance(private, ec.EllipticCurvePrivateKey) or not isinstance(
        private.curve, ec.SECP256R1
    ):
        raise SignatureError("release signing key must be an unencrypted P-256 PEM key")
    return private


def private_public_key_hex(path: Path) -> str:
    private = load_private_key(path)
    return public_key_bytes(private.public_key()).hex()


def sign_bytes(data: bytes, private_key_path: Path) -> bytes:
    _, hashes, _, ec = _crypto()
    from cryptography.hazmat.primitives.asymmetric.utils import (
        decode_dss_signature,
        encode_dss_signature,
    )
    private = load_private_key(private_key_path)
    # cryptography delegates deterministic ECDSA to OpenSSL and refuses the
    # operation if the installed backend cannot provide RFC 6979 behavior.
    signature = private.sign(
        data,
        ec.ECDSA(hashes.SHA256(), deterministic_signing=True),
    )
    second = private.sign(
        data,
        ec.ECDSA(hashes.SHA256(), deterministic_signing=True),
    )
    if signature != second:
        raise SignatureError("signing backend did not produce a deterministic signature")
    r, s = decode_dss_signature(signature)
    if s > P256_ORDER // 2:
        s = P256_ORDER - s
    return encode_dss_signature(r, s)


def verify_bytes(data: bytes, signature: bytes, public_key_hex: str) -> None:
    InvalidSignature, hashes, _, ec = _crypto()
    public = parse_public_key(public_key_hex)
    try:
        public.verify(signature, data, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature as error:
        raise SignatureError("release manifest signature is invalid") from error


def require_matching_key(private_key_path: Path, public_key_hex: str) -> None:
    actual = private_public_key_hex(private_key_path)
    if actual.lower() != public_key_hex.lower():
        raise SignatureError("release private key does not match the pinned public key")


def secure_default_key_path() -> Path:
    configured = os.environ.get("JOOAN_RELEASE_SIGNING_KEY")
    if configured:
        return Path(configured)
    return Path.home() / ".config/jooan-w3u-local-firmware/release-signing-key.pem"
