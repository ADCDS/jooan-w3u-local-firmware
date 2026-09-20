from __future__ import annotations

import importlib.util
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "src"))

from ironman.signing import public_key_bytes, public_key_id, sign_bytes
from ironman.trailer import assemble_package


def load_uploader():
    path = REPOSITORY / "tools" / "upload_ota.py"
    specification = importlib.util.spec_from_file_location("jooan_upload_ota", path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class UploadTransportTests(unittest.TestCase):
    def test_multipart_body_is_exact_and_binary_safe(self) -> None:
        uploader = load_uploader()
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "JOOAN_FW_PKG"
            package.write_bytes(b"jooan\x00\xffpayload")
            body = uploader.multipart(package, "test-boundary")

        self.assertTrue(body.startswith(b"--test-boundary\r\n"))
        self.assertIn(b'name="filename"; filename="JOOAN_FW_PKG"', body)
        self.assertIn(b"\r\n\r\njooan\x00\xffpayload\r\n", body)
        self.assertTrue(body.endswith(b"--test-boundary--\r\n"))

    def test_multipart_rejects_header_injection_filename(self) -> None:
        uploader = load_uploader()
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / 'bad"name'
            package.write_bytes(b"data")
            with self.assertRaises(ValueError):
                uploader.multipart(package, "boundary")

    def test_signed_release_index_binds_package_version_and_sequence(self) -> None:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec

        uploader = load_uploader()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private = ec.generate_private_key(ec.SECP256R1())
            key = root / "key.pem"
            key.write_bytes(private.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.TraditionalOpenSSL,
                serialization.NoEncryption(),
            ))
            os.chmod(key, 0o600)
            public = public_key_bytes(private.public_key()).hex()
            target_data = json.loads(
                (REPOSITORY / "packaging/targets/ja-a12.json").read_text()
            )
            target_data["release_authenticity"]["public_key_sec1"] = public
            target_data["release_authenticity"]["key_id"] = public_key_id(public)
            target = root / "target.json"
            target.write_text(json.dumps(target_data), encoding="utf-8")

            package = root / "JOOAN_FW_PKG"
            blob, _ = assemble_package(b"synthetic signed package")
            package.write_bytes(blob)
            package_hash = hashlib.sha256(blob).hexdigest()
            manifest_data = {
                "authenticity": target_data["release_authenticity"],
                "release_ready": True,
                "release_version": "0.2.0",
                "target": target_data,
                "packages": {
                    "install": {
                        "artifact_kind": "install",
                        "file": package.name,
                        "model_token": "A12",
                        "package_sha256": package_hash,
                        "package_size": len(blob),
                        "release_sequence": 2,
                    }
                },
            }
            manifest = root / "manifest.json"
            manifest.write_text(
                json.dumps(manifest_data, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            signature = root / "manifest.json.sig"
            signature.write_bytes(sign_bytes(manifest.read_bytes(), key))
            self.assertEqual(
                uploader.verify_release_index(package, manifest, signature, target),
                ("0.2.0", 2),
            )
            package.write_bytes(blob + b"tamper")
            with self.assertRaises(ValueError):
                uploader.verify_release_index(package, manifest, signature, target)

            package.write_bytes(blob)
            manifest_data["target"] = dict(target_data)
            manifest_data["target"]["target_id"] = "wrong-camera"
            manifest.write_text(
                json.dumps(manifest_data, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            signature.write_bytes(sign_bytes(manifest.read_bytes(), key))
            with self.assertRaisesRegex(ValueError, "target/ABI"):
                uploader.verify_release_index(package, manifest, signature, target)


if __name__ == "__main__":
    unittest.main()
