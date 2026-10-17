#!/usr/bin/env python3
"""Protocol checks against a running replay or hardware daemon (stdlib only).

Example: python3 tests/test_rtsp.py --port 8554 --username admin --password example
The caller owns daemon startup/shutdown. These tests never access the camera.
"""
import argparse
import base64
import hashlib
import re
import socket
import struct
import time
import unittest


def md5(text):
    return hashlib.md5(text.encode(), usedforsecurity=False).hexdigest()


class Client:
    def __init__(self, settings):
        self.settings = settings
        self.sock = socket.create_connection((settings.host, settings.port), 3)
        self.sock.settimeout(4)
        self.uri = f"rtsp://{settings.host}:{settings.port}/stream0"
        self.buffer = bytearray()
        self.packets = []
        self.cseq = 0
        self.nc = 0
        self.challenge = None
        self.session = None

    def close(self):
        self.sock.close()

    def take(self, size):
        while len(self.buffer) < size:
            data = self.sock.recv(65536)
            if not data:
                raise EOFError("RTSP connection closed")
            self.buffer.extend(data)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def message(self):
        while True:
            first = self.take(1)
            if first == b"$":
                header = self.take(3)
                channel, size = header[0], int.from_bytes(header[1:], "big")
                self.packets.append((channel, self.take(size)))
                continue
            header = bytearray(first)
            while not header.endswith(b"\r\n\r\n"):
                header.extend(self.take(1))
                if len(header) > 16384:
                    raise ValueError("oversize RTSP response")
            lines = header.decode("ascii").split("\r\n")
            code = int(lines[0].split()[1])
            fields = {}
            for line in lines[1:]:
                if line:
                    key, value = line.split(":", 1)
                    fields[key.lower()] = value.strip()
            body = self.take(int(fields.get("content-length", "0")))
            return code, fields, body

    def authorization(self, method, uri, legacy=False):
        if self.challenge is None:
            return None
        realm, nonce = self.challenge
        self.nc += 1
        nc, cnonce = f"{self.nc:08x}", "jipc-protocol-test"
        ha1 = md5(f"{self.settings.username}:{realm}:{self.settings.password}")
        ha2 = md5(f"{method}:{uri}")
        answer = md5(f"{ha1}:{nonce}:{ha2}" if legacy else
                     f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}")
        auth = (f'Digest username="{self.settings.username}", realm="{realm}", '
                f'nonce="{nonce}", uri="{uri}", response="{answer}", algorithm=MD5')
        if not legacy:
            auth += f', qop=auth, nc={nc}, cnonce="{cnonce}"'
        return auth

    def encoded(self, method, uri=None, headers=None, body=b"", authorization=None):
        self.cseq += 1
        lines = [f"{method} {uri or self.uri} RTSP/1.0", f"CSeq: {self.cseq}"]
        if authorization:
            lines.append(f"Authorization: {authorization}")
        if self.session:
            lines.append(f"Session: {self.session}")
        for key, value in (headers or {}).items():
            lines.append(f"{key}: {value}")
        lines.append(f"Content-Length: {len(body)}")
        return ("\r\n".join(lines) + "\r\n\r\n").encode() + body

    def request(self, method, uri=None, headers=None, body=b"", legacy=False):
        uri = uri or self.uri
        auth = self.authorization(method, uri, legacy)
        self.sock.sendall(self.encoded(method, uri, headers, body, auth))
        result = self.message()
        if result[0] == 401 and self.challenge is None:
            challenge = result[1]["www-authenticate"]
            realm = re.search(r'realm="([^"]*)"', challenge).group(1)
            nonce = re.search(r'nonce="([^"]*)"', challenge).group(1)
            self.challenge = realm, nonce
            return self.request(method, uri, headers, body, legacy)
        if "session" in result[1]:
            self.session = result[1]["session"].split(";", 1)[0]
        return result

    def next_packet(self, deadline):
        if self.packets:
            return self.packets.pop(0)
        self.sock.settimeout(max(0.05, deadline - time.monotonic()))
        if self.take(1) != b"$":
            raise AssertionError("unexpected control response while collecting RTP")
        header = self.take(3)
        return header[0], self.take(int.from_bytes(header[1:], "big"))


