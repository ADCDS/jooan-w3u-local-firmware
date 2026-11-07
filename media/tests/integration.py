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
    siren_checks(request, capture, context_status)
    return status["audio"]


def media_update_checks(call, ui_post, secret, update_root):
    """The browser's media update: who may, what is refused and why, and what each step
    leaves. The verifier and both scripts here are this test's own fixtures: nothing can
    flash or restart anything."""
    verifier, marker = update_root / "verifier.sh", update_root / "applied"
    verifier.write_text(f'#!/bin/sh\n[ ! -e "{update_root}/verify-fails" ]\n')
    verifier.chmod(0o755)
    upload = ui_post | {"X-CSRF-Token": "c1", "Content-Type": "application/octet-stream"}
    post = ui_post | {"X-CSRF-Token": "c1"}
    apply_script = (f'#!/bin/sh\n[ "$1" = check ] && {{ [ ! -e "{update_root}/check-fails" ] || {{ echo "Built from something else"; exit 1; }}; exit 0; }}\n'
                    f'[ ! -e "{update_root}/apply-fails" ] || {{ echo "1.00 ABORT: platform maintenance is in progress"; exit 1; }}\n'
                    f'echo "$1" > "{marker}"\n').encode()

    def package(kind=b"media", release=b"cafe0123-8b374e7a", image=3211264, listed=b"  appfs.img\n  upgrade-appfs.sh\n  apply-media-update.sh\n  native-boot.sh\n"):
        manifest = (b"JOOAN-SIGNED-RELEASE-V1\ntarget_id=jooan-ja-a12-t23n-dual-cv2005-skw6316\ndevice_model=JA-A12\n"
                    b"release_version=" + release + b"\nartifact_kind=" + kind + b"\nfiles-begin\n" + listed + b"files-end\n")
        signature, upgrade, boot = b"signature", b"#!/bin/sh\n", b"#!/bin/sh\n"
        header = f"JIPCMEDIA2 {len(manifest)} {len(signature)} {len(upgrade)} {len(apply_script)} {len(boot)} {image}".encode()
        return header.ljust(95) + b"\n" + manifest + signature + upgrade + apply_script + boot + b"\xff" * image

    def state():
        status, _, body = call("GET", "/api/v1/update/media", headers=ui_post)
        assert status == 200, body
        return json.loads(body)

    def idle():
        assert state() == {"state": "idle", "release": None, "error": ""}, state()
        kept = update_root / "staged"
        assert not kept.exists() or not list(kept.iterdir()), "a refused package is not left in RAM"

    good = package()
    idle()
    # Only the signed-in admin's own page: never the stream account, nor another site.
    basic = {"Authorization": "Basic " + base64.b64encode(f"admin:{secret}".encode()).decode(), "X-Requested-With": "jipc-ui",
             "Content-Type": "application/octet-stream"}
    assert call("POST", "/api/v1/update/media", b"x", basic)[0] == 403
    assert call("GET", "/api/v1/update/media", headers=basic)[0] == 403
    assert call("POST", "/api/v1/update/media", b"x", {"Content-Type": "application/octet-stream"})[0] == 401
    assert call("POST", "/api/v1/update/media", b"x", upload | {"Sec-Fetch-Site": "cross-site"})[0] == 403
    assert call("POST", "/api/v1/update/media", b"x", upload | {"X-CSRF-Token": "c2"})[0] == 403
    idle()
    # Refused for what it is, and nothing stays behind.
    assert call("POST", "/api/v1/update/media", b"", upload)[0] != 200
    for body, code, why in ((b"short", 400, b"not a media package"),
                            (b"JOOAN_FW_PKG" + b"x" * 200, 400, b"not a media package"),
                            (package(image=4096), 400, b"for this camera"),
                            (package(kind=b"install"), 400, b"not a media release"),
                            (package(release=b"../../etc"), 400, b"not a media release"),
                            (package(listed=b"  appfs.img\n"), 400, b"does not cover")):
        status, _, reply = call("POST", "/api/v1/update/media", body, upload)
        assert status == code and why in reply, (status, reply, why)
        idle()
    assert call("POST", "/api/v1/update/media", b"x", upload | {"Content-Type": "application/json"})[0] == 415
    # Too large is refused from its header alone.
    assert call("POST", "/api/v1/update/media", None, upload | {"Content-Length": "3400001"})[0] == 413
    # The signature decides, and its script is not run unless it holds.
    (update_root / "verify-fails").write_text("")
    (update_root / "check-fails").write_text("")
    status, _, reply = call("POST", "/api/v1/update/media", good, upload)
    assert status == 400 and b"signature" in reply, (status, reply)
    (update_root / "verify-fails").unlink()
    idle()
    # The package's own check says why it cannot go on this camera.
    status, _, reply = call("POST", "/api/v1/update/media", good, upload)
    assert status == 409 and b"Built from something else" in reply, (status, reply)
    (update_root / "check-fails").unlink()
    idle()
    assert call("POST", "/api/v1/update/media/apply", json.dumps({"release": "cafe0123-8b374e7a"}), post)[0] == 409
    # A good one is kept, and goes nowhere until asked.
    status, _, reply = call("POST", "/api/v1/update/media", good, upload)
    assert status == 200 and json.loads(reply) == {"release": "cafe0123-8b374e7a"}, (status, reply)
    assert state() == {"state": "staged", "release": "cafe0123-8b374e7a", "error": ""}
    assert not marker.exists()
    assert call("POST", "/api/v1/update/media/apply", json.dumps({"release": "deadbeef-8b374e7a"}), post)[0] == 409
    assert call("POST", "/api/v1/update/media/apply", "{}", post)[0] == 400
    assert call("POST", "/api/v1/update/media/apply", json.dumps({"release": "cafe0123-8b374e7a"}), basic | {"Content-Type": "application/json"})[0] == 403
    assert not marker.exists()
    # If it changed in RAM since the check, it does not go.
    (update_root / "verify-fails").write_text("")
    assert call("POST", "/api/v1/update/media/apply", json.dumps({"release": "cafe0123-8b374e7a"}), post)[0] == 400
    (update_root / "verify-fails").unlink()
    assert not marker.exists()
    idle()
    # Discarded, it is gone.
    assert call("POST", "/api/v1/update/media", good, upload)[0] == 200
    assert call("POST", "/api/v1/update/media/discard", "{}", post)[0] == 200
    idle()
    # A script that stops short of the flash says why, and the package can be tried again.
    assert call("POST", "/api/v1/update/media", good, upload)[0] == 200
    (update_root / "apply-fails").write_text("")
    assert call("POST", "/api/v1/update/media/apply", json.dumps({"release": "cafe0123-8b374e7a"}), post)[0] == 200
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not state()["error"]:
        time.sleep(0.1)
    assert "platform maintenance" in state()["error"] and state()["state"] == "staged", state()
    (update_root / "apply-fails").unlink()
    # And then it runs.
    assert call("POST", "/api/v1/update/media/apply", json.dumps({"release": "cafe0123-8b374e7a"}), post)[0] == 200
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not marker.exists():
        time.sleep(0.1)
    assert marker.read_text() == "apply\n"
    assert call("POST", "/api/v1/update/media/discard", "{}", post)[0] == 200
    # A failure's reason goes with what it was about, and an abandoned upload leaves by itself.
    assert state()["error"] == ""
    assert call("POST", "/api/v1/update/media", good, upload)[0] == 200
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline and state()["state"] != "idle":
        time.sleep(0.2)
    idle()


