from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
import sys

sys.path.insert(0, str(REPOSITORY / "src"))

from ironman.package import build_package, validate_stage  # noqa: E402
from ironman.trailer import (  # noqa: E402
    HEADER_LEN,
    PAYLOAD_LIMIT,
    PackageValidationError,
    UnencodableTrailer,
    assemble_package,
    build_trailer,
    inspect_package,
    pad_payload,
    qa_decode,
    qa_encode,
)


def legacy_md5(data: bytes) -> bytes:
    return hashlib.md5(data, usedforsecurity=False).hexdigest().encode("ascii")


class TrailerCodecTests(unittest.TestCase):
    def test_encoded_trailer_round_trips(self) -> None:
        for size in range(128):
            payload = bytes([size]) * size
            padded, _ = pad_payload(payload)
            digest = legacy_md5(padded)
            encoded = build_trailer(len(padded), digest)
            decoded = qa_decode(encoded)
            self.assertEqual(decoded[0:5], b"toolv")
            self.assertEqual(int(decoded[8:16].split(b"\x00", 1)[0]), len(padded))
            self.assertEqual(decoded[64:96], digest)

    def test_known_public_trailer_vector(self) -> None:
        payload = b"public-vector"
        encoded = build_trailer(len(payload), legacy_md5(payload))
        self.assertEqual(
            hashlib.sha256(encoded).hexdigest(),
            "0922c49331e117d7a302b994daca8d11c7d00bb0a3575ada5ef82b061f5712aa",
        )

    def test_encoder_refuses_a_nonrepresentable_trailer(self) -> None:
        found = None
        for counter in range(4096):
            payload = f"unencodable-{counter}".encode("ascii")
            decoded = bytearray(0x60)
            decoded[0:8] = b"toolv\x00\x00\x00"
            decoded[8:16] = str(len(payload)).encode().ljust(8, b"\x00")
            decoded[64:96] = legacy_md5(payload)
            try:
                qa_encode(bytes(decoded))
            except UnencodableTrailer:
                found = bytes(decoded)
                break
        self.assertIsNotNone(found)
        with self.assertRaises(UnencodableTrailer):
            qa_encode(found)

    def test_invalid_codec_inputs_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            qa_decode(b"short")
        with self.assertRaises(ValueError):
            qa_encode(bytes(95))
        with self.assertRaises(ValueError):
            build_trailer(1, b"A" * 32)
        with self.assertRaises(ValueError):
            build_trailer(PAYLOAD_LIMIT, b"0" * 32)


class PackageCodecTests(unittest.TestCase):
    def test_package_is_deterministic_and_self_validating(self) -> None:
        payload = b"hsqs" + bytes(range(256)) * 17
        first, first_padding = assemble_package(payload)
        second, second_padding = assemble_package(payload)
        self.assertEqual(first, second)
        self.assertEqual(first_padding, second_padding)

        info = inspect_package(first, expected_model="A12")
        self.assertEqual(info.model_token, "A12")
        self.assertEqual(info.firmware_version, "05.02.31.115")
        self.assertEqual(info.package_size, len(first))
        self.assertEqual(info.payload_size + 0xC0, len(first))

    def test_package_padding_preserves_truthful_digest(self) -> None:
        selected = None
        for counter in range(4096):
            payload = f"needs-padding-{counter}".encode("ascii")
            try:
                build_trailer(len(payload), legacy_md5(payload))
            except UnencodableTrailer:
                selected = payload
                break
        self.assertIsNotNone(selected)

        package, padding_size = assemble_package(selected)
        self.assertGreater(padding_size, 0)
        info = inspect_package(package, expected_model="A12")
        payload = package[HEADER_LEN:HEADER_LEN + info.payload_size]
        self.assertEqual(info.payload_md5, legacy_md5(payload).decode("ascii"))
        self.assertTrue(payload.startswith(selected))

    def test_tampering_and_wrong_model_are_rejected(self) -> None:
        package, _ = assemble_package(b"payload")
        with self.assertRaises(PackageValidationError):
            inspect_package(package, expected_model="not-A12")

        tampered = bytearray(package)
        tampered[HEADER_LEN] ^= 0x01
        with self.assertRaises(PackageValidationError):
            inspect_package(bytes(tampered))

        bad_size = bytearray(package)
        bad_size[8:16] = b"1\x00\x00\x00\x00\x00\x00\x00"
        with self.assertRaises(PackageValidationError):
            inspect_package(bytes(bad_size))


