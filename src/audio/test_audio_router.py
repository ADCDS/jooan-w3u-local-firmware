#!/usr/bin/env python3
import base64
import hashlib
import os
import socket
import struct
import subprocess
import tempfile
import time


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
KEY = "dGhlIHNhbXBsZSBub25jZQ=="
TOKEN = "ABCDEFGHIJKLMNOP"  # hygiene: allow-test-vector


def wait_path(path):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.01)
    raise AssertionError(f"socket did not appear: {path}")


def websocket_connect(path, route="/ws/audio/talk", protocols=True):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(2)
    client.connect(path)
    protocol = (
        f"Sec-WebSocket-Protocol: jaud.v1, jaud.auth.{TOKEN}\r\n"
        if protocols else "Sec-WebSocket-Protocol: jaud.v1\r\n"
    )
    request = (
        f"GET {route} HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        f"Sec-WebSocket-Key: {KEY}\r\n"
        f"{protocol}\r\n"
    ).encode()
    client.sendall(request)
    response = b""
    while b"\r\n\r\n" not in response:
        block = client.recv(1024)
        if not block:
            break
        response += block
    if protocols:
        expected = base64.b64encode(hashlib.sha1((KEY + GUID).encode()).digest())
        assert response.startswith(b"HTTP/1.1 101 "), response
        assert b"Sec-WebSocket-Accept: " + expected + b"\r\n" in response
        assert b"Sec-WebSocket-Protocol: jaud.v1\r\n" in response
    return client, response


def jaud(kind, sequence, payload=b""):
    return struct.pack("!4sBBBBIHH", b"JAUD", 1, kind, 0, 16,
                       sequence, len(payload), 0) + payload


def client_frame(payload, opcode=2, masked=True):
    first = 0x80 | opcode
    length = len(payload)
    marker = 0x80 if masked else 0
    if length <= 125:
        header = bytes((first, marker | length))
    else:
        header = bytes((first, marker | 126)) + struct.pack("!H", length)
    if not masked:
        return header + payload
    mask = b"\x12\x34\x56\x78"
    encoded = bytes(value ^ mask[index & 3]
                    for index, value in enumerate(payload))
    return header + mask + encoded


def receive_server_frame(client):
    header = client.recv(2)
    assert len(header) == 2 and header[0] == 0x82 and not header[1] & 0x80
    length = header[1] & 0x7f
    if length == 126:
        length = struct.unpack("!H", client.recv(2))[0]
    payload = b""
    while len(payload) < length:
        payload += client.recv(length - len(payload))
    return payload


def receive_guard(receiver, expected_type, expected_sequence):
    packet = receiver.recv(2048)
    assert len(packet) in (24, 344)
    magic, version, kind, flags, session, sequence, length, reserved = \
        struct.unpack("!4sBBHQIHH", packet[:24])
    assert magic == b"JAGD" and version == 1 and flags == 0 and reserved == 0
    assert session != 0 and kind == expected_type and sequence == expected_sequence
    assert len(packet) == 24 + length
    return session, packet[24:]


def main():
    with tempfile.TemporaryDirectory(prefix="jooan-audio-router-") as directory:
        ws_path = os.path.join(directory, "ws.sock")
        mic_path = os.path.join(directory, "mic.sock")
        guard_path = os.path.join(directory, "guard.sock")
        guard = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        guard.settimeout(2)
        guard.bind(guard_path)
        process = subprocess.Popen([
            "./jooan-audio-router",
            "--ws-socket", ws_path,
            "--mic-socket", mic_path,
            "--guard-socket", guard_path,
        ], cwd=os.path.dirname(__file__), stdout=subprocess.PIPE,
           stderr=subprocess.PIPE)
        try:
            wait_path(ws_path)
            wait_path(mic_path)

            # Full PTT lifecycle.
            client, _ = websocket_connect(ws_path)
            pcma = bytes([0xD5]) * 320
            client.sendall(client_frame(jaud(1, 1)))
            session, _ = receive_guard(guard, 1, 1)
            client.sendall(client_frame(jaud(2, 2, pcma)))
            same_session, forwarded = receive_guard(guard, 2, 2)
            assert same_session == session and forwarded == pcma
            client.sendall(client_frame(jaud(3, 3)))
            same_session, _ = receive_guard(guard, 3, 3)
            assert same_session == session
            client.close()

            # Disconnect while holding talk must synthesize RELEASE.
            client, _ = websocket_connect(ws_path)
            client.sendall(client_frame(jaud(1, 1)))
            session, _ = receive_guard(guard, 1, 1)
            client.close()
            same_session, _ = receive_guard(guard, 3, 2)
            assert same_session == session

            # Protocol error while holding talk must also release.
            client, _ = websocket_connect(ws_path)
            client.sendall(client_frame(jaud(1, 1)))
            session, _ = receive_guard(guard, 1, 1)
            client.sendall(client_frame(jaud(2, 2, pcma), masked=False))
            same_session, _ = receive_guard(guard, 3, 2)
            assert same_session == session
            assert client.recv(1) == b""
            client.close()

            # Guard microphone datagram becomes an unmasked server JAUD frame.
            listener, _ = websocket_connect(ws_path, "/ws/audio/mic")
            producer = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            producer.connect(mic_path)
            # The guard may have dropped earlier datagrams before the router
            # existed, so the first observed sequence need not be one.
            jagm = struct.pack("!4sBBHI", b"JAGM", 1, 12, 320, 17) + pcma
            producer.send(jagm)
            downlink = receive_server_frame(listener)
            magic, version, kind, flags, header_size, sequence, length, reserved = \
                struct.unpack("!4sBBBBIHH", downlink[:16])
            assert (magic, version, kind, flags, header_size) == \
                   (b"JAUD", 1, 0x81, 0, 16)
            assert sequence == 1 and length == 320 and reserved == 0
            assert downlink[16:] == pcma
            producer.close()
            listener.close()

            # Missing authentication subprotocol fails the upgrade.
            invalid, response = websocket_connect(ws_path, protocols=False)
            assert not response.startswith(b"HTTP/1.1 101 ")
            invalid.close()
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
            stdout, stderr = process.communicate()
            assert process.returncode in (0, -15), (stdout, stderr)
            guard.close()
    print("audio-router integration: PASS")


if __name__ == "__main__":
    main()
