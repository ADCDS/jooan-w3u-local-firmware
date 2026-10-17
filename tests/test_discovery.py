#!/usr/bin/env python3
"""Unicast WS-Discovery checks against the caller's local replay daemon.

No multicast traffic, daemon startup, hardware controls, or configuration writes.
"""
import argparse
import socket
import time
import unittest
import uuid
import xml.etree.ElementTree as ET

SOAP = "http://www.w3.org/2003/05/soap-envelope"
ADDR = "http://schemas.xmlsoap.org/ws/2004/08/addressing"
DISC = "http://schemas.xmlsoap.org/ws/2005/04/discovery"
NEW_DISC = "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"


def packet(action="Probe", inner="", identifier=None, discovery=DISC):
    identifier = identifier or "urn:uuid:" + str(uuid.uuid4())
    body = (f'<s:Envelope xmlns:s="{SOAP}" xmlns:a="{ADDR}" xmlns:d="{discovery}" '
            'xmlns:dn="http://www.onvif.org/ver10/network/wsdl">'
            f'<s:Header><a:MessageID>{identifier}</a:MessageID>'
            f'<a:Action>{discovery}/{action}</a:Action></s:Header>'
            f'<s:Body><d:{action}>{inner}</d:{action}></s:Body></s:Envelope>')
    return identifier, body.encode()


class DiscoveryTests(unittest.TestCase):
    settings = None
    last_send = 0.0

    def setUp(self):
        if self.settings is None:
            self.skipTest("run test_discovery.py with daemon connection arguments")

    def exchange(self, data, expect=True):
        # The daemon intentionally permits at most four replies per peer/second.
        remaining = 0.3 - (time.monotonic() - type(self).last_send)
        if remaining > 0:
            time.sleep(remaining)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
            client.settimeout(1 if expect else 0.4)
            client.sendto(data, (self.settings.host, self.settings.port))
            type(self).last_send = time.monotonic()
            if not expect:
                with self.assertRaises(socket.timeout):
                    client.recvfrom(8192)
                return None
            response, peer = client.recvfrom(8192)
            self.assertEqual(peer[1], self.settings.port)
            self.assertLessEqual(len(response), 4096)
            return ET.fromstring(response)

    def test_probe_and_app_sequence(self):
        identifier, body = packet(inner="<d:Types>dn:NetworkVideoTransmitter</d:Types>")
        xml = self.exchange(body)
        self.assertEqual(xml.findtext(f".//{{{ADDR}}}RelatesTo"), identifier)
        endpoint = xml.findtext(f".//{{{ADDR}}}EndpointReference/{{{ADDR}}}Address")
        self.assertTrue(endpoint.startswith("urn:uuid:"))
        uuid.UUID(endpoint.removeprefix("urn:uuid:"))
        appseq = xml.find(f".//{{{DISC}}}AppSequence")
        self.assertIsNotNone(appseq)
        self.assertGreater(int(appseq.attrib["InstanceId"]), 0)
        self.assertGreater(int(appseq.attrib["MessageNumber"]), 0)
        address = xml.findtext(f".//{{{DISC}}}XAddrs")
        self.assertNotIn("0.0.0.0", address)
        self.assertEqual(address,
                         f"{'https' if self.settings.tls else 'http'}://"
                         f"{self.settings.host}:{self.settings.http_port}/onvif/device_service")

    def test_resolve_and_2009_probe(self):
        _, body = packet()
        xml = self.exchange(body)
        endpoint = xml.findtext(f".//{{{ADDR}}}EndpointReference/{{{ADDR}}}Address")
        identifier, body = packet("Resolve", f"<a:EndpointReference><a:Address>{endpoint}</a:Address></a:EndpointReference>")
        resolved = self.exchange(body)
        self.assertEqual(resolved.findtext(f".//{{{ADDR}}}RelatesTo"), identifier)
        self.assertIsNotNone(resolved.find(f".//{{{DISC}}}ResolveMatch"))
        _, body = packet(discovery=NEW_DISC)
        self.assertIsNotNone(self.exchange(body).find(f".//{{{NEW_DISC}}}ProbeMatch"))

    def test_nonmatching_filters_are_silent(self):
        for inner in ("<d:Types>dn:Printer</d:Types>",
                      "<d:Scopes>onvif://www.onvif.org/name/another-camera</d:Scopes>",
                      "<d:Types>dn:Device</d:Types><d:Types>dn:Printer</d:Types>"):
            _, body = packet(inner=inner)
            self.exchange(body, expect=False)
        _, body = packet("Resolve", "<a:EndpointReference><a:Address>urn:uuid:00000000-0000-0000-0000-000000000000</a:Address></a:EndpointReference>")
        self.exchange(body, expect=False)

    def test_entities_and_oversized_packets_are_silent(self):
        _, body = packet(identifier="urn:uuid:untrusted&amp;entity")
        self.exchange(body, expect=False)
        self.exchange(b"x" * 4097, expect=False)
        _, body = packet()
        self.exchange(b'<!DOCTYPE x [<!ENTITY x SYSTEM "file:///etc/passwd">]>' + body,
                      expect=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3702)
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--tls", action="store_true")
    settings, remaining = parser.parse_known_args()
    DiscoveryTests.settings = settings
    unittest.main(argv=[__file__, *remaining])