class ProtocolTests(unittest.TestCase):
    settings = None

    def setUp(self):
        if self.settings is None:
            self.skipTest("run test_rtsp.py with daemon connection arguments")
        self.client = Client(self.settings)
        self.addCleanup(self.client.close)

    def test_options_and_digest(self):
        client = self.client
        code, fields, _ = client.request("OPTIONS")
        self.assertEqual(code, 200)
        self.assertIn("DESCRIBE", fields["public"])
        code, fields, body = client.request("DESCRIBE", headers={"Accept": "application/sdp"})
        self.assertEqual(code, 200, body)
        self.assertEqual(fields["content-type"], "application/sdp")
        self.assertIn(b"H264/90000", body)
        parameters = re.search(rb"sprop-parameter-sets=([^,\r]+),([^\r]+)", body)
        self.assertIsNotNone(parameters)
        self.assertEqual(base64.b64decode(parameters[1])[0] & 31, 7)
        self.assertEqual(base64.b64decode(parameters[2])[0] & 31, 8)
        if self.settings.audio:
            self.assertIn(b"PCMA/8000/1", body)
        auth = client.authorization("DESCRIBE", client.uri)
        client.sock.sendall(client.encoded("DESCRIBE", authorization=auth))
        self.assertEqual(client.message()[0], 200)
        client.sock.sendall(client.encoded("DESCRIBE", authorization=auth))
        self.assertEqual(client.message()[0], 401, "digest nonce-count replay was accepted")

    def test_legacy_digest_and_aliases(self):
        for path in ("/live/ch0", "/live/0", "/stream0"):
            uri = self.client.uri.rsplit("/", 1)[0] + path
            self.assertEqual(self.client.request("DESCRIBE", uri, legacy=True)[0], 200)

    def test_request_body_and_pipeline(self):
        client = self.client
        self.assertEqual(client.request("DESCRIBE")[0], 200)
        first = client.encoded("GET_PARAMETER", body=b"test\r\n\r\nbody",
                               authorization=client.authorization("GET_PARAMETER", client.uri))
        second = client.encoded("OPTIONS")
        # Exercise split headers, a complete request body and pipelining together.
        client.sock.sendall(first[:13])
        client.sock.sendall(first[13:] + second)
        self.assertEqual(client.message()[0], 200)
        self.assertEqual(client.message()[0], 200)

    def test_tcp_rtp_and_rtcp(self):
        client = self.client
        self.assertEqual(client.request("DESCRIBE")[0], 200)
        code, headers, _ = client.request("SETUP", client.uri + "/trackID=0",
                                         {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
        self.assertEqual(code, 200)
        expected_ssrc = int(re.search(r"ssrc=([0-9A-Fa-f]+)", headers["transport"])[1], 16)
        if self.settings.audio:
            self.assertEqual(client.request("SETUP", client.uri + "/trackID=1",
                {"Transport": "RTP/AVP/TCP;unicast;interleaved=2-3"})[0], 200)
        self.assertEqual(client.request("PLAY")[0], 200)
        deadline = time.monotonic() + 8
        last_sequence = None
        video = audio = report = marker = 0
        fragments = {}
        while time.monotonic() < deadline:
            channel, packet = client.next_packet(deadline)
            self.assertGreaterEqual(len(packet), 4)
            self.assertEqual(packet[0] >> 6, 2)
            if channel in (1, 3):
                if packet[1] == 200:
                    report += 1
                continue
            self.assertGreaterEqual(len(packet), 13)
            if channel == 2:
                self.assertEqual(packet[1] & 127, 8)
                audio += 1
                continue
            self.assertEqual(channel, 0)
            self.assertEqual(packet[1] & 127, 96)
            seq, timestamp, ssrc = struct.unpack("!HII", packet[2:12])
            self.assertEqual(ssrc, expected_ssrc)
            if last_sequence is not None:
                self.assertEqual(seq, (last_sequence + 1) & 65535)
            last_sequence = seq
            video += 1
            marker += bool(packet[1] & 128)
            nal_type = packet[12] & 31
            if nal_type == 28:
                self.assertGreaterEqual(len(packet), 15)
                flags = packet[13]
                self.assertFalse((flags & 0xC0) == 0xC0)
                if flags & 0x80:
                    self.assertNotIn(timestamp, fragments)
                    fragments[timestamp] = flags & 31
                self.assertIn(timestamp, fragments)
                if flags & 0x40:
                    del fragments[timestamp]
            else:
                self.assertTrue(1 <= nal_type <= 23)
            if video > 12 and marker >= 2 and report and (audio or not self.settings.audio):
                break
        self.assertGreater(video, 0)
        self.assertGreater(marker, 0)
        self.assertGreater(report, 0)
        if self.settings.audio:
            self.assertGreater(audio, 0)
        self.assertEqual(client.request("PAUSE")[0], 200)
        self.assertEqual(client.request("GET_PARAMETER")[0], 200)
        self.assertEqual(client.request("PLAY")[0], 200)
        self.assertEqual(client.request("TEARDOWN")[0], 200)

    def test_udp_unicast(self):
        sockets = None
        for _ in range(100):
            rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            rtp.bind((self.settings.host, 0))
            port = rtp.getsockname()[1]
            if port & 1 or port == 65535:
                rtp.close(); rtcp.close(); continue
            try:
                rtcp.bind((self.settings.host, port + 1))
                sockets = rtp, rtcp
                break
            except OSError:
                rtp.close(); rtcp.close()
        self.assertIsNotNone(sockets, "cannot allocate adjacent UDP ports")
        rtp, rtcp = sockets
        self.addCleanup(rtp.close); self.addCleanup(rtcp.close)
        rtp.settimeout(6); rtcp.settimeout(6)
        client = self.client
        self.assertEqual(client.request("DESCRIBE")[0], 200)
        code, headers, _ = client.request("SETUP", client.uri + "/trackID=0",
            {"Transport": f"RTP/AVP;unicast;client_port={port}-{port + 1}"})
        self.assertEqual(code, 200)
        self.assertIn("server_port=", headers["transport"])
        self.assertEqual(client.request("PLAY")[0], 200)
        packet, _ = rtp.recvfrom(2048)
        self.assertEqual(packet[0] >> 6, 2)
        self.assertEqual(packet[1] & 127, 96)
        report, _ = rtcp.recvfrom(2048)
        self.assertEqual(report[1], 200)
        self.assertEqual(client.request("TEARDOWN")[0], 200)
        deadline = time.monotonic() + 5
        found_bye = False
        while not found_bye and time.monotonic() < deadline:
            compound, _ = rtcp.recvfrom(2048)
            offset = 0
            while offset + 4 <= len(compound):
                size = (int.from_bytes(compound[offset + 2:offset + 4], "big") + 1) * 4
                self.assertGreaterEqual(size, 4)
                found_bye |= compound[offset + 1] == 203
                offset += size
        self.assertTrue(found_bye)

    def test_reject_bad_framing_and_transport(self):
        client = self.client
        self.assertEqual(client.request("DESCRIBE")[0], 200)
        transports = ["RTP/AVP;multicast;client_port=5000-5001",
                      "RTP/AVP;unicast;destination=1.2.3.4;client_port=5000-5001",
                      "RTP/AVP/TCP;unicast;interleaved=999999999999999999999999-1",
                      "RTP/AVP/TCP;unicast;interleaved=0-0"]
        for transport in transports:
            self.assertEqual(client.request("SETUP", client.uri + "/trackID=0",
                                             {"Transport": transport})[0], 461)
        client.sock.sendall(f"OPTIONS {client.uri} RTSP/1.0\r\nCSeq: 99\r\n"
                            "Content-Length: -1\r\n\r\n".encode())
        self.assertEqual(client.message()[0], 400)
        self.assertEqual(client.sock.recv(1), b"")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8554)
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", required=True)
    parser.add_argument("--audio", action="store_true")
    ProtocolTests.settings = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolTests))
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