def siren_checks(request, capture, context_status):
    """The siren through the HTTP operations: its saved settings, a test, a talker
    kept out meanwhile, and that it stops."""
    def op(operation, value=0, value2=0):
        return request("POST", "/api/v1/control", {"operation": operation, "value": value, "value2": value2})[0]

    def speaker():
        return json.loads(request("GET", "/api/v1/status")[1])["audio"]["speaker"]

    def wait_for(check, what):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if check():
                return
            time.sleep(0.05)
        raise AssertionError(f"{what}: {speaker()}")

    wait_for(lambda: speaker()["state"] == "idle", "idle before the siren")
    defaults = json.loads(request("GET", "/api/v1/controls")[1])["siren"]
    assert defaults == {"sound": "alarm", "volume": 100, "seconds": 30, "strobe": False}, defaults
    for bad in ({"sound": "bell"}, {"sound": 1}, {"volume": 0}, {"volume": 101}, {"seconds": 0}, {"seconds": 301}, {"strobe": 1}):
        assert request("POST", "/api/v1/controls", {"siren": bad})[0] == 400, bad
    assert request("POST", "/api/v1/controls", {"siren": {"sound": "pulse", "volume": 40, "seconds": 60}})[0] == 200
    size = capture.stat().st_size
    assert op("siren", 1) == 200
    wait_for(lambda: speaker()["state"] == "siren", "the siren starts")
    state = speaker()
    assert state["siren"]["sound"] == "pulse" and 50 < state["siren"]["left_s"] <= 60, state
    assert request_talk(context_status, b"\xd5" * 800)[0] == 409
    wait_for(lambda: capture.stat().st_size > size + 4000, "the siren plays")
    assert op("siren", 0) == 200
    wait_for(lambda: speaker()["state"] == "idle" and speaker()["siren"] is None, "the siren stops")
    played = capture.read_bytes()[size:]
    assert len(set(played)) > 8, "the siren should be more than silence"
    # A length given with the request; a test of another sound, which ends by itself.
    assert op("siren", 1, 1) == 200
    wait_for(lambda: speaker()["state"] == "siren", "a short siren starts")
    assert speaker()["siren"]["left_s"] <= 1
    wait_for(lambda: speaker()["state"] == "idle", "a short siren ends")
    assert op("siren_test", 3) == 200
    wait_for(lambda: speaker()["state"] == "siren", "the test starts")
    state = speaker()
    assert state["siren"]["sound"] == "chime" and state["siren"]["left_s"] <= 3, state
    assert op("siren", 0) == 200
    wait_for(lambda: speaker()["state"] == "idle", "the test stops")
    # The flashing light goes with its siren, whichever way the siren ends, and says so while it lasts.
    def strobe_left():
        return json.loads(request("GET", "/api/v1/controls")[1])["live"]["strobe_left_s"]

    def wait_strobe(check, what):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if check(strobe_left()):
                return
            time.sleep(0.05)
        raise AssertionError(f"{what}: {strobe_left()}")

    assert request("POST", "/api/v1/controls", {"siren": {"strobe": True}})[0] == 200
    assert op("siren", 1, 20) == 200
    wait_strobe(lambda left: left > 0, "the siren brings its flashing")
    # ...on the pin as well as in the report.
    seen = set()
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline and len(seen) < 2:
        seen.add(json.loads(request("GET", "/api/v1/status")[1])["platform"]["white_light"])
        time.sleep(0.1)
    assert seen == {0, 1}, seen
    assert op("siren", 0) == 200
    wait_strobe(lambda left: left == 0, "the siren's stop ends it")
    wait_for(lambda: speaker()["state"] == "idle", "idle after stopping")
    assert op("siren", 1, 20) == 200
    wait_strobe(lambda left: left > 0, "the siren again")
    assert op("siren_test", 0) == 200
    wait_strobe(lambda left: left == 0, "a test sound replaces the siren, and ends the flashing")
    assert op("siren", 0) == 200
    wait_for(lambda: speaker()["state"] == "idle", "idle after the test")
    assert op("strobe", 20) == 200
    wait_strobe(lambda left: left > 0, "flashing by itself")
    assert request("POST", "/api/v1/controls", {"white_light": {"on": False}})[0] == 200
    wait_strobe(lambda left: left == 0, "choosing off ends it")
    assert request("POST", "/api/v1/controls", {"siren": {"strobe": False}})[0] == 200
    for operation, value, value2 in (("siren", 2, 0), ("siren", 1, 301), ("siren_test", 4, 0), ("siren_test", -1, 0)):
        assert op(operation, value, value2) != 200, (operation, value, value2)
    assert request("POST", "/api/v1/controls", {"siren": defaults})[0] == 200


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
            if self.path == "/api/v1/network/wifi" and self.command == "POST":  # a real join takes up to 65 s
                time.sleep(16)
                return self.reply(200, b'{"ok":true}')
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
    assert controls()["live"]["hold_left_s"] > 0, controls()["live"]  # no flapping right after a switch
    set_ev(20000)  # between the levels: stays night (hysteresis)
    time.sleep(6.5)
    assert controls()["live"]["night"] == "night"
    set_ev(9000)
    wait_for(lambda: controls()["live"]["night"] == "day" and platform()["night"] == 0, 20, "auto to day")
    # Leaving privacy: its time in day mode counts as a start's settling. A dark scene goes to
    # night at once, and only the rest of the wait is left after a short privacy.
    set_ev(40000)
    assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"], 3, "privacy on")
    time.sleep(4.5)
    assert platform()["night"] == 0
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    wait_for(lambda: controls()["live"]["night"] == "night" and platform()["night"] == 1, 1.5, "night at once after privacy")
    assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"], 3, "privacy on again")
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    wait_for(lambda: not controls()["live"]["privacy"], 3, "privacy off")
    assert controls()["live"]["night"] == "day" and "settle" in controls()["live"]["reason"], controls()["live"]
    wait_for(lambda: controls()["live"]["night"] == "night", 6, "night after the rest of the wait")
    set_ev(10000)
    # Validation leaves everything as it was.
    for bad in ({"night": {"night_ev": 100, "day_ev": 200}}, {"night": {"from": "25:00"}},
                {"night": {"mode": "dusk"}}, {"picture": [{"denoise": 300}]}, {"anti_flicker": "40"}, [1]):
        assert request("POST", "/api/v1/controls", bad)[0] == 400, bad
    assert controls()["night"]["mode"] == "auto"
    # The camera's zone (vendor config, GMT-03:00) wins over the browser's offset (0).
    assert controls()["live"]["zone"] == "camera", controls()["live"]
    now = time.gmtime()
    start = (now.tm_hour * 60 + now.tm_min - 180 - 1) % 1440
    end = (start + 4) % 1440
    status, _ = request("POST", "/api/v1/controls", {"utc_offset_min": 0, "night": {
        "mode": "schedule", "from": f"{start // 60:02d}:{start % 60:02d}", "until": f"{end // 60:02d}:{end % 60:02d}"}})
    assert status == 200
    wait_for(lambda: controls()["live"]["night"] == "night", 5, "schedule night")
    assert controls()["utc_offset_min"] == -180 and \
        controls()["live"]["local_minutes"] == (int(time.time()) // 60 - 180) % 1440, controls()["live"]
    # A clock nothing has confirmed: schedules do not trust it, and privacy fails closed.
    marker = root / "clock-set"
    outside = (now.tm_hour * 60 + now.tm_min - 180 + 300) % 1440
    window = {"enabled": True, "from": f"{outside // 60:02d}:{outside % 60:02d}",
              "until": f"{(outside + 10) // 60 % 24:02d}:{(outside + 10) % 60:02d}"}
    assert controls()["live"]["clock_ok"] is True
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": window}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"] is False, 5, "outside the window")
    marker.unlink()
    wait_for(lambda: controls()["live"]["clock_ok"] is False and controls()["live"]["privacy"] is True, 5,
             "with no confirmed clock, a privacy window stays private")
    assert "privacy" in controls()["live"]["reason"]
    # An out-of-range value is refused during privacy too, not saved for later.
    assert request("POST", "/api/v1/control", {"operation": "brightness", "value": 9999, "value2": 0})[0] != 200
    # The other schedules wait too.
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": {"enabled": False}}})[0] == 200
    wait_for(lambda: "waiting for the clock" in controls()["live"]["reason"], 5, "night schedule waits")
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": window}})[0] == 200
    marker.write_text("")
    wait_for(lambda: controls()["live"]["clock_ok"] is True and controls()["live"]["privacy"] is False, 5, "clock confirmed")
    # "On" by hand under a schedule is saved as a time, so it survives a restart, and goes with "off".
    assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
    state = controls()
    assert state["live"]["privacy"] is True or wait_for(lambda: controls()["live"]["privacy"] is True, 5, "on by hand") is None
    assert controls()["privacy"]["on"] is False and controls()["privacy"]["held_until"] > time.time(), controls()["privacy"]
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    assert "held_until" not in controls()["privacy"]
    # Turning a schedule on during manual privacy keeps it private until the next edge, then off for good.
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": {"enabled": False}}})[0] == 200
    assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"] is True, 5, "manual privacy")
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": window}})[0] == 200
    time.sleep(1.5)
    state = controls()
    assert state["live"]["privacy"] is True and state["privacy"]["on"] is False and state["privacy"]["held_until"] > time.time(), state["privacy"]
    # Turning that schedule off keeps it on, by hand: nothing the owner chose ends by surprise.
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": {"enabled": False}}})[0] == 200
    time.sleep(1.2)
    state = controls()
    assert state["live"]["privacy"] is True and state["privacy"]["on"] is True and "held_until" not in state["privacy"], state["privacy"]
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"] is False, 5, "off by hand")
    # Privacy runs in day mode, whatever is chosen: the ISP's night mode undoes the black-out.
    assert request("POST", "/api/v1/controls", {"night": {"mode": "night"}})[0] == 200
    wait_for(lambda: platform()["night"] == 1, 5, "night chosen")
    assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
    wait_for(lambda: platform()["night"] == 0 and controls()["live"]["privacy"] is True, 5, "day mode for privacy")
    assert "privacy" in controls()["live"]["reason"] and controls()["night"]["mode"] == "night"
    # Nothing goes around that meanwhile, and what was asked for still counts afterwards.
    for operation in ("night", "isp_night", "ircut"):
        assert request("POST", "/api/v1/control", {"operation": operation, "value": 1, "value2": 0})[0] == 200
    time.sleep(1.5)
    assert platform()["night"] == 0, platform()
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    wait_for(lambda: platform()["night"] == 1 and controls()["live"]["privacy"] is False, 5, "night again after privacy")
    assert request("POST", "/api/v1/controls", {"night": {"mode": "day"}})[0] == 200
    # The board's health for the admin UI, native already while the boot qualifies.
    qualified = (root / "jipc-native.log").read_text()
    (root / "jipc-native.log").write_text("53.82 jipc-native: media daemon pid=1 ready; qualifying for 185 seconds\n")
    assert json.loads(request("GET", "/api/v1/status")[1])["system"]["boot_mode"] == "native"
    (root / "jipc-native.log").write_text("1.00 jipc-native: starting\n")
    assert json.loads(request("GET", "/api/v1/status")[1])["system"]["boot_mode"] == "unknown"
    # A long wait for the gateway writes kilobytes before the line that counts.
    (root / "jipc-native.log").write_text("waiting for the gateway\n" * 400 + qualified)
    system = json.loads(request("GET", "/api/v1/status")[1])["system"]
    assert system["boot_mode"] == "native" and system["uptime_s"] > 0 and system["memfree_kb"] > 0 and \
        system["state_free_kb"] > 0, system
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
    # A rename while someone is steering waits: it writes flash, and only the lease stops the head.
    if request("POST", "/api/v1/control", {"operation": "preset_set", "value": 16, "value2": 0})[0] == 200:
        assert request("POST", "/api/v1/control", {"operation": "ptz", "value": 50, "value2": 0})[0] == 200
        status, _ = request("POST", "/api/v1/control", {"operation": "preset_rename", "value": 16, "value2": 0, "name": "Renamed"})
        assert status == 409, status
        assert request("POST", "/api/v1/control", {"operation": "ptz_stop", "value": 0, "value2": 0})[0] == 200
        assert request("POST", "/api/v1/control", {"operation": "preset_remove", "value": 16, "value2": 0})[0] == 200
    # Saved (after a short debounce) and readable.
    saved = root / "state" / "controls.json"
    wait_for(lambda: saved.exists() and json.loads(saved.read_text())["night"]["ir"] == "off", 8, "controls.json")
    stored = json.loads(saved.read_text())
    assert stored["v"] == 1 and stored["night"]["mode"] == "auto" and oct(saved.stat().st_mode & 0o777) == "0o600", stored
    request("POST", "/api/v1/controls", {"night": {"mode": "day", "ir": "night"}})
    imaging = imaging_checks(connection, secret, controls, request) if connection else None
    return {"saved": stored["night"], "imaging": imaging}


