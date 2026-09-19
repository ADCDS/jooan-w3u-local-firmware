"""Codec and validator for the JOOAN IronMan OTA container.

The container uses MD5 as a vendor-format compatibility check. It is not a
signature and provides no authenticity. Release tooling adds SHA-256 metadata
outside the container for reproducibility and transport verification.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import re


HEADER_LEN = 0x60
TRAILER_LEN = 0x60
PAYLOAD_LIMIT = 0x200001  # The decoded payload length must be strictly lower.

HEADER_MAGIC = b"jooan\x00\x00\x00"
TRAILER_MAGIC = b"toolv\x00\x00\x00"
DEFAULT_MODEL_TOKEN = "A12"
DEFAULT_FIRMWARE_VERSION = "05.02.31.115"

_LOWER_MD5_RE = re.compile(rb"[0-9a-f]{32}\Z")


class UnencodableTrailer(ValueError):
    """The vendor transform cannot represent a particular decoded trailer."""


class PackageValidationError(ValueError):
    """The package does not satisfy the checks performed by the device."""


@dataclass(frozen=True)
class PackageInfo:
    model_token: str
    firmware_version: str
    package_size: int
    payload_size: int
    payload_md5: str
    body_md5: str
    package_sha256: str


def _md5(data: bytes) -> str:
    # This is mandated by the legacy file format, not used for security.
    return hashlib.md5(data, usedforsecurity=False).hexdigest()


def qa_decode(raw: bytes, size: int = TRAILER_LEN) -> bytes:
    """Decode the on-disk trailer using the device's QAUpgradeDecV1 transform."""
    if size != TRAILER_LEN:
        raise ValueError("only 0x60-byte IronMan trailers are supported")
    if len(raw) < size:
        raise ValueError("encoded trailer is shorter than 0x60 bytes")

    swapped = bytearray(raw[:size])
    stride = ((swapped[size - 1] >> 2) & 0x0F) + 1
    offset = ((size - 3) // stride) * stride
    while offset >= stride:
        swapped[offset], swapped[offset + 1] = (
            swapped[offset + 1],
            swapped[offset],
        )
        offset -= stride

    decoded = bytearray(size)
    decoded[0] = ((swapped[0] >> 2) | (swapped[size - 1] << 6)) & 0xFF
    for offset in range(1, size):
        decoded[offset] = (
            (swapped[offset] >> 2) | (swapped[offset - 1] << 6)
        ) & 0xFF
    return bytes(decoded)


def _rotate_left_two(decoded: bytes) -> bytes:
    rotated = bytearray(TRAILER_LEN)
    for offset in range(TRAILER_LEN - 1):
        rotated[offset] = (
            (decoded[offset] << 2) | (decoded[offset + 1] >> 6)
        ) & 0xFF
    rotated[-1] = ((decoded[-1] << 2) | (decoded[0] >> 6)) & 0xFF
    return bytes(rotated)


def _swap_pairs(data: bytes, stride: int) -> bytes:
    swapped = bytearray(data)
    offset = ((TRAILER_LEN - 3) // stride) * stride
    while offset >= stride:
        swapped[offset], swapped[offset + 1] = (
            swapped[offset + 1],
            swapped[offset],
        )
        offset -= stride
    return bytes(swapped)


def qa_encode(decoded: bytes, size: int = TRAILER_LEN) -> bytes:
    """Encode a decoded trailer, refusing non-round-trippable inputs.

    The swap stride is recovered from the encoded final byte. For some trailer
    contents this self-reference has no valid inverse. Callers must alter the
    payload, then record its new truthful MD5, rather than forge the MD5 field.
    """
    if size != TRAILER_LEN:
        raise ValueError("only 0x60-byte IronMan trailers are supported")
    if len(decoded) != size:
        raise ValueError("decoded trailer must be exactly 0x60 bytes")

    rotated = _rotate_left_two(decoded)
    stride = ((rotated[-1] >> 2) & 0x0F) + 1
    encoded = _swap_pairs(rotated, stride)
    if qa_decode(encoded) != decoded:
        raise UnencodableTrailer(
            "the vendor trailer transform cannot represent this payload"
        )
    return encoded


def _ascii_decimal(value: int, field_name: str) -> bytes:
    if value < 0:
        raise ValueError(f"{field_name} cannot be negative")
    encoded = str(value).encode("ascii")
    if len(encoded) > 8:
        raise ValueError(f"{field_name} does not fit its 8-byte field")
    return encoded.ljust(8, b"\x00")


def _validate_md5(md5_hex: bytes) -> None:
    if not _LOWER_MD5_RE.fullmatch(md5_hex):
        raise ValueError("MD5 must be 32 lowercase ASCII hex characters")


def build_header(body_size: int, version_field: bytes, body_md5: bytes) -> bytes:
    """Build the clear-text 0x60-byte package header."""
    _validate_md5(body_md5)
    if len(version_field) > 48:
        raise ValueError("version field exceeds 48 bytes")

    header = bytearray(HEADER_LEN)
    header[0:8] = HEADER_MAGIC
    header[8:16] = _ascii_decimal(body_size, "body size")
    header[16:16 + len(version_field)] = version_field
    header[64:96] = body_md5
    return bytes(header)


def build_trailer(payload_size: int, payload_md5: bytes) -> bytes:
    """Build and encode the 0x60-byte payload trailer."""
    if payload_size >= PAYLOAD_LIMIT:
        raise ValueError(
            f"payload size {payload_size} must be lower than {PAYLOAD_LIMIT}"
        )
    _validate_md5(payload_md5)

    decoded = bytearray(TRAILER_LEN)
    decoded[0:8] = TRAILER_MAGIC
    decoded[8:16] = _ascii_decimal(payload_size, "payload size")
    decoded[64:96] = payload_md5
    return qa_encode(bytes(decoded))


def _validate_text_field(value: str, name: str) -> None:
    if not value or not value.isascii():
        raise ValueError(f"{name} must be non-empty ASCII")
    if any(character in value for character in (";", "=", "\x00")):
        raise ValueError(f"{name} contains an IronMan field delimiter")


def pad_payload(payload: bytes) -> tuple[bytes, int]:
    """Return payload bytes whose truthful MD5 has an encodable trailer.

    SquashFS records its own filesystem extent, so a deterministic suffix is
    ignored when the image is mounted. The returned integer is the suffix size.
    """
    if len(payload) >= PAYLOAD_LIMIT:
        raise ValueError(
            f"payload size {len(payload)} must be lower than {PAYLOAD_LIMIT}"
        )

    for counter in range(4096):
        suffix = b"" if counter == 0 else f"\n#jooan-pad/{counter}\n".encode("ascii")
        candidate = payload + suffix
        if len(candidate) >= PAYLOAD_LIMIT:
            raise ValueError("deterministic trailer padding exceeds payload limit")
        payload_md5 = _md5(candidate).encode("ascii")
        try:
            build_trailer(len(candidate), payload_md5)
        except UnencodableTrailer:
            continue
        return candidate, len(suffix)
    raise UnencodableTrailer("no encodable payload found in 4096 attempts")


def assemble_package(
    payload: bytes,
    *,
    model_token: str = DEFAULT_MODEL_TOKEN,
    firmware_version: str = DEFAULT_FIRMWARE_VERSION,
) -> tuple[bytes, int]:
    """Assemble a validated package and return ``(bytes, padding_size)``."""
    _validate_text_field(model_token, "model token")
    _validate_text_field(firmware_version, "firmware version")

    version_field = (
        f"ver={firmware_version};ProductName={model_token}".encode("ascii")
    )
    if len(version_field) > 48:
        raise ValueError("combined version/model field exceeds 48 bytes")

    padded_payload, padding_size = pad_payload(payload)
    payload_md5 = _md5(padded_payload).encode("ascii")
    trailer = build_trailer(len(padded_payload), payload_md5)
    body = padded_payload + trailer
    header = build_header(
        len(body), version_field, _md5(body).encode("ascii")
    )
    package = header + body
    inspect_package(package, expected_model=model_token)
    return package, padding_size


def _parse_decimal(field: bytes, field_name: str) -> int:
    raw = field.split(b"\x00", 1)[0]
    if not raw or not raw.isdigit():
        raise PackageValidationError(f"invalid {field_name}")
    return int(raw)


def _parse_version_field(field: bytes) -> tuple[str, str]:
    try:
        text = field.split(b"\x00", 1)[0].decode("ascii")
    except UnicodeDecodeError as error:
        raise PackageValidationError("version field is not ASCII") from error
    parts = text.split(";")
    if len(parts) < 2 or not parts[0].startswith("ver="):
        raise PackageValidationError("invalid IronMan version field")
    if not parts[1].startswith("ProductName="):
        raise PackageValidationError("missing ProductName model token")
    firmware_version = parts[0].split("=", 1)[1]
    model_token = parts[1].split("=", 1)[1]
    if not firmware_version or not model_token:
        raise PackageValidationError("empty firmware version or model token")
    return firmware_version, model_token


def inspect_package(
    package: bytes, *, expected_model: str | None = None
) -> PackageInfo:
    """Replay the device-side package checks and return normalized metadata."""
    if len(package) < HEADER_LEN + TRAILER_LEN:
        raise PackageValidationError("package is too short")
    if package[0:8] != HEADER_MAGIC:
        raise PackageValidationError("invalid package magic")

    body_size = _parse_decimal(package[8:16], "body size")
    if body_size + HEADER_LEN != len(package):
        raise PackageValidationError("body size does not match package size")
    firmware_version, model_token = _parse_version_field(package[16:64])
    if expected_model is not None and model_token != expected_model:
        raise PackageValidationError(
            f"model token {model_token!r} does not match {expected_model!r}"
        )

    recorded_body_md5 = package[64:96]
    try:
        _validate_md5(recorded_body_md5)
    except ValueError as error:
        raise PackageValidationError("invalid header MD5 field") from error
    actual_body_md5 = _md5(package[HEADER_LEN:])
    if recorded_body_md5.decode("ascii") != actual_body_md5:
        raise PackageValidationError("body MD5 mismatch")

    decoded_trailer = qa_decode(package[-TRAILER_LEN:])
    if decoded_trailer[0:8] != TRAILER_MAGIC:
        raise PackageValidationError("invalid decoded trailer magic")
    payload_size = _parse_decimal(decoded_trailer[8:16], "payload size")
    if payload_size >= PAYLOAD_LIMIT:
        raise PackageValidationError("payload exceeds the device limit")
    if payload_size + HEADER_LEN + TRAILER_LEN != len(package):
        raise PackageValidationError("payload size does not match package layout")

    recorded_payload_md5 = decoded_trailer[64:96]
    try:
        _validate_md5(recorded_payload_md5)
    except ValueError as error:
        raise PackageValidationError("invalid trailer MD5 field") from error
    payload = package[HEADER_LEN:HEADER_LEN + payload_size]
    actual_payload_md5 = _md5(payload)
    if recorded_payload_md5.decode("ascii") != actual_payload_md5:
        raise PackageValidationError("payload MD5 mismatch")

    return PackageInfo(
        model_token=model_token,
        firmware_version=firmware_version,
        package_size=len(package),
        payload_size=payload_size,
        payload_md5=actual_payload_md5,
        body_md5=actual_body_md5,
        package_sha256=hashlib.sha256(package).hexdigest(),
    )