class HostBuilderTests(unittest.TestCase):
    def test_target_manifest_is_exact_ja_a12(self) -> None:
        target = json.loads(
            (REPOSITORY / "packaging" / "targets" / "ja-a12.json").read_text()
        )
        self.assertEqual(target["device"]["model"], "JA-A12")
        self.assertEqual(target["ironman"]["model_token"], "A12")
        self.assertEqual(
            [sensor["driver"] for sensor in target["device"]["sensors"]],
            ["cv2005", "cv2005s1"],
        )
        self.assertEqual(target["device"]["radio"]["driver_family"], "skw6316")

    def test_stage_requires_executable_upgrade_script(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            stage = Path(temporary_dir)
            with self.assertRaises(ValueError):
                validate_stage(stage)
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o644)
            with self.assertRaises(ValueError):
                validate_stage(stage)
            os.chmod(upgrade, 0o755)
            validate_stage(stage)

    @unittest.skipUnless(shutil.which("mksquashfs"), "mksquashfs is not installed")
    def test_builder_rejects_output_inside_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            stage = Path(temporary_dir)
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o755)
            with self.assertRaisesRegex(ValueError, "must not be inside"):
                build_package(
                    stage,
                    stage / "JOOAN_FW_PKG",
                    model_token="A12",
                    firmware_version="05.02.31.115",
                )

    @unittest.skipUnless(shutil.which("mksquashfs"), "mksquashfs is not installed")
    def test_full_builder_is_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            stage = root / "stage"
            stage.mkdir()
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o755)
            (stage / "public.txt").write_text("local firmware\n", encoding="utf-8")

            first = root / "first.pkg"
            second = root / "second.pkg"
            first_info, _ = build_package(
                stage,
                first,
                model_token="A12",
                firmware_version="05.02.31.115",
            )
            second_info, _ = build_package(
                stage,
                second,
                model_token="A12",
                firmware_version="05.02.31.115",
            )
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(first_info, second_info)
            self.assertEqual(
                (root / "first.pkg.sha256").read_text().split()[0],
                first_info.package_sha256,
            )

    @unittest.skipUnless(shutil.which("mksquashfs"), "mksquashfs is not installed")
    def test_release_entrypoint_builds_install_and_uninstall(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            stages = []
            for name in ("install", "uninstall"):
                stage = root / name
                stage.mkdir()
                upgrade = stage / "upgrade.sh"
                upgrade.write_text(f"#!/bin/sh\n# {name}\nexit 0\n", encoding="utf-8")
                os.chmod(upgrade, 0o755)
                stages.append(stage)
            output = root / "dist"
            subprocess.run(
                [
                    str(REPOSITORY / "build.sh"),
                    "--release-version",
                    "test-1",
                    "--install-stage",
                    str(stages[0]),
                    "--uninstall-stage",
                    str(stages[1]),
                    "--out-dir",
                    str(output),
                ],
                cwd=REPOSITORY,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["release_version"], "test-1")
            self.assertEqual(set(manifest["packages"]), {"install", "uninstall"})
            self.assertEqual(manifest["target"]["device"]["model"], "JA-A12")
            for filename in ("JOOAN_FW_PKG", "JOOAN_UNINSTALL"):
                package = output / filename
                self.assertTrue(package.is_file())
                inspect_package(package.read_bytes(), expected_model="A12")


if __name__ == "__main__":
    unittest.main()
