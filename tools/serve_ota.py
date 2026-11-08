#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Serve one signed firmware package to a JA-A12 camera's vendor updater.

The stock JA-A12 web updater is a cloud *pull*, not an upload: when triggered it
downloads `https://use1api.jooaniot.com/fw/JOOAN_FW_PKG` over TLS. The camera
does not validate the certificate, so a self-signed one works. This server
answers exactly that one path with the package you pass and 404s everything
else, which is all a local install needs.

It does NOT impersonate the cloud API, forward anything upstream, speak MQTT, or
log anything about the device. It is the minimal, self-contained delivery side
of an over-the-air install.

Two things must be true for the camera to reach this server (see
docs/INSTALL.md, "Over-the-air delivery"):

  1. `use1api.jooaniot.com` must resolve to the machine running this server, for
     the camera. The camera ignores DHCP-provided DNS and uses hard-coded public
     resolvers (223.6.6.6, 8.8.8.8), so a normal "set your DNS" override does not
     work: the redirect must happen in the network path -- a router that answers
     that name for the camera, or that redirects the camera's port 53 to a
     resolver you control. docs/INSTALL.md shows both.
  2. This server must be reachable on TCP 443 from the camera's network.

Usage:
  python3 tools/serve_ota.py --pkg /path/to/JOOAN_FW_PKG
  # optional: --bind 0.0.0.0 --port 443 --cert cert.pem --key key.pem
  # with no --cert/--key it generates a throwaway self-signed pair for
  # use1api.jooaniot.com (needs the `openssl` program, or the `cryptography`
  # module) and prints where it put them.
"""
from __future__ import annotations

import argparse
import http.server
import os
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

SERVED_PATH = "/fw/JOOAN_FW_PKG"
CLOUD_HOST = "use1api.jooaniot.com"


def make_self_signed(directory: Path) -> tuple[Path, Path]:
    """A throwaway cert/key for CLOUD_HOST. The camera does not validate it."""
    cert = directory / "cert.pem"
    key = directory / "key.pem"
    # Prefer openssl (ubiquitous); fall back to the cryptography module.
    openssl = _which("openssl")
    if openssl:
        subprocess.run(
            [openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-days", "3", "-keyout", str(key), "-out", str(cert),
             "-subj", f"/CN={CLOUD_HOST}",
             "-addext", f"subjectAltName=DNS:{CLOUD_HOST}"],
            check=True, capture_output=True,
        )
        return cert, key
    try:
        _cryptography_self_signed(cert, key)
        return cert, key
    except Exception as exc:  # pragma: no cover - environment dependent
        raise SystemExit(
            "need either the `openssl` program or the Python `cryptography` "
            f"module to make a certificate ({exc}); or pass --cert/--key"
        )


def _which(program: str) -> str | None:
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = os.path.join(directory, program)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def _cryptography_self_signed(cert: Path, key: Path) -> None:
    import datetime
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID

    private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, CLOUD_HOST)])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name)
        .public_key(private.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=3))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName(CLOUD_HOST)]), False)
        .sign(private, hashes.SHA256())
    )
    key.write_bytes(private.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    ))
    cert.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))


def make_handler(package: bytes):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):  # noqa: N802
            if self.path.split("?")[0] == SERVED_PATH:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(package)))
                self.end_headers()
                self.wfile.write(package)
                print(f"[served] {self.path} -> {len(package)} bytes", flush=True)
            else:
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                print(f"[404] {self.command} {self.path}", flush=True)

        do_HEAD = do_GET

        def log_message(self, *args):
            pass

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pkg", required=True, type=Path, help="path to JOOAN_FW_PKG")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=443)
    parser.add_argument("--cert", type=Path, help="TLS cert (PEM); generated if omitted")
    parser.add_argument("--key", type=Path, help="TLS key (PEM); generated if omitted")
    args = parser.parse_args()

    if not args.pkg.is_file():
        raise SystemExit(f"package not found: {args.pkg}")
    package = args.pkg.read_bytes()
    if package[:5] != b"jooan":
        raise SystemExit("that file is not an IronMan package (missing 'jooan' magic)")

    tempdir = None
    if args.cert and args.key:
        cert, key = args.cert, args.key
    else:
        tempdir = tempfile.TemporaryDirectory(prefix="jooan-ota-cert-")
        cert, key = make_self_signed(Path(tempdir.name))
        print(f"[cert] self-signed for {CLOUD_HOST} in {tempdir.name}", flush=True)

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(cert), str(key))
    server = http.server.ThreadingHTTPServer((args.bind, args.port),
                                             make_handler(package))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"[ready] https://{args.bind}:{args.port}{SERVED_PATH}", flush=True)
    print(f"        the camera must resolve {CLOUD_HOST} to this machine "
          "(see docs/INSTALL.md)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[stopped]", flush=True)
    finally:
        if tempdir:
            tempdir.cleanup()
    return 0


if __name__ == "__main__":
    sys.exit(main())
