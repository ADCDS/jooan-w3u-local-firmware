#!/usr/bin/env python3
import hashlib
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "src/daemon/joan-daemon"
HELPER = ROOT / "tools/tests/fake-integration.sh"
PASSWORD = "admin"  # hygiene: allow-test-vector
REALM = "joan-rtsp"


def free_port():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    port = listener.getsockname()[1]
    listener.close()
    return port


def receive_header(stream):
    data = b""
    while b"\r\n\r\n" not in data:
        part = stream.recv(4096)
        if not part:
            break
        data += part
        assert len(data) <= 16384
    return data


class Upstream:
    def __init__(self, port):
        self.port = port
        self.stop = threading.Event()
        self.requests = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def client(self, stream):
        try:
            request = receive_header(stream)
            if request:
                with self.lock:
                    self.requests.append(request)
                cseq = b"1"
                for line in request.split(b"\r\n"):
                    if line.lower().startswith(b"cseq:"):
                        cseq = line.split(b":", 1)[1].strip()
                stream.sendall(
                    b"RTSP/1.0 200 OK\r\nCSeq: "
                    + cseq
                    + b"\r\nX-Joan-Upstream: yes\r\nContent-Length: 0\r\n\r\n"
                )
        finally:
            stream.close()

    def run(self):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", self.port))
        listener.listen(16)
        listener.settimeout(0.1)
        try:
            while not self.stop.is_set():
                try:
                    stream, _ = listener.accept()
                except socket.timeout:
                    continue
                threading.Thread(target=self.client, args=(stream,), daemon=True).start()
        finally:
            listener.close()

    def close(self):
        self.stop.set()
        self.thread.join(2)


def request_without_auth(stream, cseq="1", fragmented=False):
    request = (
        f"OPTIONS rtsp://camera.local/live/ch0 RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n\r\n"
    ).encode()
    if fragmented:
        for cut in (1, 7, 19, len(request)):
            stream.sendall(request[:cut])
            request = request[cut:]
            if not request:
                break
            time.sleep(0.01)
    else:
        stream.sendall(request)
    reply = receive_header(stream)
    assert reply.startswith(b"RTSP/1.0 401 Unauthorized"), reply
    assert f"CSeq: {cseq}\r\n".encode() in reply
    match = re.search(rb'nonce="([0-9a-f]{32})"', reply)
    assert match and b'qop="auth"' in reply
    return match.group(1).decode()


def authorization(method, uri, nonce, response_override=None):
    nc = "00000001"
    cnonce = "0011223344556677"
    ha1 = hashlib.md5(f"admin:{REALM}:{PASSWORD}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    response = hashlib.md5(
        f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}".encode()
    ).hexdigest()
    if response_override is not None:
        response = response_override
    return (
        f'Authorization: Digest username="admin", realm="{REALM}", '
        f'nonce="{nonce}", uri="{uri}", response="{response}", '
        f"algorithm=MD5, qop=auth, nc={nc}, cnonce=\"{cnonce}\"\r\n"
    )


def authenticated_request(stream, nonce, cseq="2", wrong=False, fragmented=False):
    method = "DESCRIBE"
    uri = "rtsp://camera.local/live/ch0"
    auth = authorization(method, uri, nonce, "0" * 32 if wrong else None)
    request = (
        f"{method} {uri} RTSP/1.0\r\nCSeq: {cseq}\r\n{auth}\r\n"
    ).encode()
    if fragmented:
        for offset in range(0, len(request), 13):
            stream.sendall(request[offset : offset + 13])
            time.sleep(0.002)
    else:
        stream.sendall(request)
    return receive_header(stream)


upstream_port, proxy_port, http_port = free_port(), free_port(), free_port()
upstream = Upstream(upstream_port)
upstream.start()
with tempfile.TemporaryDirectory() as state, tempfile.TemporaryDirectory() as staging:
    environment = os.environ | {
        "JOAN_STATE_DIR": state,
        "JOAN_STAGING_DIR": staging,
        "JOAN_WEB_DIR": str(ROOT / "web"),
        "JOAN_INTEGRATION_HELPER": str(HELPER),
        "JOAN_PORT": str(http_port),
        "JOAN_REDIRECT_PORT": "0",
        "JOAN_MQTT_PORT": "0",
        "JOAN_RTSP_PORT": str(upstream_port),
        "JOAN_RTSP_PROXY_PORT": str(proxy_port),
        "JOAN_MDNS": "0",
    }
    process = subprocess.Popen(
        [BIN, "--plain-http", "--bind", "127.0.0.1"],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        for _ in range(100):
            try:
                probe = socket.create_connection(("127.0.0.1", proxy_port), 0.2)
                probe.close()
                break
            except OSError:
                time.sleep(0.03)
        else:
            raise AssertionError("RTSP proxy did not listen")

        client = socket.create_connection(("127.0.0.1", proxy_port), 2)
        client.settimeout(2)
        nonce = request_without_auth(client, fragmented=True)
        wrong = authenticated_request(client, nonce, cseq="2", wrong=True)
        assert wrong.startswith(b"RTSP/1.0 401 Unauthorized"), wrong
        good = authenticated_request(client, nonce, cseq="3", fragmented=True)
        assert good.startswith(b"RTSP/1.0 200 OK"), good
        assert b"X-Joan-Upstream: yes" in good
        client.close()

        with upstream.lock:
            forwarded = [r for r in upstream.requests if b"camera.local/live/ch0" in r]
        assert forwarded and b"Authorization: Digest" in forwarded[-1]

        oversized = socket.create_connection(("127.0.0.1", proxy_port), 2)
        oversized.settimeout(2)
        oversized.sendall(b"X" * 8192)
        assert receive_header(oversized).startswith(b"RTSP/1.0 400 Bad Request")
        oversized.close()

        held = []
        for _ in range(8):
            stream = socket.create_connection(("127.0.0.1", proxy_port), 2)
            stream.sendall(b"O")
            held.append(stream)
        time.sleep(0.1)
        overflow = socket.create_connection(("127.0.0.1", proxy_port), 2)
        overflow.settimeout(2)
        assert receive_header(overflow).startswith(b"RTSP/1.0 453 Not Enough Bandwidth")
        overflow.close()
        held.pop().close()
        for stream in held:
            stream.close()
        time.sleep(0.1)

        recovered = socket.create_connection(("127.0.0.1", proxy_port), 2)
        recovered.settimeout(2)
        assert request_without_auth(recovered)
        recovered.close()
        print("PASS: RTSP Digest proxy authentication, fragmentation, bounds and workers")
    finally:
        process.terminate()
        try:
            process.wait(3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(3)
upstream.close()
