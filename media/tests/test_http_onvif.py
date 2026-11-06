#!/usr/bin/env python3
"""Bounded HTTP/ONVIF protocol checks against an already running local daemon.

The caller owns the sole replay stack. These tests do not change configuration,
move the motor, start recording, or access the camera directly.
"""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import datetime
import hashlib
import http.client
import json
import secrets
import socket
import ssl
import unittest
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

SOAP = "http://www.w3.org/2003/05/soap-envelope"
MEDIA = "http://www.onvif.org/ver10/media/wsdl"
DEVICE = "http://www.onvif.org/ver10/device/wsdl"
SCHEMA = "http://www.onvif.org/ver10/schema"
WSSE = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd"
WSU = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"
DIGEST = "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest"


def envelope(action, inner="", header="", namespace=MEDIA):
    return (f'<s:Envelope xmlns:s="{SOAP}" xmlns:t="{namespace}" '
            f'xmlns:wsse="{WSSE}" xmlns:wsu="{WSU}">'
            f'<s:Header>{header}</s:Header><s:Body><t:{action}>'
            f'{inner}</t:{action}></s:Body></s:Envelope>').encode()


class HTTPTests(unittest.TestCase):
    settings = None

    def setUp(self):
        if self.settings is None:
            self.skipTest("run test_http_onvif.py with daemon connection arguments")

    def connection(self):
        cfg = self.settings
        if cfg.tls:
            context = ssl.create_default_context(cafile=cfg.ca)
            if cfg.insecure:
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
            return http.client.HTTPSConnection(cfg.host, cfg.port, context=context, timeout=5)
        return http.client.HTTPConnection(cfg.host, cfg.port, timeout=5)

    def request(self, method, path, body=None, auth=True, headers=None):
        fields = dict(headers or {})
        if auth:
            encoded = base64.b64encode(
                f"{self.settings.username}:{self.settings.password}".encode()).decode()
            fields["Authorization"] = f"Basic {encoded}"
        client = self.connection()
        try:
            client.request(method, path, body=body, headers=fields)
            response = client.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            client.close()

    def soap(self, action, inner="", auth=True, header="", namespace=MEDIA):
        return self.request("POST", "/onvif/device_service",
                            envelope(action, inner, header, namespace), auth,
                            {"Content-Type": "application/soap+xml"})

    def raw(self, request):
        sock = socket.create_connection((self.settings.host, self.settings.port), 5)
        if self.settings.tls:
            context = ssl.create_default_context(cafile=self.settings.ca)
            if self.settings.insecure:
                context.check_hostname = False
                context.verify_mode = ssl.CERT_NONE
            sock = context.wrap_socket(sock, server_hostname=self.settings.host)
        with sock:
            sock.settimeout(5)
            sock.sendall(request)
            response = http.client.HTTPResponse(sock)
            response.begin()
            return response.status, response.read()

    def wsse(self, offset=0, password=None, timezone_minutes=None, created=None):
        nonce = secrets.token_bytes(16)
        moment = (datetime.datetime.now(datetime.timezone.utc) +
                  datetime.timedelta(seconds=self.settings.clock_offset + offset))
        if created is None:
            created = (moment.strftime("%Y-%m-%dT%H:%M:%SZ") if timezone_minutes is None else
                       moment.astimezone(datetime.timezone(datetime.timedelta(minutes=timezone_minutes))).isoformat(timespec="seconds"))
        password = self.settings.password if password is None else password
        digest = base64.b64encode(hashlib.sha1(
            nonce + created.encode() + password.encode()).digest()).decode()
        return (f'<wsse:Security><wsse:UsernameToken>'
                f'<wsse:Username>{escape(self.settings.username)}</wsse:Username>'
                f'<wsse:Password Type="{DIGEST}">{digest}</wsse:Password>'
                f'<wsse:Nonce>{base64.b64encode(nonce).decode()}</wsse:Nonce>'
                f'<wsu:Created>{created}</wsu:Created>'
                f'</wsse:UsernameToken></wsse:Security>')

    def test_authentication_and_readiness(self):
        status, _, body = self.request("GET", "/api/v1/status", auth=False)
        self.assertEqual(status, 401)
        self.assertIn("error", json.loads(body))
        status, _, body = self.request("GET", "/api/v1/status")
        self.assertEqual(status, 200, body)
        self.assertIsInstance(json.loads(body), dict)
        status, _, body = self.request("GET", "/healthz", auth=False)
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body), {"ok": True})

    def test_persistent_connection_authentication(self):
        client = self.connection()
        auth = "Basic " + base64.b64encode(f"{self.settings.username}:{self.settings.password}".encode()).decode()
        try:
            client.request("GET", "/api/v1/status", headers={"Authorization": auth})
            reply = client.getresponse(); self.assertEqual(reply.status, 200); reply.read()
            original = client.sock; self.assertIsNotNone(original)
            client.request("GET", "/api/v1/status")
            reply = client.getresponse(); self.assertEqual(reply.status, 401); reply.read()
            self.assertIs(client.sock, original)
            client.request("GET", "/api/v1/status", headers={"Authorization": auth})
            reply = client.getresponse(); self.assertEqual(reply.status, 200); reply.read()
            self.assertIs(client.sock, original)
        finally:
            client.close()

    def test_idle_connections_release_workers(self):
        clients = []
        try:
            for _ in range(6):
                client = self.connection(); clients.append(client)
                client.request("GET", "/health")
                reply = client.getresponse(); self.assertEqual(reply.status, 200); reply.read()
            self.assertEqual(self.request("GET", "/health", auth=False)[0], 200)
            for client in clients:
                client.request("GET", "/health")
                reply = client.getresponse(); self.assertEqual(reply.status, 200); reply.read()
        finally:
            for client in clients:
                client.close()

    def test_time_only_is_public(self):
        self.assertEqual(self.soap("GetSystemDateAndTime", auth=False,
                                   namespace=DEVICE)[0], 200)
        self.assertEqual(self.soap("GetProfiles", auth=False)[0], 401)
        self.assertEqual(self.soap("SetSystemDateAndTime", auth=False,
                                   namespace=DEVICE)[0], 401)

    def test_parallel_authenticated_status_reads(self):
        with ThreadPoolExecutor(max_workers=2) as pool:
            replies = list(pool.map(lambda _: self.request("GET", "/api/v1/status"), range(4)))
        for status, _, body in replies:
            self.assertEqual(status, 200, body)
            self.assertIsInstance(json.loads(body), dict)

    def test_profiles_and_stream_uri(self):
        status, _, body = self.soap("GetProfiles")
        self.assertEqual(status, 200, body)
        profiles = ET.fromstring(body).findall(f".//{{{MEDIA}}}Profiles")
        self.assertGreaterEqual(len(profiles), 2)
        self.assertEqual(profiles[0].attrib["token"], "main0")
        status, _, body = self.soap("GetStreamUri", "<t:ProfileToken>main0</t:ProfileToken>")
        self.assertEqual(status, 200, body)
        uri = ET.fromstring(body).findtext(f".//{{{SCHEMA}}}Uri")
        self.assertIn("/live/ch0", uri)
        self.assertEqual(self.soap("GetStreamUri", "<t:ProfileToken>missing</t:ProfileToken>")[0], 500)

    def test_digest_and_nonce_replay(self):
        token = self.wsse()
        self.assertEqual(self.soap("GetProfiles", auth=False, header=token)[0], 200)
        self.assertEqual(self.soap("GetProfiles", auth=False, header=token)[0], 401)
        self.assertEqual(self.soap("GetProfiles", auth=False, header=self.wsse(password="incorrect"))[0], 401)  # hygiene: allow-test-vector
        self.assertEqual(self.soap("GetProfiles", auth=False, header=self.wsse(offset=-601))[0], 401)
        self.assertEqual(self.soap("GetProfiles", auth=False, header=self.wsse(offset=601))[0], 401)

    def test_tokens_outlast_the_replay_table(self):
        """Frigate polls GetStatus in a tight loop while the motor moves. More tokens
        than the table holds must keep working, and replays must still be refused,
        both of a token it remembers and of one it has forgotten."""
        now = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=self.settings.clock_offset)
        def created(i):
            return (now + datetime.timedelta(seconds=-200 + i * 0.4)).strftime("%Y-%m-%dT%H:%M:%SZ")
        first = last = None
        for i in range(1100):
            last = self.wsse(created=created(i))
            first = first or last
            self.assertEqual(self.soap("GetProfiles", auth=False, header=last)[0], 200, i)
        self.assertEqual(self.soap("GetProfiles", auth=False, header=first)[0], 401, "a forgotten token, by its age")
        self.assertEqual(self.soap("GetProfiles", auth=False, header=last)[0], 401, "a remembered token")
        self.assertEqual(self.soap("GetProfiles", auth=False, header=self.wsse(offset=245))[0], 200)

    def test_digest_timezone_offsets(self):
        for minutes in (0, 330, -180, 840, -840):
            with self.subTest(minutes=minutes):
                self.assertEqual(self.soap("GetProfiles", auth=False,
                                           header=self.wsse(timezone_minutes=minutes))[0], 200)
        utc = (datetime.datetime.now(datetime.timezone.utc) +
               datetime.timedelta(seconds=self.settings.clock_offset)).strftime("%Y-%m-%dT%H:%M:%S")
        for suffix in ("+14:01", "+00:60", "+2:00", "+15:00", "-24:00"):
            with self.subTest(suffix=suffix):
                self.assertEqual(self.soap("GetProfiles", auth=False,
                                           header=self.wsse(created=utc + suffix))[0], 401)

    def test_unknown_action_and_external_entities(self):
        status, _, body = self.soap("InventedWriteAction")
        self.assertEqual(status, 500)
        self.assertIsNotNone(ET.fromstring(body).find(f".//{{{SOAP}}}Fault"))
        body = (b'<!DOCTYPE x [<!ENTITY x SYSTEM "file:///etc/passwd">]>' +
                envelope("GetProfiles"))
        status, _, result = self.request("POST", "/onvif/device_service", body,
                                         headers={"Content-Type": "application/soap+xml"})
        self.assertEqual(status, 500)
        self.assertNotIn(b"root:", result)
        ET.fromstring(result)

    def test_request_framing_is_unambiguous(self):
        prefixes = [
            b"Content-Length: 0\r\nContent-Length: 0\r\n",
            b"Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
            b"Content-Length: -1\r\n",
            b"Content-Length: 1, 1\r\n",
        ]
        for fields in prefixes:
            with self.subTest(fields=fields):
                status, body = self.raw(b"POST /api/v1/config HTTP/1.1\r\nHost: local\r\n" + fields + b"\r\n")
                self.assertEqual(status, 400, body)
        self.assertEqual(self.raw(b"GET /health HTTP/1.1\r\nHost:\r\nHost: local\r\n\r\n")[0], 400)

    def test_invalid_write_and_download_paths(self):
        status, _, body = self.request("POST", "/api/v1/recording", '{"enabled":"yes"}',
                                       headers={"Content-Type": "application/json"})
        self.assertEqual(status, 400, body)
        for path in ["/api/v1/recordings/../../etc/passwd", "/api/v1/recordings/%2e%2e%2fetc%2fpasswd"]:
            status, _, body = self.request("GET", path)
            self.assertEqual(status, 400, body)
        self.assertEqual(self.request("GET", "/api/v1/snapshot?channel=")[0], 400)
        self.assertEqual(self.request("GET", "/api/v1/snapshot?channel=4")[0], 400)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", required=True)
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--ca")
    parser.add_argument("--insecure", action="store_true")
    parser.add_argument("--clock-offset", type=float, default=0,
                        help="Camera UTC clock minus client UTC clock, in seconds")
    settings, remaining = parser.parse_known_args()
    HTTPTests.settings = settings
    unittest.main(argv=[__file__, *remaining])