def ptz_checks(request, connection, secret):
    """Wave D: ONVIF moves in the shapes Frigate sends, recalibrate, patrol and
    privacy mode. The replay backend's head moves at 70 units per second."""
    ns = {"s": "http://www.w3.org/2003/05/soap-envelope", "tt": "http://www.onvif.org/ver10/schema",
          "tptz": "http://www.onvif.org/ver20/ptz/wsdl"}
    spaces = "http://www.onvif.org/ver10/tptz/PanTiltSpaces/"

    def soap(body):
        client = connection()
        try:
            client.request("POST", "/onvif/ptz_service", body=(
                '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" '
                f'xmlns:tt="http://www.onvif.org/ver10/schema"><s:Body>{body}</s:Body></s:Envelope>'), headers={
                "Authorization": "Basic " + base64.b64encode(f"admin:{secret}".encode()).decode(),
                "Content-Type": "application/soap+xml"})
            reply = client.getresponse()
            return reply.status, ET.fromstring(reply.read())
        finally:
            client.close()

    def status():
        code, root = soap('<tptz:GetStatus><tptz:ProfileToken>main0</tptz:ProfileToken></tptz:GetStatus>')
        assert code == 200, code
        point = root.find(".//tt:Position/tt:PanTilt", ns)
        return float(point.get("x")), float(point.get("y")), root.find(".//tt:MoveStatus/tt:PanTilt", ns).text

    def settle(seconds=8):
        end = time.time() + seconds
        seen = None
        while time.time() < end:
            seen = status()
            if seen[2] == "IDLE":
                return seen
            time.sleep(0.1)
        raise AssertionError(f"still moving: {seen}")

    def platform():
        return json.loads(request("GET", "/api/v1/status")[1])["platform"]

    def controls():
        return json.loads(request("GET", "/api/v1/controls")[1])

    def wait_for(check, seconds, what):
        end = time.time() + seconds
        while time.time() < end:
            if check():
                return
            time.sleep(0.1)
        raise AssertionError(f"timed out waiting for {what}: {controls()['live']}")

    def move(tag, element, x, y, space=None, speed=None):
        attr = f' space="{space}"' if space else ""
        sp = f'<tptz:Speed><tt:PanTilt x="{speed}" y="{speed}"/></tptz:Speed>' if speed is not None else ""
        return soap(f'<tptz:{tag}><tptz:ProfileToken>main0</tptz:ProfileToken><tptz:{element}><tt:PanTilt x="{x}" y="{y}"{attr}/>'
                    f'</tptz:{element}>{sp}</tptz:{tag}>')

    # What Frigate looks for: the FOV translation space, in a schema-ordered list.
    code, root = soap('<tptz:GetConfigurationOptions><tptz:ConfigurationToken>ptz0</tptz:ConfigurationToken></tptz:GetConfigurationOptions>')
    assert code == 200
    children = [c.tag.split("}")[1] for c in root.find(".//tt:Spaces", ns)]
    assert children == ["AbsolutePanTiltPositionSpace", "RelativePanTiltTranslationSpace", "RelativePanTiltTranslationSpace",
                        "ContinuousPanTiltVelocitySpace", "PanTiltSpeedSpace"], children
    uris = [e.text for e in root.findall(".//tt:RelativePanTiltTranslationSpace/tt:URI", ns)]
    assert uris == [spaces + "TranslationGenericSpace", spaces + "TranslationSpaceFov"], uris
    code, root = soap('<tptz:GetConfigurations/>')
    config = [c.tag.split("}")[1] for c in root.find(".//tptz:PTZConfiguration", ns)]
    assert config.index("DefaultAbsolutePantTiltPositionSpace") < config.index("DefaultRelativePanTiltTranslationSpace") < \
        config.index("DefaultContinuousPanTiltVelocitySpace") < config.index("DefaultPTZTimeout"), config
    code, root = soap('<tptz:GetNodes/>')
    assert root.find(".//tt:HomeSupported", ns).text == "true"
    # Moves report MOVING until they end, and land where they were told to.
    assert move("AbsoluteMove", "Position", 0, 0, spaces + "PositionGenericSpace")[0] == 200
    settle()
    assert move("AbsoluteMove", "Position", 0.5, -0.25, spaces + "PositionGenericSpace", 1)[0] == 200
    assert status()[2] == "MOVING"
    x, y, _ = settle()
    assert abs(x - 0.5) < 0.02 and abs(y + 0.25) < 0.02, (x, y)
    assert move("RelativeMove", "Translation", 0.5, 0.5, spaces + "TranslationSpaceFov", 1)[0] == 200  # a quarter of the range in replay
    x2, y2, _ = settle()
    assert abs(x2 - 0.75) < 0.02 and abs(y2 - 0.0) < 0.02, (x2, y2)
    assert move("RelativeMove", "Translation", -0.25, 0, spaces + "TranslationGenericSpace")[0] == 200
    x3, _, _ = settle()
    assert abs(x3 - 0.5) < 0.02, x3
    assert move("RelativeMove", "Translation", 0.1, 0, "http://example.com/other")[0] == 500  # a SOAP fault
    assert move("AbsoluteMove", "Position", 2, 0)[0] == 500
    # Home: saved, then gone back to.
    assert soap('<tptz:SetHomePosition><tptz:ProfileToken>main0</tptz:ProfileToken></tptz:SetHomePosition>')[0] == 200
    assert move("AbsoluteMove", "Position", -0.5, -0.5)[0] == 200
    settle()
    assert soap('<tptz:GotoHomePosition><tptz:ProfileToken>main0</tptz:ProfileToken></tptz:GotoHomePosition>')[0] == 200
    x4, _, _ = settle()
    assert abs(x4 - 0.5) < 0.02, x4
    # Recalibrate reports when it was done.
    assert request("POST", "/api/v1/control", {"operation": "ptz_recalibrate", "value": 0, "value2": 0})[0] == 200
    assert platform()["homed_ago_s"] is not None
    # Patrol: stop, stay, next stop; steering pauses it; it survives an unknown slot.
    for slot, x, y in ((5, -0.4, 0), (6, 0.4, 0)):
        assert move("AbsoluteMove", "Position", x, y)[0] == 200
        settle()
        assert request("POST", "/api/v1/control", {"operation": "preset_set", "value": slot, "value2": 0, "name": f"Stop {slot}"})[0] == 200
    bad = request("POST", "/api/v1/controls", {"patrol": {"stops": [{"slot": 99}]}})
    assert bad[0] == 400, bad
    assert request("POST", "/api/v1/controls", {"patrol": {"stops": [{"slot": 5, "dwell_s": 2}]}})[0] == 400  # at least 3 s
    assert request("POST", "/api/v1/controls", {"patrol": {"enabled": True, "pause_s": 3, "stops": [
        {"slot": 5, "dwell_s": 3}, {"slot": 15, "dwell_s": 3}, {"slot": 6, "dwell_s": 3}]}})[0] == 200
    wait_for(lambda: controls()["live"]["patrol"]["state"] == "paused", 3, "the pause after the last manual move")
    wait_for(lambda: controls()["live"]["patrol"]["slot"] in (5, 15) and controls()["live"]["patrol"]["state"] == "dwell", 15, "the first stop")
    assert abs(status()[0] + 0.4) < 0.03, status()
    wait_for(lambda: controls()["live"]["patrol"]["slot"] == 6 and controls()["live"]["patrol"]["state"] == "dwell", 15, "the second stop, past the empty slot")
    assert abs(status()[0] - 0.4) < 0.03, status()
    assert request("POST", "/api/v1/control", {"operation": "ptz", "value": 20, "value2": 0})[0] == 200
    wait_for(lambda: controls()["live"]["patrol"]["state"] == "paused", 3, "steering to pause the patrol")
    assert request("POST", "/api/v1/control", {"operation": "ptz_stop", "value": 0, "value2": 0})[0] == 200
    wait_for(lambda: controls()["live"]["patrol"]["state"] in ("moving", "dwell"), 8, "the patrol to resume")
    assert request("POST", "/api/v1/controls", {"patrol": {"enabled": False}})[0] == 200
    wait_for(lambda: controls()["live"]["patrol"]["state"] == "off", 3, "the patrol to stop")
    settle()
    # Privacy: the head looks away and nothing moves it; video keeps flowing (black on a real lens).
    assert move("AbsoluteMove", "Position", 0.1, 0.1)[0] == 200
    before = settle()
    mute_before = json.loads(request("GET", "/api/v1/status")[1])["audio"]["muted"]
    # The timestamp's settings: off until asked for, checked and kept. Replay has no picture to draw on.
    assert controls()["overlay"] == {"enabled": False, "position": "top-left", "format": "ymd", "name": ""}
    for bad in ({"position": "middle"}, {"format": "iso"}, {"name": "a" * 17}, {"name": "caf\u00e9"}, {"name": 7}, {"enabled": 1}):
        assert request("POST", "/api/v1/controls", {"overlay": bad})[0] == 400, bad
    assert request("POST", "/api/v1/controls", {"overlay": {"enabled": True, "position": "bottom-right", "format": "dmy", "name": "camera1"}})[0] == 200
    assert controls()["overlay"] == {"enabled": True, "position": "bottom-right", "format": "dmy", "name": "camera1"}
    assert request("POST", "/api/v1/controls", {"overlay": {"enabled": False}})[0] == 200
    assert controls()["overlay"]["enabled"] is False and controls()["overlay"]["name"] == "camera1"

    # The flashing light goes on and off, ends by itself, and leaves the light as it was.
    def control(operation, value=0):
        return request("POST", "/api/v1/control", {"operation": operation, "value": value, "value2": 0})[0]
    light_before = platform()["white_light"]
    assert control("strobe", 301) != 200 and control("strobe", -1) != 200
    started = time.monotonic()
    assert control("strobe", 3) == 200
    seen, deadline = set(), time.monotonic() + 2
    while time.monotonic() < deadline:
        seen.add(platform()["white_light"])
        time.sleep(0.05)
    assert seen >= {0, 1}, seen
    # An off phase of the flash looks like the end, so wait out its time before looking.
    time.sleep(max(0, started + 3.8 - time.monotonic()))
    settled = {platform()["white_light"]}
    for _ in range(12):
        time.sleep(0.1)
        settled.add(platform()["white_light"])
    assert settled == {light_before}, settled
    assert control("strobe", 60) == 200
    time.sleep(0.2)
    assert control("strobe", 0) == 200
    time.sleep(0.6)  # the engine has seen it by now
    settled = {platform()["white_light"]}
    for _ in range(12):
        time.sleep(0.1)
        settled.add(platform()["white_light"])
    assert settled == {light_before}, settled
    # Privacy mode, as an operation for Home Assistant.
    assert control("privacy", 2) != 200
    assert control("privacy", 1) == 200
    wait_for(lambda: controls()["live"]["privacy"] and platform()["privacy"], 3, "privacy by its operation")
    assert control("privacy", 0) == 200
    wait_for(lambda: not controls()["live"]["privacy"] and not platform()["privacy"], 3, "privacy off by its operation")

    assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"] and platform()["privacy"], 3, "privacy mode")
    assert json.loads(request("GET", "/api/v1/status")[1])["audio"]["muted"] is True
    assert abs(settle()[1] - 1.0) < 0.02, status()
    assert request("POST", "/api/v1/control", {"operation": "ptz", "value": 20, "value2": 0})[0] == 409
    assert move("AbsoluteMove", "Position", 0, 0)[0] == 500
    assert request("POST", "/api/v1/control", {"operation": "mic_mute", "value": 0, "value2": 0})[0] == 200
    assert json.loads(request("GET", "/api/v1/status")[1])["audio"]["muted"] is True  # unmuting waits for the end of privacy
    produced = json.loads(request("GET", "/api/v1/status")[1])["streams"][0]
    time.sleep(2.5)
    later = json.loads(request("GET", "/api/v1/status")[1])["streams"][0]
    assert later != produced, "frames must flow again once the black-out is applied"
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    wait_for(lambda: not controls()["live"]["privacy"] and not platform()["privacy"], 3, "privacy off")
    back = settle()
    assert abs(back[0] - before[0]) < 0.02 and abs(back[1] - before[1]) < 0.02, (back, before)
    assert json.loads(request("GET", "/api/v1/status")[1])["audio"]["muted"] is False or mute_before
    # A schedule covering now, and a manual change holding until its next edge.
    now = controls()["live"]["local_minutes"]
    hhmm = lambda m: f"{(m % 1440) // 60:02d}:{(m % 1440) % 60:02d}"
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": {"enabled": True, "from": hhmm(now - 5), "until": hhmm(now + 60)}}})[0] == 200
    wait_for(lambda: controls()["live"]["privacy"], 3, "the schedule")
    assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
    wait_for(lambda: not controls()["live"]["privacy"], 3, "the manual change")
    assert request("POST", "/api/v1/controls", {"privacy": {"schedule": {"enabled": False}}})[0] == 200
    settle()
    return {"position": back[:2]}


