# SPDX-License-Identifier: GPL-2.0-only
from __future__ import annotations

import importlib.util
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import unittest
from unittest import mock


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


class FakeCamera:
    """A local server standing in for the stock firmware or for the platform."""

    def __init__(self, get=(404, b"<html>not found</html>"), post=(200, b"Done...rebooting"), close=False):
        outer = self
        self.posts = 0

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def respond(self, reply):
                self.send_response(reply[0])
                self.send_header("Content-Length", str(len(reply[1])))
                self.end_headers()
                self.wfile.write(reply[1])

            def do_GET(self):
                self.respond(get)

            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                outer.posts += 1
                if close:  # as the camera does when it reboots before answering
                    self.close_connection = True
                    self.connection.close()
                    return
                self.respond(post)

        self.server = HTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.server.shutdown()
        self.server.server_close()


def unused_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


PLATFORM = json.dumps({"release_version": "0.2.36", "release_sequence": 70}).encode()


class CameraStateTests(unittest.TestCase):
    def run_tool(self, camera_port: int, version: str = "0.3.0", sequence: int = 70) -> int:
        uploader = load_uploader()
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary) / "JOOAN_FW_PKG"
            package.write_bytes(b"package")
            argv = ["upload_ota.py", "--host", "127.0.0.1", "--port", str(camera_port), "--pkg", str(package), "--timeout", "3"]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                uploader, "verify_release_index", return_value=(version, sequence)
            ):
                return uploader.main()

    def test_the_stock_firmware_is_recognised(self) -> None:
        uploader = load_uploader()
        with FakeCamera() as camera:
            self.assertIsNone(uploader.running_release("127.0.0.1", camera.port))
        with FakeCamera(get=(200, b"not json")) as camera:
            self.assertIsNone(uploader.running_release("127.0.0.1", camera.port))
        self.assertIsNone(uploader.running_release("127.0.0.1", unused_port()))
        with FakeCamera(get=(200, PLATFORM)) as camera:
            self.assertEqual(uploader.running_release("127.0.0.1", camera.port)["release_sequence"], 70)

    def test_a_camera_with_the_platform_gets_no_upload(self) -> None:
        # This used to end "active" for the same release without anything having been sent.
        with FakeCamera(get=(200, PLATFORM)) as camera:
            self.assertEqual(self.run_tool(camera.port), 2)
            self.assertEqual(camera.posts, 0)
        with FakeCamera(get=(200, PLATFORM)) as camera:
            self.assertEqual(self.run_tool(camera.port, "0.2.36", 70), 0)
            self.assertEqual(camera.posts, 0)

    def test_nothing_listening_is_a_failure_at_once(self) -> None:
        # This used to count as accepted, then wait out the whole timeout.
        self.assertEqual(self.run_tool(unused_port()), 2)

    def test_what_the_stock_firmware_says(self) -> None:
        uploader = load_uploader()
        with FakeCamera() as camera:
            self.assertEqual(uploader.send_package("127.0.0.1", camera.port, 3, b"x", "b"), "done")
        with FakeCamera(post=(500, b"no")) as camera:
            self.assertEqual(uploader.send_package("127.0.0.1", camera.port, 3, b"x", "b"), "failed")
        with FakeCamera(close=True) as camera:
            self.assertEqual(uploader.send_package("127.0.0.1", camera.port, 3, b"x", "b"), "ended")
            self.assertEqual(camera.posts, 1)
        self.assertEqual(uploader.send_package("127.0.0.1", unused_port(), 3, b"x", "b"), "failed")

    def test_an_upload_only_counts_with_the_new_release_running(self) -> None:
        with FakeCamera(close=True) as camera:
            self.assertEqual(self.run_tool(camera.port), 3)
            self.assertEqual(camera.posts, 1)


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
