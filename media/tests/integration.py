#!/usr/bin/env python3
"""One owned replay stack: protocol, real decode, persistence and recording checks.

No camera access. --qemu executes the rebuilt MIPS application in userspace
against workstation-generated files, never an OEM application or kernel.
"""
import argparse
import base64
import http.client
import json
import os
from pathlib import Path
import re
import sys
from types import SimpleNamespace
import secrets
import signal
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import time


def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                            timeout=kwargs.pop("timeout", 90), **kwargs)
    if result.returncode:
        raise AssertionError(f"Command failed ({result.returncode}): {command[0]}\n{result.stdout}\n{result.stderr}")
    return result


def sntp_server(stop):
    """Loopback SNTP server answering with this host's own clock.

    Every request first receives a forged reply with the wrong origin cookie,
    which the daemon must ignore. The genuine reply's offset is below the
    daemon's step threshold, so the test never moves the host clock.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0.2)
    counts = {"requests": 0, "forged": 0}

    def stamp(now):
        return struct.pack("!II", (int(now) + 2208988800) & 0xffffffff, int((now % 1) * 2**32))

    def serve():
        while not stop.is_set():
            try:
                request, peer = sock.recvfrom(512)
            except socket.timeout:
                continue
            if len(request) < 48 or request[0] & 7 != 3:
                continue
            counts["requests"] += 1
            received = stamp(time.time())
            sock.sendto(bytes([0x24, 2]) + bytes(30) + received + stamp(time.time()), peer)
            counts["forged"] += 1
            sock.sendto(bytes([0x24, 2]) + bytes(22) + request[40:48] + received + stamp(time.time()), peer)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return sock, thread, counts


def talk_checks(rtsp_port, secret, request, capture, context_status):
    """A go2rtc-like talker: RFC 2069 digest with the nonce reused, Require on
    DESCRIBE, three interleaved tracks, then 1024-byte PT 8 packets with RTP
    timestamp 0 every 128 ms (go2rtc 1.9.14's repacketizer)."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_rtsp import Client
    settings = SimpleNamespace(host="127.0.0.1", port=rtsp_port, username="admin", password=secret)
    talker = Client(settings)
    talker.uri = f"rtsp://127.0.0.1:{rtsp_port}/live/ch0"
    code, _, body = talker.request("DESCRIBE", headers={"Require": "www.onvif.org/ver20/backchannel"}, legacy=True)
    assert code == 200 and b"a=sendonly" in body, body
    for track, pair in ((0, "0-1"), (1, "2-3"), (2, "4-5")):
        code = talker.request("SETUP", talker.uri + f"/trackID={track}",
                              {"Transport": f"RTP/AVP/TCP;unicast;interleaved={pair}"}, legacy=True)[0]
        assert code == 200, (track, code)
    assert talker.request("PLAY", legacy=True)[0] == 200

    def drain(seconds):
        """Read what the camera sends; return the microphone payloads."""
        audio, deadline = [], time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                channel, packet = talker.next_packet(deadline)
            except (socket.timeout, TimeoutError):
                break
            if channel == 2 and len(packet) > 12:
                audio.append(packet[12:])
        return audio

    before = b"".join(drain(0.6))
    assert before and set(before) != {0xd5}, "the replay tone should be audible before talk"
    payloads = [bytes(((i * 13 + n * 29) & 0x7f) | 0x80 for i in range(1024)) for n in range(12)]
    ssrc, talking_status, mic_during = 0x1234ABCD, None, b""
    for n, payload in enumerate(payloads):
        rtp = struct.pack("!BBHII", 0x80, 0x80 | 8, n, 0, ssrc) + payload
        talker.sock.sendall(b"$" + bytes([4]) + len(rtp).to_bytes(2, "big") + rtp)
        mic = b"".join(drain(0.128))
        if n >= 6:
            mic_during += mic
        if n == 6:
            talking_status = json.loads(request("GET", "/api/v1/status")[1])
            # A second talker, over RTSP or HTTP, is refused while this one talks.
            code, body = request_talk(context_status, b"\xd5" * 800)
            assert code == 409, (code, body)
            other = Client(settings)
            other.uri = talker.uri
            assert other.request("DESCRIBE", headers={"Require": "www.onvif.org/ver20/backchannel"})[0] == 200
            assert other.request("SETUP", other.uri + "/trackID=2",
                                 {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})[0] == 200
            assert other.request("PLAY")[0] == 200
            rtp2 = struct.pack("!BBHII", 0x80, 8, 1, 0, 0x55AA55AA) + b"\x9a" * 160
            other.sock.sendall(b"$" + bytes([0]) + len(rtp2).to_bytes(2, "big") + rtp2)
            time.sleep(0.05)
            other.close()
    assert talking_status["platform"]["speaker_enable"] == 1, talking_status["platform"]
    speaker = talking_status["audio"]["speaker"]
    assert speaker["state"] == "talking" and speaker["half_duplex"], speaker
    assert mic_during and set(mic_during) == {0xd5}, "half-duplex: the microphone is muted while talking"
    # Stop talking: the deadman ends the session and mutes within a second.
    deadline = time.monotonic() + 2.5
    while time.monotonic() < deadline:
        drain(0.1)
        status = json.loads(request("GET", "/api/v1/status")[1])
        if status["platform"]["speaker_enable"] == 0 and status["audio"]["speaker"]["state"] == "idle":
            break
    else:
        raise AssertionError(f"speaker not muted after talk: {status}")
    assert status["audio"]["speaker"]["refused"] >= 2, status["audio"]["speaker"]
    played = capture.read_bytes()
    position = 0
    for n, payload in enumerate(payloads):
        found = played.find(payload, position)
        assert found >= 0, f"talk packet {n} missing or out of order in what was played"
        position = found + len(payload)
    assert talker.request("TEARDOWN", legacy=True)[0] == 200
    talker.close()
    # HTTP talk on an idle speaker is accepted and plays.
    code, body = request_talk(context_status, bytes(range(128, 256)) * 10)
    assert code == 200, (code, body)
    return status["audio"]


def go2rtc_interop(binary, root, rtsp_port, secret, request):
    """Optional (GO2RTC_BIN): the Frigate layout against the real go2rtc. The
    talk stream must offer `audio, sendonly` and play a pushed file."""
    tone = root / "tone.wav"
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=8000:duration=1", "-ac", "1", tone])
    api, restream = port(), port()
    camera = f"rtsp://admin:{secret}@127.0.0.1:{rtsp_port}/live/ch0"  # hygiene: allow-test-vector
    (root / "go2rtc.yaml").write_text(
        f'api:\n  listen: "127.0.0.1:{api}"\nrtsp:\n  listen: "127.0.0.1:{restream}"\n'
        'webrtc:\n  listen: ""\nstreams:\n'
        f'  cam:\n    - {camera}#backchannel=0\n'
        f'  cam_talk:\n    - rtsp://127.0.0.1:{restream}/cam#backchannel=0\n'
        f'    - {camera}#backchannel=1#media=audio\n')
    log = (root / "go2rtc.log").open("w")
    go2rtc = subprocess.Popen([binary, "-config", str(root / "go2rtc.yaml")], stdout=log,
                              stderr=subprocess.STDOUT, cwd=root)
    try:
        time.sleep(2)
        connection = http.client.HTTPConnection("127.0.0.1", api, timeout=10)
        connection.request("GET", "/api/streams?src=cam_talk&video=all&audio=all&microphone")
        streams = json.loads(connection.getresponse().read())
        medias = [m for producer in streams.get("producers", []) for m in producer.get("medias") or []]
        assert "audio, sendonly, PCMA/8000" in medias, streams
        before = json.loads(request("GET", "/api/v1/status")[1])["audio"]["speaker"]["played_ms"]
        connection.request("POST", f"/api/streams?dst=cam_talk&src=ffmpeg:{tone}%23audio=pcma%23input=file")
        assert connection.getresponse().status == 200
        connection.close()
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            speaker = json.loads(request("GET", "/api/v1/status")[1])["audio"]["speaker"]
            if speaker["played_ms"] - before >= 800 and speaker["state"] == "idle":
                break
            time.sleep(0.2)
        else:
            raise AssertionError(f"go2rtc talk did not play: {speaker}")
    finally:
        go2rtc.terminate()
        go2rtc.wait(timeout=10)
        log.close()


def request_talk(connection, data):
    client = connection()
    try:
        client.request("POST", "/api/v1/talk", body=data, headers={
            "Authorization": "Basic " + base64.b64encode(f"admin:{connection.secret}".encode()).decode(),
            "Content-Type": "audio/PCMA"})
        reply = client.getresponse()
        return reply.status, reply.read()
    finally:
        client.close()


def onvif_profile_order(connection, secret):
    """tt:Profile children must follow the schema sequence (zeep binds by
    position and would drop PTZConfiguration otherwise)."""
    client = connection()
    try:
        body = ('<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
                '<GetProfiles xmlns="http://www.onvif.org/ver10/media/wsdl"/></s:Body></s:Envelope>')
        client.request("POST", "/onvif/media_service", body=body, headers={
            "Authorization": "Basic " + base64.b64encode(f"admin:{secret}".encode()).decode(),
            "Content-Type": "application/soap+xml"})
        reply = client.getresponse()
        assert reply.status == 200
        tree = ET.fromstring(reply.read())
    finally:
        client.close()
    order = ["VideoSourceConfiguration", "AudioSourceConfiguration", "VideoEncoderConfiguration",
             "AudioEncoderConfiguration", "PTZConfiguration"]
    profiles = tree.findall(".//{http://www.onvif.org/ver10/media/wsdl}Profiles")
    assert profiles
    for profile in profiles:
        names = [child.tag.rsplit("}", 1)[-1] for child in profile if child.tag.rsplit("}", 1)[-1] in order]
        assert names == sorted(names, key=order.index), names
        assert "AudioSourceConfiguration" in names and "AudioEncoderConfiguration" in names, names


def fake_platform():
    """A stand-in for joan-daemon on loopback: one admin session (token tok1,
    csrf c1), a session check, sign-out, and a CSRF-protected PUT that echoes.
    Records what the relay sends so the checks can see what crossed it."""
    seen, state = [], {"live": False}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, status, body=b"", extra=()):
            self.send_response(status)
            if body:
                self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for name, value in extra:
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(body)

        def handle_any(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""
            seen.append({"method": self.command, "path": self.path, "host": self.headers.get("Host"),
                         "origin": self.headers.get("Origin"), "cookie": self.headers.get("Cookie"),
                         "csrf": self.headers.get("X-CSRF-Token"), "type": self.headers.get("Content-Type"),
                         "body": body})
            live = state["live"] and "joan_session=tok1" in (self.headers.get("Cookie") or "")
            if self.path == "/api/v1/session" and self.command == "POST":
                if json.loads(body or b"{}") == {"username": "admin", "password": "right"}:
                    state["live"] = True
                    return self.reply(200, b'{"ok":true,"csrf":"c1","default_password_warning":false}',
                                      [("Set-Cookie", "joan_session=tok1; Path=/; HttpOnly; SameSite=Strict")])
                return self.reply(401, b'{"error":{"code":"invalid_credentials"}}')
            if not live:
                return self.reply(401, b'{"error":{"code":"authentication_required"}}')
            if self.path == "/api/v1/session" and self.command == "GET":
                return self.reply(200, b'{"authenticated":true,"csrf":"c1"}')
            if self.headers.get("X-CSRF-Token") != "c1":
                return self.reply(403, b'{"error":{"code":"csrf_rejected"}}')
            if self.path == "/api/v1/session" and self.command == "DELETE":
                state["live"] = False
                return self.reply(204, extra=[("Set-Cookie", "joan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict")])
            if self.path == "/api/v1/network/mdns" and self.command == "PUT":
                return self.reply(200, body)
            if self.path == "/api/v1/update" and self.command == "POST":
                import hashlib
                return self.reply(200, json.dumps({"id": "pkg", "bytes": len(body),
                                                   "sha256": hashlib.sha256(body).hexdigest()}).encode())
            return self.reply(404, b'{"error":{"code":"route_not_found"}}')

        do_GET = do_POST = do_PUT = do_DELETE = handle_any

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, seen


ISP_PROC = ("****************** ISP INFO 0**********************\nISP Runing Mode : Day\n"
            "SENSOR Integration Time : 2990 lines\nISP EV value: {ev0}\nISP EV value log2: 1\n"
            "****************** ISP INFO 1**********************\nISP EV value: {ev1}\n")


def controls_checks(request, root, connection=None, secret=None):
    """Persisted controls and their engine: automatic day/night from the ISP's
    exposure value with hysteresis and a wait, the night schedule, the timed
    white-light trigger, validation, and controls.json."""
    proc = root / "isp-m0"

    def set_ev(ev):
        proc.write_text(ISP_PROC.format(ev0=ev, ev1=ev // 2))

    def controls():
        status, body = request("GET", "/api/v1/controls")
        assert status == 200, body
        return json.loads(body)

    def platform():
        return json.loads(request("GET", "/api/v1/status")[1])["platform"]

    def wait_for(check, seconds, what):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if check():
                return
            time.sleep(0.25)
        raise AssertionError(f"{what}: {controls()}")

    set_ev(10000)
    status, body = request("POST", "/api/v1/controls",
                           {"night": {"mode": "auto", "night_ev": 30000, "day_ev": 15000, "wait_s": 1}})
    assert status == 200 and json.loads(body)["night"]["mode"] == "auto", body
    wait_for(lambda: controls()["live"]["level"] == 10000, 8, "exposure read")
    assert controls()["live"]["night"] == "day" and platform()["night"] == 0
    set_ev(40000)  # darker than the night level: night after the wait
    wait_for(lambda: controls()["live"]["night"] == "night" and platform()["night"] == 1, 12, "auto to night")
    set_ev(20000)  # between the levels: stays night (hysteresis)
    time.sleep(6.5)
    assert controls()["live"]["night"] == "night"
    set_ev(9000)
    wait_for(lambda: controls()["live"]["night"] == "day" and platform()["night"] == 0, 12, "auto to day")
    # Validation leaves everything as it was.
    for bad in ({"night": {"night_ev": 100, "day_ev": 200}}, {"night": {"from": "25:00"}},
                {"night": {"mode": "dusk"}}, {"picture": [{"denoise": 300}]}, {"anti_flicker": "40"}, [1]):
        assert request("POST", "/api/v1/controls", bad)[0] == 400, bad
    assert controls()["night"]["mode"] == "auto"
    # Schedule, in UTC minutes (utc_offset_min 0): the current minute is inside.
    now = time.gmtime()
    start = (now.tm_hour * 60 + now.tm_min - 1) % 1440
    end = (start + 4) % 1440
    status, _ = request("POST", "/api/v1/controls", {"utc_offset_min": 0, "night": {
        "mode": "schedule", "from": f"{start // 60:02d}:{start % 60:02d}", "until": f"{end // 60:02d}:{end % 60:02d}"}})
    assert status == 200
    wait_for(lambda: controls()["live"]["night"] == "night", 5, "schedule night")
    # The legacy control still works and becomes the choice.
    assert request("POST", "/api/v1/control", {"operation": "night", "value": 0, "value2": 0})[0] == 200
    assert controls()["night"]["mode"] == "day" and platform()["night"] == 0
    assert request("POST", "/api/v1/control", {"operation": "night", "value": 2, "value2": 0})[0] == 200
    assert controls()["night"]["mode"] == "auto"
    assert request("POST", "/api/v1/control", {"operation": "night", "value": 4, "value2": 0})[0] == 409
    assert request("POST", "/api/v1/control", {"operation": "ir_mode", "value": 1, "value2": 0})[0] == 200
    assert controls()["night"]["ir"] == "off"
    # White light: on for 2 s from a trigger, then back off by itself.
    assert request("POST", "/api/v1/control", {"operation": "white_light_trigger", "value": 2, "value2": 0})[0] == 200
    wait_for(lambda: platform()["white_light"] == 1, 4, "trigger on")
    wait_for(lambda: platform()["white_light"] == 0 and controls()["live"]["trigger_left_s"] == 0, 6, "trigger off")
    # Saved (after a short debounce) and readable.
    saved = root / "state" / "controls.json"
    wait_for(lambda: saved.exists() and json.loads(saved.read_text())["night"]["ir"] == "off", 8, "controls.json")
    stored = json.loads(saved.read_text())
    assert stored["v"] == 1 and stored["night"]["mode"] == "auto" and oct(saved.stat().st_mode & 0o777) == "0o600", stored
    request("POST", "/api/v1/controls", {"night": {"mode": "day", "ir": "night"}})
    imaging = imaging_checks(connection, secret, controls) if connection else None
    return {"saved": stored["night"], "imaging": imaging}


def imaging_checks(connection, secret, controls):
    """ONVIF imaging: advertised in schema order, reads the persisted values on
    the 0..100 scale, and writes through the controls."""
    ns = {"s": "http://www.w3.org/2003/05/soap-envelope", "tt": "http://www.onvif.org/ver10/schema",
          "timg": "http://www.onvif.org/ver20/imaging/wsdl", "tds": "http://www.onvif.org/ver10/device/wsdl"}

    def soap(path, body):
        client = connection()
        try:
            client.request("POST", path, body=('<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
                                               f'<s:Body>{body}</s:Body></s:Envelope>'), headers={
                "Authorization": "Basic " + base64.b64encode(f"admin:{secret}".encode()).decode(),
                "Content-Type": "application/soap+xml"})
            reply = client.getresponse()
            return reply.status, ET.fromstring(reply.read())
        finally:
            client.close()

    status, tree = soap("/onvif/device_service", '<GetCapabilities xmlns="http://www.onvif.org/ver10/device/wsdl"/>')
    caps = tree.find(".//tds:Capabilities", ns)
    order = [child.tag.rsplit("}", 1)[-1] for child in caps]
    assert status == 200 and order == ["Device", "Imaging", "Media", "PTZ"], order
    xaddr = caps.find("tt:Imaging/tt:XAddr", ns).text
    assert xaddr.endswith("/onvif/imaging_service"), xaddr
    img = '<{0} xmlns="http://www.onvif.org/ver20/imaging/wsdl"><VideoSourceToken>sensor0</VideoSourceToken>{1}</{0}>'
    status, tree = soap("/onvif/imaging_service", img.format("GetImagingSettings", ""))
    settings = tree.find(".//timg:ImagingSettings", ns)
    assert status == 200 and settings.find("tt:Brightness", ns).text == "50" and \
        settings.find("tt:IrCutFilter", ns).text == "ON", ET.tostring(tree)
    status, tree = soap("/onvif/imaging_service", img.format("SetImagingSettings",
        '<ImagingSettings><Brightness xmlns="http://www.onvif.org/ver10/schema">80</Brightness>'
        '<IrCutFilter xmlns="http://www.onvif.org/ver10/schema">AUTO</IrCutFilter></ImagingSettings>'))
    assert status == 200, ET.tostring(tree)
    state = controls()
    assert state["picture"][0]["brightness"] == 204 and state["night"]["mode"] == "auto", state
    status, tree = soap("/onvif/imaging_service", img.format("GetOptions", ""))
    modes = [m.text for m in tree.findall(".//tt:IrCutFilterModes", ns)]
    assert status == 200 and modes == ["ON", "OFF", "AUTO"], modes
    status, _ = soap("/onvif/imaging_service", img.replace("sensor0", "sensor9").format("GetImagingSettings", ""))
    assert status != 200
    status, _ = soap("/onvif/imaging_service", img.format("SetImagingSettings",
        '<ImagingSettings><Brightness xmlns="http://www.onvif.org/ver10/schema">101</Brightness></ImagingSettings>'))
    assert status != 200 and controls()["picture"][0]["brightness"] == 204
    soap("/onvif/imaging_service", img.format("SetImagingSettings",
        '<ImagingSettings><Brightness xmlns="http://www.onvif.org/ver10/schema">50</Brightness>'
        '<IrCutFilter xmlns="http://www.onvif.org/ver10/schema">ON</IrCutFilter></ImagingSettings>'))
    return {"capabilities": order}


def wave_a_checks(request):
    """Microphone mute and level meter (the replay tone), and renaming a
    preset without moving it."""
    def status():
        return json.loads(request("GET", "/api/v1/status")[1])

    # The talk checks just ran: wait out the half-duplex silence for a full
    # one-second window of the replay tone.
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        loud = status()["audio"].get("level")
        if loud and loud["peak_dbfs"] > -40:
            break
        time.sleep(0.2)
    assert loud["peak_dbfs"] > -40 and loud["rms_dbfs"] > -50, loud
    assert request("POST", "/api/v1/control", {"operation": "mic_mute", "value": 1, "value2": 0})[0] == 200
    time.sleep(2.3)
    muted = status()["audio"]
    assert muted["muted"] is True and muted["level"]["peak_dbfs"] < -70, muted
    assert request("POST", "/api/v1/control", {"operation": "mic_mute", "value": 2, "value2": 0})[0] == 409
    assert request("POST", "/api/v1/control", {"operation": "mic_mute", "value": 0, "value2": 0})[0] == 200
    time.sleep(2.3)
    back = status()["audio"]
    assert back["muted"] is False and back["level"]["peak_dbfs"] > -40, back
    assert request("POST", "/api/v1/control", {"operation": "preset_set", "value": 3, "value2": 0, "name": "Door"})[0] == 200
    assert request("POST", "/api/v1/control", {"operation": "preset_set", "value": 4, "value2": 0, "name": "Gate"})[0] == 200
    before = {p["slot"]: p for p in status()["platform"]["presets"]}
    assert request("POST", "/api/v1/control", {"operation": "preset_rename", "value": 3, "value2": 0, "name": "Front door"})[0] == 200
    after = {p["slot"]: p for p in status()["platform"]["presets"]}
    assert after[3]["name"] == "Front door" and (after[3]["x"], after[3]["y"]) == (before[3]["x"], before[3]["y"]), after
    assert request("POST", "/api/v1/control", {"operation": "preset_rename", "value": 3, "value2": 0, "name": "gate"})[0] == 409
    assert request("POST", "/api/v1/control", {"operation": "preset_rename", "value": 9, "value2": 0, "name": "Nine"})[0] == 409
    assert request("POST", "/api/v1/control", {"operation": "preset_rename", "value": 3, "value2": 0})[0] == 409
    for slot in (3, 4):
        assert request("POST", "/api/v1/control", {"operation": "preset_remove", "value": slot, "value2": 0})[0] == 200
    return {"tone": loud, "muted": muted["level"]}


def ui_checks(connection, scheme, http_port, secret, seen, ui_connection=None):
    """The admin UI: public files with a strict CSP, single sign-in through
    the platform relay, the session cookie on the media API with CSRF and
    Origin checks, and no browser password dialog for the UI's requests."""
    origin = f"{scheme}://127.0.0.1:{http_port}"

    def call(method, path, body=None, headers=None):
        client = connection()
        try:
            client.request(method, path, body=body, headers=headers or {})
            reply = client.getresponse()
            return reply.status, reply.getheaders(), reply.read()
        finally:
            client.close()

    def header(headers, name):
        return [value for key, value in headers if key.lower() == name.lower()]

    status, headers, body = call("GET", "/")
    assert status == 200 and header(headers, "Content-Type")[0].startswith("text/html"), (status, body[:200])
    csp = header(headers, "Content-Security-Policy")[0]
    assert "script-src 'self'" in csp and "frame-ancestors 'none'" in csp, csp
    ui = Path(__file__).resolve().parent.parent / "ui"
    for name in sorted(p.name for p in ui.glob("*") if p.suffix in (".css", ".js", ".svg")):
        status, _, served = call("GET", f"/ui/{name}")
        assert status == 200 and served == (ui / name).read_bytes(), name
    assert call("GET", "/ui/missing.js")[0] == 404
    json_headers = {"Content-Type": "application/json", "Origin": origin}
    # Sign-in: wrong password, then right; the platform cookie gains Secure on HTTPS.
    status, _, _ = call("POST", "/api/joan/v1/session", json.dumps({"username": "admin", "password": "wrong"}), json_headers)
    assert status == 401
    status, headers, body = call("POST", "/api/joan/v1/session", json.dumps({"username": "admin", "password": "right"}),
                                 json_headers)
    assert status == 200 and json.loads(body)["csrf"] == "c1", body
    cookie = header(headers, "Set-Cookie")
    assert cookie and cookie[0].startswith("joan_session=tok1") and (("Secure" in cookie[0]) == (scheme == "https")), cookie
    login = seen[-1]
    assert login["host"] == "127.0.0.1" and login["origin"] is None, login
    # A state change needs this page's Origin; another site's is refused.
    assert call("POST", "/api/joan/v1/session", "{}", {"Content-Type": "application/json"})[0] == 403
    assert call("POST", "/api/joan/v1/session", "{}", {"Content-Type": "application/json",
                                                       "Origin": "https://evil.example"})[0] == 403
    assert call("GET", "/api/joan/v1/session", headers={"Cookie": "joan_session=tok1",
                                                        "Sec-Fetch-Site": "cross-site"})[0] == 403
    # The media API accepts the platform session; only the session cookie crosses.
    ui_get = {"Cookie": "other=1; joan_session=tok1", "X-Requested-With": "jipc-ui"}
    status, _, body = call("GET", "/api/v1/status", headers=ui_get)
    assert status == 200 and json.loads(body)["ready"], body
    assert seen[-1]["cookie"] == "joan_session=tok1" and seen[-1]["path"] == "/api/v1/session", seen[-1]
    control = json.dumps({"operation": "idr", "value": 0, "value2": 0})
    ui_post = ui_get | {"Content-Type": "application/json", "Origin": origin}
    assert call("POST", "/api/v1/control", control, ui_post)[0] == 403              # no CSRF token
    assert call("POST", "/api/v1/control", control, ui_post | {"X-CSRF-Token": "c2"})[0] == 403
    assert call("POST", "/api/v1/control", control, {k: v for k, v in ui_post.items() if k != "Origin"} |
                {"X-CSRF-Token": "c1"})[0] == 403                                     # no Origin
    status, _, body = call("POST", "/api/v1/control", control, ui_post | {"X-CSRF-Token": "c1"})
    assert status == 200, body
    # Relay with a body: method, body, token and cookie pass; Origin does not.
    status, _, body = call("PUT", "/api/joan/v1/network/mdns", '{"name":"camera1"}', ui_post | {"X-CSRF-Token": "c1"})
    assert status == 200 and body == b'{"name":"camera1"}', (status, body)
    put = seen[-1]
    assert put["method"] == "PUT" and put["csrf"] == "c1" and put["origin"] is None and put["cookie"] == "joan_session=tok1"
    assert call("PUT", "/api/joan/v1/network/mdns", '{}', ui_post | {"X-CSRF-Token": "c1",
                "Content-Type": "application/octet-stream"})[0] == 415
    assert call("GET", "/api/joan/../v1/status", headers=ui_get)[0] in (400, 404)
    # No Basic challenge for the UI's own requests; other clients still get one.
    status, headers, _ = call("GET", "/api/v1/status", headers={"X-Requested-With": "jipc-ui"})
    assert status == 401 and not header(headers, "WWW-Authenticate")
    status, headers, _ = call("GET", "/api/v1/status")
    assert status == 401 and header(headers, "WWW-Authenticate")
    # A platform package streams through (never buffered whole in the daemon).
    import hashlib
    package = secrets.token_bytes(1536 * 1024)
    upload = ui_post | {"X-CSRF-Token": "c1", "Content-Type": "application/octet-stream"}
    status, _, body = call("POST", "/api/joan/v1/update", package, upload)
    staged = json.loads(body) if status == 200 else body
    assert status == 200 and staged["bytes"] == len(package) and \
        staged["sha256"] == hashlib.sha256(package).hexdigest(), (status, staged)
    assert call("POST", "/api/joan/v1/update", b"x" * 1024, upload | {"Content-Type": "text/plain"})[0] == 415
    client = connection()
    try:  # over the platform's own limit: refused before any byte is relayed
        client.putrequest("POST", "/api/joan/v1/update")
        for name, value in upload.items():
            client.putheader(name, value)
        client.putheader("Content-Length", str(2097345))
        client.endheaders()
        assert client.getresponse().status == 413
    finally:
        client.close()
    # The admin UI's own HTTPS port serves the same pages and API.
    if ui_connection:
        client = ui_connection()
        try:
            client.request("GET", "/")
            reply = client.getresponse()
            assert reply.status == 200 and b"<" in reply.read()
            client.request("GET", "/api/v1/status", headers=ui_get)
            reply = client.getresponse()
            assert reply.status == 200 and json.loads(reply.read())["ready"]
        finally:
            client.close()
    # The stream password: only the UI's own signed-in fetch may read it.
    status, _, body = call("GET", "/api/v1/config/password", headers=ui_get)
    assert status == 200 and json.loads(body) == {"username": "admin", "password": secret}, (status, body)
    assert call("GET", "/api/v1/config/password", headers={"Cookie": "joan_session=tok1"})[0] == 403
    basic = {"Authorization": "Basic " + base64.b64encode(f"admin:{secret}".encode()).decode(),
             "X-Requested-With": "jipc-ui"}
    assert call("GET", "/api/v1/config/password", headers=basic)[0] == 403
    assert call("GET", "/api/v1/config/password", headers=ui_get | {"Sec-Fetch-Site": "cross-site"})[0] == 403
    # Sign-out reaches the media API at once (the session cache is dropped).
    status, headers, _ = call("DELETE", "/api/joan/v1/session", None, ui_post | {"X-CSRF-Token": "c1"})
    assert status == 204 and header(headers, "Set-Cookie")[0].startswith("joan_session=;"), status
    assert call("GET", "/api/v1/status", headers=ui_get)[0] == 401
    return {"relayed_requests": len(seen)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--qemu")
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--main-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    prefix = [args.qemu, "-L", str(args.sysroot.resolve())] if args.qemu and args.sysroot else []
    if args.qemu and not args.sysroot:
        parser.error("--qemu requires --sysroot")
    here = Path(__file__).resolve().parent
    result = {"binary": str(binary), "mode": "qemu-replay" if prefix else "host-replay",
              "tls": args.tls, "main_only": args.main_only, "camera_access": False, "checks": []}
    with tempfile.TemporaryDirectory(prefix="jipc-integration-") as temp:
        root = Path(temp)
        fixture = root / "source.h264"
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             "testsrc2=size=320x240:rate=15", "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
             "-x264-params", "threads=1:keyint=15:min-keyint=15:scenecut=0:aud=1:repeat-headers=1",
             "-pix_fmt", "yuv420p", "-f", "h264", fixture])
        fixed_fixture = root / "fixed.h264"
        if args.main_only:
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                 "testsrc2=size=160x120:rate=15", "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
                 "-x264-params", "threads=1:keyint=15:min-keyint=15:scenecut=0:aud=1:repeat-headers=1",
                 "-pix_fmt", "yuv420p", "-f", "h264", fixed_fixture])
        http_port, rtsp_port, discovery_port = port(), port(), port(socket.SOCK_DGRAM)
        secret = "test-" + secrets.token_hex(12)
        ntp_stop = threading.Event()
        ntp_socket, ntp_thread, ntp_counts = sntp_server(ntp_stop)
        ntp_port = ntp_socket.getsockname()[1]
        config = root / "camera.ini"
        # Empty file-side input intentionally exercises CLI override bootstrap/reload.
        config.write_text(f"username=admin\npassword={secret}\nbackend=replay\ninput0=\n"
                          f"width=320\nheight=240\nsub_width=320\nsub_height=240\n"
                          f"state_dir={root / 'state'}\nrecord_dir={root / 'recordings'}\n"
                          f"record_seconds=1\nrecord_max_mb=2\ndiscovery_port={discovery_port}\n"
                          f"ntp_server=127.0.0.1:{ntp_port}\n")
        if args.main_only:
            text = config.read_text().replace("sub_width=320", "sub_width=160").replace("sub_height=240", "sub_height=120")
            config.write_text(text + f"main_only=1\nwidth1=160\nheight1=120\ninput2={fixed_fixture}\n")
        config.chmod(0o600)
        context = None
        if args.tls:
            key, cert = root / "key.pem", root / "cert.pem"
            run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
                 "-keyout", key, "-out", cert])
            with config.open("a") as handle:
                handle.write(f"tls_cert={cert}\ntls_key={key}\n")
            context = ssl.create_default_context(cafile=str(cert))
        command = prefix + [str(binary), "--config", str(config), "--input", str(fixture),
                            "--http-port", str(http_port), "--rtsp-port", str(rtsp_port), "--duration", "180"]
        log = (root / "daemon.log").open("w")
        capture = root / "speaker.alaw"
        # No boot gate on a workstation; capture what the speaker would play;
        # an audible replay tone so the half-duplex mute is observable.
        platform_server, platform_seen = fake_platform()
        ui_port = port()
        environment = os.environ | {"JIPC_SPEAKER_GATE": "", "JIPC_SPEAKER_CAPTURE": str(capture),
                                    "JIPC_REPLAY_TONE": "1",
                                    "JIPC_JOAN_PORT": str(platform_server.server_address[1]),
                                    "JIPC_HTTPS_UI_PORT": str(ui_port),
                                    "JIPC_ISP_PROC": str(root / "isp-m0")}
        (root / "isp-m0").write_text(ISP_PROC.format(ev0=10000, ev1=5000))
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=root, env=environment)

        def request(method, path, data=None, password=None):
            password = secret if password is None else password
            client = (http.client.HTTPSConnection("127.0.0.1", http_port, context=context, timeout=5)
                      if args.tls else http.client.HTTPConnection("127.0.0.1", http_port, timeout=5))
            headers = {"Authorization": "Basic " + base64.b64encode(f"admin:{password}".encode()).decode()}
            body = json.dumps(data) if data is not None else None
            if body is not None:
                headers["Content-Type"] = "application/json"
            try:
                client.request(method, path, body=body, headers=headers)
                reply = client.getresponse()
                return reply.status, reply.read()
            finally:
                client.close()

        def wait_ready():
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError("Daemon exited:\n" + (root / "daemon.log").read_text())
                try:
                    status, body = request("GET", "/api/v1/status")
                    if status == 200 and json.loads(body)["ready"]:
                        return json.loads(body)
                except (OSError, http.client.HTTPException, ValueError):
                    pass
                time.sleep(0.05)
            raise AssertionError("Daemon did not become ready:\n" + (root / "daemon.log").read_text())

        try:
            status = wait_ready()
            assert status["backend"] == "replay" and "platform" in status
            result["checks"].append("CLI input override and ready status")
            deadline = time.monotonic() + 10
            while True:
                clock = json.loads(request("GET", "/api/v1/status")[1])["clock"]
                if clock["synchronized"] or time.monotonic() > deadline:
                    break
                time.sleep(0.1)
            assert clock["source"] == "ntp" and clock["synchronized"], clock
            assert clock["error"] == "" and abs(clock["last_offset_ms"]) < 1000, clock
            assert ntp_counts["forged"] >= 1, ntp_counts
            client = (http.client.HTTPSConnection("127.0.0.1", http_port, context=context, timeout=5)
                      if args.tls else http.client.HTTPConnection("127.0.0.1", http_port, timeout=5))
            try:
                client.request("POST", "/onvif/device_service", headers={"Content-Type": "application/soap+xml"},
                               body='<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
                                    '<GetSystemDateAndTime xmlns="http://www.onvif.org/ver10/device/wsdl"/>'
                                    '</s:Body></s:Envelope>')
                assert b"<tt:DateTimeType>NTP</tt:DateTimeType>" in client.getresponse().read()
            finally:
                client.close()
            result["clock"] = clock
            result["checks"].append("SNTP sync ignoring a forged-origin reply; ONVIF reports NTP time")
            protocol = run(["python3", here / "test_rtsp.py", "--port", rtsp_port,
                            "--username", "admin", "--password", secret, "--audio", "--backchannel"], timeout=100)
            result["rtsp_protocol"] = protocol.stderr.strip()

            def connection():
                return (http.client.HTTPSConnection("127.0.0.1", http_port, context=context, timeout=5)
                        if args.tls else http.client.HTTPConnection("127.0.0.1", http_port, timeout=5))
            connection.secret = secret
            result["audio"] = talk_checks(rtsp_port, secret, request, capture, connection)
            result["wave_a"] = wave_a_checks(request)
            result["controls"] = controls_checks(request, root, connection, secret)
            result["checks"].append("controls: auto day/night with hysteresis, schedule, white-light trigger, "
                                    "validation and controls.json")
            result["checks"].append("microphone mute and level meter; preset rename keeps its position")
            result["checks"].append("go2rtc-style backchannel talk: one talker, half-duplex mute, "
                                    "deadman unmute/mute, ordered playback; HTTP talk")
            probe = run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-select_streams", "a:0",
                         "-show_entries", "packet=pts_time", "-of", "csv=p=0", "-read_intervals", "%+2",
                         f"rtsp://admin:{secret}@127.0.0.1:{rtsp_port}/live/ch0"], timeout=30)  # hygiene: allow-test-vector
            times = [float(value.strip(',')) for value in probe.stdout.split() if value.strip(',')]
            assert len(times) >= 20, probe.stdout
            # ffmpeg switches from its first-packet timeline to the RTCP sender
            # report's when the first report arrives, so its first step can be
            # anything; every later one must be exactly 20 ms. The RTP step
            # itself is checked exactly by test_rtsp.py's audio timeline test.
            assert times[1] > times[0], times[:4]
            steps = {round(b - a, 3) for a, b in zip(times[1:], times[2:])}
            assert steps == {0.02}, (steps, times[:40])
            result["checks"].append("FFprobe reads the microphone track with exact 20 ms packet spacing")
            onvif_profile_order(connection, secret)
            result["checks"].append("ONVIF profile children in schema order with audio enabled")
            ui_connection = (lambda: http.client.HTTPSConnection("127.0.0.1", ui_port, context=context, timeout=5)) \
                if args.tls else None
            result["ui"] = ui_checks(connection, "https" if args.tls else "http", http_port, secret, platform_seen,
                                     ui_connection)
            result["checks"].append("admin UI: public files with CSP, platform sign-in relay, session cookie "
                                    "with CSRF and Origin on the media API, no Basic dialog for the UI, "
                                    "streamed platform package upload, the UI's own HTTPS port")
            if os.environ.get("GO2RTC_BIN") and not args.tls:
                go2rtc_interop(os.environ["GO2RTC_BIN"], root, rtsp_port, secret, request)
                result["checks"].append("go2rtc talk stream offers a sendonly PCMA track and plays a pushed file")
            http_args = ["python3", here / "test_http_onvif.py", "--port", http_port,
                         "--username", "admin", "--password", secret]
            if args.tls:
                http_args += ["--tls", "--ca", root / "cert.pem"]
            protocol = run(http_args, timeout=100)
            result["http_onvif_protocol"] = protocol.stderr.strip()
            discovery_args = ["python3", here / "test_discovery.py", "--port", discovery_port,
                              "--http-port", http_port]
            if args.tls:
                discovery_args += ["--tls"]
            protocol = run(discovery_args, timeout=20)
            result["discovery_protocol"] = protocol.stderr.strip()
            def decode(transport, channel):
                url = f"rtsp://admin:{secret}@127.0.0.1:{rtsp_port}/live/ch{channel}"  # hygiene: allow-test-vector
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-threads", "1",
                     "-rtsp_transport", transport, "-i", url, "-t", "1", "-map", "0:v:0", "-threads:v", "1", "-f", "null", "-"], timeout=25)
                return f"FFmpeg decoded channel{channel} via {transport}"
            if args.main_only:
                # Two simultaneous subscribers exercise each alias pair.
                for pair in ((0,1),(2,3)):
                    with ThreadPoolExecutor(max_workers=2) as pool:
                        jobs=[pool.submit(decode,transport,ch) for transport,ch in zip(("tcp","udp"),pair)]
                        result["checks"].extend(j.result() for j in jobs)
                client = (http.client.HTTPSConnection("127.0.0.1",http_port,context=context,timeout=5)
                          if args.tls else http.client.HTTPConnection("127.0.0.1",http_port,timeout=5))
                headers={"Authorization":"Basic "+base64.b64encode(f"admin:{secret}".encode()).decode(),"Content-Type":"application/soap+xml"}
                def soap(action, inner=""):
                    client.request("POST","/onvif/media_service",body=f"<Envelope><Body><{action}>{inner}</{action}></Body></Envelope>",headers=headers)
                    reply=client.getresponse();return reply.status,reply.read()
                code,body=soap("GetProfiles");assert code==200
                profiles=ET.fromstring(body).findall(".//{http://www.onvif.org/ver10/media/wsdl}Profiles")
                ns={"tt":"http://www.onvif.org/ver10/schema"}
                for i,p in enumerate(profiles):
                    enc=p.find("tt:VideoEncoderConfiguration",ns)
                    size=(int(enc.findtext("tt:Resolution/tt:Width",namespaces=ns)),int(enc.findtext("tt:Resolution/tt:Height",namespaces=ns)))
                    assert size==((320,240) if i<2 else (160,120)),size
                    assert soap("GetSnapshotUri",f"<ProfileToken>{p.attrib['token']}</ProfileToken>")[0]==500
                assert len(profiles)==4
                code,body=soap("GetServiceCapabilities");assert code==200 and b'SnapshotUri="false"' in body
                client.close()
                streams=json.loads(request("GET","/api/v1/status")[1])["streams"]
                assert streams[1]["alias_of"]==0 and streams[3]["alias_of"]==2
                result["checks"].append("all logical profiles report effective geometry; alias subscribers decode concurrently; JPEG capability disabled")
            else:
                for transport,channel in (("tcp",0),("udp",3)):
                    result["checks"].append(decode(transport,channel))
            assert request("POST", "/api/v1/control", {"operation": "white_light", "value": 1})[0] == 200
            assert request("POST", "/api/v1/control", {"operation": "preset_set", "value": 1})[0] == 200
            assert request("POST", "/api/v1/control", {"operation": "ptz", "value": 50, "value2": -25})[0] == 200
            time.sleep(1.5)
            status = json.loads(request("GET", "/api/v1/status")[1])
            assert not status["platform"]["moving"]
            result["checks"].append("replay PTZ deadman, presets and light control")
            assert request("POST", "/api/v1/recording", {"enabled": 1})[0] == 200
            time.sleep(2.5)
            assert request("POST", "/api/v1/recording", {"enabled": 0})[0] == 200
            time.sleep(0.2)
            listing = json.loads(request("GET", "/api/v1/recordings")[1])["recordings"]
            if args.main_only:
                assert not any(item["name"].startswith(("stream-1-","stream-3-")) for item in listing)
                assert all(any(item["name"].startswith(f"stream-{ch}-") for item in listing) for ch in (0,2))
            video = next(item["name"] for item in listing if item["name"].endswith(".h264") and item["size"] > 1024)
            code, body = request("GET", "/api/v1/recordings/" + video)
            assert code == 200 and len(body) > 1024
            recording = root / "download.h264"
            recording.write_bytes(body)
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-threads", "1",
                 "-i", recording, "-f", "null", "-"], timeout=15)
            result["checks"].append("recording rotation, authenticated download and FFmpeg decode")
            new_secret = "changed-" + secrets.token_hex(12)
            assert request("POST", "/api/v1/config", {"password": new_secret})[0] == 200
            assert request("GET", "/api/v1/status", password=secret)[0] == 401
            secret = new_secret
            assert request("GET", "/api/v1/status")[0] == 200
            contents = config.read_text()
            for key, value in [("input0", ""), ("http_port", "8080"), ("rtsp_port", "8554")]:
                contents = "\n".join(f"{key}={value}" if line.startswith(key + "=") else line
                                     for line in contents.splitlines()) + "\n"
            config.write_text(contents)
            process.send_signal(signal.SIGHUP)
            time.sleep(0.5)
            wait_ready()
            assert (config.stat().st_mode & 0o777) == 0o600
            result["checks"].append("credential persistence, old-password rejection, CLI overrides retained on reload")
            # Last: this locks 127.0.0.1 out for a minute. Earlier negative
            # tests may already have counted, so the block can come sooner.
            statuses = [request("GET", "/api/v1/status", password="not-" + secret)[0] for _ in range(10)]
            assert set(statuses) <= {401, 429}, statuses
            # After ten failures even the right password is not checked.
            assert request("GET", "/api/v1/status")[0] == 429
            assert request("GET", "/health")[0] == 200
            result["checks"].append("failed-login throttle blocks an address after repeated wrong passwords")
        finally:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
                raise AssertionError("Daemon did not shut down within15seconds")
            ntp_stop.set(); ntp_thread.join(); ntp_socket.close()
            log.close()
            result["daemon_exit"] = process.returncode
            result["daemon_log"] = (root / "daemon.log").read_text()
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(result, indent=2) + "\n")
        assert process.returncode == 0, result["daemon_log"]
    print(json.dumps({key: value for key, value in result.items() if key != "daemon_log"}, indent=2))


if __name__ == "__main__":
    main()