def imaging_checks(connection, secret, controls, request):
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
    # ONVIF has no word for a schedule and shows it as AUTO: writing that back must not end it.
    auto_body = ('<ImagingSettings><Brightness xmlns="http://www.onvif.org/ver10/schema">80</Brightness>'
                   '<IrCutFilter xmlns="http://www.onvif.org/ver10/schema">AUTO</IrCutFilter></ImagingSettings>')
    assert request("POST", "/api/v1/controls", {"night": {"mode": "schedule"}})[0] == 200
    assert controls()["night"]["mode"] == "schedule"
    status, tree = soap("/onvif/imaging_service", img.format("SetImagingSettings", auto_body))
    assert status == 200 and controls()["night"]["mode"] == "schedule", controls()["night"]
    assert request("POST", "/api/v1/controls", {"night": {"mode": "auto"}})[0] == 200
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


def tls_refusals(http_port):
    """The daemon's TLS configuration is cut down on purpose; these prove the cut, and that
    nothing has quietly turned a removed suite back on."""
    def handshake(prepare):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.set_ciphers("ALL:@SECLEVEL=0")
        prepare(context)
        with socket.create_connection(("127.0.0.1", http_port), timeout=5) as raw:
            with context.wrap_socket(raw) as secure:
                return secure.version(), secure.cipher()[0]

    def tls12(cipher):
        def prepare(context):
            context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
            context.set_ciphers(cipher + ":@SECLEVEL=0")
        return prepare

    def only13(context):
        context.minimum_version = ssl.TLSVersion.TLSv1_3

    def only11(context):
        context.minimum_version = ssl.TLSVersion.TLSv1
        context.maximum_version = ssl.TLSVersion.TLSv1_1

    assert handshake(tls12("ECDHE-RSA-AES128-GCM-SHA256")) == ("TLSv1.2", "ECDHE-RSA-AES128-GCM-SHA256")
    for name, prepare in (("TLS 1.3 only", only13), ("TLS 1.1", only11), ("RSA key exchange", tls12("AES128-GCM-SHA256")),
                          ("CBC", tls12("ECDHE-RSA-AES128-SHA256")), ("ChaCha20", tls12("ECDHE-RSA-CHACHA20-POLY1305"))):
        try:
            got = handshake(prepare)
        except (ssl.SSLError, OSError):
            continue
        raise AssertionError(f"{name} should be refused, and got {got}")


