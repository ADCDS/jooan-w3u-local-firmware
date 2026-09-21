#!/usr/bin/env python3
"""A camera must answer to the name on the certificate it serves.

Enrolling a certificate exists so the camera can be reached over a real DNS
name instead of its `.local`. That only works if the same name is accepted as
its own origin: otherwise the page loads and every state-changing request is
refused as cross-origin, which looks like a broken console rather than a
rejected origin.

Needs TLS, so it cannot live in the plain-HTTP daemon suite.

    tools/tests/test_cert_origin.py [path-to-joan-daemon]
"""
import http.client
import json
import os
import pathlib
import ssl
import subprocess
import sys
import tempfile
import time

BIN = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "src/daemon/joan-daemon").resolve()
NAME = "cam.test"
PORT = 18449


def openssl(*args):
    subprocess.run(["openssl", *args], check=True, capture_output=True)


def issue(tmp):
    """A private CA and a leaf for NAME, standing in for a public issuer."""
    # keyUsage=keyCertSign is not optional: a strict verifier rejects a CA
    # without it, and the failure names the CA rather than the leaf.
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/ca.key",
            "-out", f"{tmp}/ca.crt", "-days", "3650", "-subj", "/CN=Test CA",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{tmp}/l.key",
            "-out", f"{tmp}/l.csr", "-subj", f"/CN={NAME}")
    pathlib.Path(f"{tmp}/ext").write_text(
        f"subjectAltName=DNS:{NAME}\nbasicConstraints=CA:FALSE\n")
    openssl("x509", "-req", "-in", f"{tmp}/l.csr", "-CA", f"{tmp}/ca.crt", "-CAkey",
            f"{tmp}/ca.key", "-CAcreateserial", "-out", f"{tmp}/l.crt", "-days", "30",
            "-extfile", f"{tmp}/ext")


class Connection(http.client.HTTPSConnection):
    """Reach the daemon on the loopback while presenting the real name, so the
    certificate verifies for what it was issued to rather than for an address."""

    def connect(self):
        import socket
        sock = socket.create_connection(("127.0.0.1", PORT), timeout=self.timeout)
        self.sock = self._context.wrap_socket(sock, server_hostname=NAME)


def request(ctx, method, path, body=None, cookie="", csrf="", origin=None):
    conn = Connection(NAME, PORT, context=ctx, timeout=12)
    headers = {"Content-Type": "application/json", "Host": NAME}
    if origin:
        headers["Origin"] = origin
    if cookie:
        headers["Cookie"] = cookie
    if csrf:
        headers["X-CSRF-Token"] = csrf
    conn.request(method, path, body=json.dumps(body) if body else None, headers=headers)
    r = conn.getresponse()
    return r.status, dict(r.getheaders()), r.read()


def main():
    tmp = tempfile.mkdtemp()
    issue(tmp)
    state, staging = f"{tmp}/state", f"{tmp}/staging"
    os.makedirs(state)
    os.makedirs(staging)
    # Seed the identity the way an enrolment leaves it, then start: the names
    # are read from the certificate the listener actually serves.
    pathlib.Path(f"{state}/tls-key.pem").write_text(pathlib.Path(f"{tmp}/l.key").read_text())
    pathlib.Path(f"{state}/tls-cert.pem").write_text(pathlib.Path(f"{tmp}/l.crt").read_text())
    pathlib.Path(f"{state}/tls-enrolled").write_text("enrolled\n")

    env = dict(os.environ, JOAN_STATE_DIR=state, JOAN_STAGING_DIR=staging,
               JOAN_WEB_DIR="web", JOAN_MQTT_PORT="0", JOAN_RTSP_PORT="18578",
               JOAN_RTSP_PROXY_PORT="0", JOAN_MDNS="0", JOAN_REDIRECT_PORT="0")
    proc = subprocess.Popen([str(BIN), "--bind", "127.0.0.1", "--port", str(PORT)],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        ctx = ssl.create_default_context(cafile=f"{tmp}/ca.crt")
        last = None
        for _ in range(50):
            try:
                request(ctx, "GET", "/api/v1/setup/status")
                break
            except Exception as exc:
                last = exc
                time.sleep(0.2)
        else:
            proc.terminate()
            out = proc.stdout.read().decode(errors="replace") if proc.stdout else ""
            raise SystemExit(f"daemon did not come up: {last!r}\n" + out[-800:])

        origin = f"https://{NAME}"
        status, headers, body = request(ctx, "POST", "/api/v1/session",
                                        {"username": "admin", "password": "admin"},
                                        origin=origin)
        assert status == 200, (status, body)
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        csrf = json.loads(body)["csrf"]

        # The certificate's own name: accepted.
        status, _, body = request(ctx, "PUT", "/api/v1/network/mdns", {"hostname": "viacert"},
                                  cookie, csrf, origin=origin)
        assert status == 200, ("cert name must be accepted as its own origin", status, body)

        # Anything else: still refused, so this did not become an open door.
        status, _, body = request(ctx, "PUT", "/api/v1/network/mdns", {"hostname": "nope"},
                                  cookie, csrf, origin="https://evil.invalid")
        assert status == 403, ("a foreign origin must still be refused", status, body)

        print("test_cert_origin: PASS")
    finally:
        proc.terminate()
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