def idle_connection_checks(http_port, tls):
    """A browser opens connections before it has a request for them. The server
    must not speak first: an unsolicited reply would be read as the answer to the
    client's next request. Over TLS the connection also stays usable."""
    raw = socket.create_connection(("127.0.0.1", http_port), timeout=8)
    if tls:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        raw = context.wrap_socket(raw)
    raw.settimeout(6.5)
    try:
        data = raw.recv(64)
        assert data == b"", f"the server spoke first: {data!r}"
        assert not tls, "the idle TLS connection was closed too early"
    except TimeoutError:
        assert tls, "an idle plain connection must be closed by the worker's deadline"
        raw.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        raw.settimeout(5)
        reply = raw.recv(64)
        assert reply.startswith(b"HTTP/1.1 200"), reply
    finally:
        raw.close()


def ui_checks(connection, scheme, http_port, secret, seen, ui_connection=None, update_root=None):
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
    media_update_checks(call, ui_post, secret, update_root)
    # Sign-out reaches the media API at once (the session cache is dropped).
    # Joining a network takes the platform longer than the relay's usual 15 s.
    slow = connection()
    slow.timeout = 30
    slow.request("POST", "/api/joan/v1/network/wifi", body="{}", headers=ui_post | {"X-CSRF-Token": "c1"})
    reply = slow.getresponse()
    assert reply.status == 200, reply.status
    reply.read()
    slow.close()
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
                            "--http-port", str(http_port), "--rtsp-port", str(rtsp_port), "--duration", "600"]
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
                                    "JIPC_ISP_PROC": str(root / "isp-m0"), "JIPC_AUTO_HOLD_S": "12", "JIPC_AUTO_SETTLE_S": "4",
                                    "JIPC_UPDATE_STAGED_S": "6", "JIPC_UPDATE_DIR": str(root / "staged"), "JIPC_UPDATE_VERIFIER": str(root / "verifier.sh"),
                                    "JIPC_OEM_CONFIG": str(root / "oem-config.json"),
                                    "JIPC_CLOCK_MARKER": str(root / "clock-set"), "JIPC_CLOCK_WAIT_S": "100000", "JIPC_CLOCK_IGNORE_SNTP": "1",
                                    "JIPC_NTP_CONFIG": str(root / "ntp-server"),
                                    "JIPC_BOOT_LOG_DIR": str(root)}
        (root / "clock-set").write_text("")
        (root / "oem-config.json").write_text('{"IpcParam":{"TimeZomeCfg":{\n "GmtTz":\t"GMT-03:00"\n}}}\n')
        (root / "jipc-native.log").write_text("249.68 jipc-native: native boot qualified; OEM application never started\n")
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
            result["ptz"] = ptz_checks(request, connection, secret)
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
            idle_connection_checks(http_port, args.tls)
            result["ui"] = ui_checks(connection, "https" if args.tls else "http", http_port, secret, platform_seen,
                                     ui_connection, update_root=root)
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
                tls_refusals(int(http_port))
                result["checks"].append("TLS 1.3 only, RSA key exchange, CBC and TLS 1.1 are all refused")
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
                    code,body=soap("GetSnapshotUri",f"<ProfileToken>{p.attrib['token']}</ProfileToken>")
                    assert code==200 and f"/api/v1/snapshot?channel={i}".encode() in body.replace(b"&amp;",b"&")
                assert len(profiles)==4
                code,body=soap("GetServiceCapabilities");assert code==200 and b'SnapshotUri="true"' in body
                client.close()
                streams=json.loads(request("GET","/api/v1/status")[1])["streams"]
                assert streams[1]["alias_of"]==0 and streams[3]["alias_of"]==2
                result["checks"].append("all logical profiles report effective geometry; alias subscribers decode concurrently; still URI offered")
            else:
                for transport,channel in (("tcp",0),("udp",3)):
                    result["checks"].append(decode(transport,channel))
            assert request("POST", "/api/v1/control", {"operation": "white_light", "value": 1})[0] == 200
            assert request("POST", "/api/v1/control", {"operation": "preset_set", "value": 1})[0] == 200
            assert request("POST", "/api/v1/control", {"operation": "ptz", "value": 50, "value2": -25})[0] == 200
            time.sleep(1.5)
            status = json.loads(request("GET", "/api/v1/status")[1])
            assert not status["platform"]["moving"]
            code,body=request("POST","/api/v1/aim",{"u":0.5,"v":0.5});assert code==409,(code,body)
            code,body=request("GET","/api/v1/aim");aimed=json.loads(body);assert code==200 and aimed["calibrated"] is False and aimed["view"] is None
            assert request("POST","/api/v1/aim",{"u":2,"v":0.5})[0]==400
            assert request("POST","/api/v1/aim",{"x":[0,60,0,0,0,0],"y":[0,0,60,0,0,0]})[0]==200
            # A calibration is for the fixed lens the way up it was, and says which.
            aim_file=root/"state"/"aim.json"
            stored_aim=json.loads(aim_file.read_text());assert "flip1" in stored_aim,stored_aim
            aim_file.write_text(json.dumps(stored_aim|{"flip1":(stored_aim["flip1"]+1)%4}))
            assert json.loads(request("GET","/api/v1/aim")[1])["calibrated"] is False
            assert request("POST","/api/v1/aim",{"u":0.75,"v":0.25})[0]==409
            aim_file.write_text(json.dumps({k:v for k,v in stored_aim.items() if k!="flip1"}))
            assert json.loads(request("GET","/api/v1/aim")[1])["calibrated"] is True # an older file has no say
            aim_file.write_text(json.dumps(stored_aim))
            assert request("POST","/api/v1/aim",{"u":0.75,"v":0.25})[0]==200
            deadline=time.time()+15
            while time.time()<deadline:
                aimed=json.loads(request("GET","/api/v1/aim")[1])
                if aimed["steps"]==[30,-30]:break
                time.sleep(0.2)
            assert aimed["steps"]==[30,-30] and aimed["calibrated"] is True,aimed
            view=aimed["view"];assert abs(view["u"]-0.75)<0.01 and abs(view["v"]-0.25)<0.01,view
            assert request("POST","/api/v1/aim",{"steps":[0,0]})[0]==200
            result["checks"].append("replay PTZ deadman, presets, aiming from the fixed lens and light control")
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
            viewer = socket.create_connection(("127.0.0.1", rtsp_port), timeout=5)
            viewer.sendall(b"OPTIONS rtsp://127.0.0.1/ RTSP/1.0\r\nCSeq: 1\r\n\r\n")
            assert viewer.recv(4096).startswith(b"RTSP/1.0 200")
            assert request("POST", "/api/v1/config", {"password": new_secret})[0] == 200
            # Whoever had the old password does not keep watching: the warning in the UI is true.
            viewer.settimeout(3)
            assert viewer.recv(4096) == b"", "the session outlived its password"
            viewer.close()
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
            # A start with privacy mode already on: frames are held from before video
            # starts, which must not stop the daemon coming up. On the camera this took
            # the board down: the startup check waits for parameter sets from the frames.
            assert request("POST", "/api/v1/controls", {"privacy": {"on": True}})[0] == 200
            saved = root / "state" / "controls.json"
            end = time.time() + 8
            while time.time() < end and not (saved.exists() and json.loads(saved.read_text())["privacy"]["on"]):
                time.sleep(0.2)
            assert json.loads(saved.read_text())["privacy"]["on"] is True
            process.send_signal(signal.SIGHUP)
            time.sleep(0.5)
            wait_ready()
            assert process.poll() is None, "the daemon must come up with privacy mode on"
            live = json.loads(request("GET", "/api/v1/controls")[1])["live"]
            assert live["privacy"] is True and "privacy" not in live["error"], live
            first = json.loads(request("GET", "/api/v1/status")[1])["streams"][0]["frames"]
            end = time.time() + 6
            while time.time() < end and json.loads(request("GET", "/api/v1/status")[1])["streams"][0]["frames"] <= first:
                time.sleep(0.2)
            assert json.loads(request("GET", "/api/v1/status")[1])["streams"][0]["frames"] > first, \
                "frames resume once the black-out is applied"
            assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
            result["checks"].append("a start with privacy mode on comes up and resumes frames at a keyframe")
            # A start in auto mode. In the dark: day first, then night once the exposure
            # has settled, whatever the first reading said. In the light: day, with no
            # hold, so that dusk is not kept waiting.
            def live_night():
                return json.loads(request("GET", "/api/v1/controls")[1])["live"]
            def until(check, seconds, what):
                end = time.time() + seconds
                while time.time() < end:
                    if check(live_night()):
                        return
                    time.sleep(0.1)
                raise AssertionError(f"{what}: {live_night()}")
            isp = root / "isp-m0"
            isp.write_text(ISP_PROC.format(ev0=45000, ev1=20000))
            assert request("POST", "/api/v1/controls", {"night": {"mode": "auto", "night_ev": 30000, "day_ev": 5000, "wait_s": 1}})[0] == 200
            end = time.time() + 8
            while time.time() < end and json.loads(saved.read_text())["night"]["mode"] != "auto":
                time.sleep(0.2)
            assert json.loads(saved.read_text())["night"]["mode"] == "auto"
            process.send_signal(signal.SIGHUP)
            time.sleep(0.5)
            wait_ready()
            until(lambda v: v["night"] == "day" and "settle" in v["reason"], 3, "a start waits in day")
            until(lambda v: v["night"] == "night" and v["reason"].startswith("auto: started dark, level 45000"), 7, "dark at a start")
            assert live_night()["hold_left_s"] > 0, "a switch to night is held like any other"
            isp.write_text(ISP_PROC.format(ev0=8000, ev1=4000))  # between the two levels
            process.send_signal(signal.SIGHUP)
            time.sleep(0.5)
            wait_ready()
            until(lambda v: v["night"] == "day" and v["reason"].startswith("auto: started light, level 8000"), 8, "light at a start")
            assert live_night()["hold_left_s"] == 0, "staying in day starts no hold"
            isp.write_text(ISP_PROC.format(ev0=45000, ev1=20000))
            until(lambda v: v["night"] == "night", 9, "dusk after a light start")
            # A change of the infrared setting keeps the mode, and takes no new start.
            held = live_night()["since_s"]
            assert request("POST", "/api/v1/controls", {"night": {"ir": "off"}})[0] == 200
            time.sleep(1.5)
            now = live_night()
            assert now["night"] == "night" and "started" not in now["reason"] and now["since_s"] >= held, now
            assert request("POST", "/api/v1/controls", {"night": {"mode": "day", "ir": "night"}})[0] == 200
            result["checks"].append("auto mode at a start waits for the exposure, and only a switch to night is held")
            # One bad value in the saved file costs that section, not the rest: least of all privacy.
            end = time.time() + 8
            while time.time() < end and json.loads(saved.read_text())["night"]["mode"] != "day":
                time.sleep(0.2)
            stored = json.loads(saved.read_text())
            stored["privacy"]["on"] = True
            stored["picture"] = [{"brightness": 9999}]
            saved.write_text(json.dumps(stored))
            process.send_signal(signal.SIGHUP)
            time.sleep(0.5)
            wait_ready()
            live = json.loads(request("GET", "/api/v1/controls")[1])
            assert live["live"]["privacy"] is True and live["picture"][0]["brightness"] == 128, live
            assert request("POST", "/api/v1/controls", {"privacy": {"on": False}})[0] == 200
            result["checks"].append("a bad saved value costs its section only")
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
