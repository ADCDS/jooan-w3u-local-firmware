#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Workstation mock of camera1's HTTP API, for developing the admin UI in media/ui/.

It serves the UI files and imitates both API families the UI uses with one
session: the platform daemon's /api/joan/v1/... (relayed by the media daemon on
the camera) and the media daemon's own /api/v1/....  Standard library only,
plain HTTP, state in memory.  The built-in sample data is anonymized.
Camera controls (/api/v1/controls) survive simulated restarts like the
daemon's; automatic day/night follows a simulated exposure that drifts across
both default levels every 5 minutes, so the wait before a switch is visible.

    python3 media/tools/ui-mock-server.py                # open the printed URL
    python3 media/tools/ui-mock-server.py --homing 20 --latency 300 --wifi-mode lost

Sign in as admin with --password (default "admin").  "Camera not answering":
start with --fail, or toggle it at runtime (the /__mock/ route always works):

    curl "http://127.0.0.1:8080/__mock/fail?on=1"        # ...?on=0 restores

Simulated reboots (firmware apply, Wi-Fi rollback) and --restart-outage use the
same mechanism and end on their own.  A reboot signs every session out.  Like
the camera's relay, at most two /api/joan/ requests run at once (with
--latency they overlap); a third is answered 503.
"""

import argparse
import base64
import ipaddress
import json
import math
import os
import random
import re
import secrets
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

CSP = ("default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob:; connect-src 'self'; "
       "style-src 'self'; script-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
         ".js": "text/javascript; charset=utf-8", ".svg": "image/svg+xml", ".json": "application/json"}
FALLBACK = b"<!doctype html><meta charset=utf-8><title>jooanipc-open</title><p>This build has no admin UI.\n"
JSON_LIMIT, UPDATE_LIMIT, TALK_LIMIT, QUEUE_BYTES = 16384, 2097344, 8000, 16000  # queue: 2 s of A-law
PAN_STEPS, TILT_STEPS, LEASE, NUDGE = 4080, 1230, 1.2, 0.35  # calibrated travel; manual lease; nudge pulse
GENERIC = "unknown, duplicate, invalid or local-only configuration setting"

# Captured from the camera, anonymized.  The password is never returned.
MEDIA_CONFIG = {
    "bind": "0.0.0.0", "username": "admin", "password_set": True, "realm": "jooanipc-open",
    "backend": "imp", "libimp": "/mnt/mtd/jooanipc-open/releases/b2e07752-904d60a6/libimp.so",
    "input0": "", "input1": "", "input2": "", "input3": "", "state_dir": "/opt/jooanipc-open/state",
    "record_dir": "/run/jooanipc-open/recordings", "tls_cert": "/opt/jooanipc-open/cert.pem",
    "tls_key": "/opt/jooanipc-open/key.pem", "ntp_server": "", "http_port": 18443, "rtsp_port": 18554,
    "max_clients": 8, "fps": 15, "bitrate": 3072, "width": 1920, "height": 1080, "sub_width": 320,
    "width1": 640, "height1": 360, "bitrate1": 768, "sub_bitrate": 256, "gop_frames": 15, "sub_height": 240,
    "sensors": 2, "flip0": 3, "flip1": 3, "record_seconds": 300, "record_max_mb": 512, "audio": True,
    "recording": False, "allow_plain_http": False, "discovery": False, "discovery_port": 3702,
    "watchdog": True, "onvif_nudge": True, "main_only": True, "motor_home": True}
# Nearby networks (name, dBm, MHz, secure), and the address a Wi-Fi trial gets.
SCAN = [("HomeNet-IoT", -54, 5180, True), ("HomeNet", -57, 5220, True), ("Neighbour-2G", -68, 2437, True),
        ("Caf? 5G", -74, 5745, True), ("Cafe ?Free?", -79, 2412, False)]  # "?": the scan's stand-in for other characters
TRIAL_ADDRESS = "192.168.1.40"
REMEMBER_SECONDS = 30 * 24 * 3600
PRESETS = {1: ["Gate", -46, 65], 2: ["Street", 20, 64], 3: ["Driveway", 33, 22], 4: ["Yard", 3, 100]}
# What POST /api/v1/config may change; everything else is local-only or unknown.
STRINGS = {"username": 33, "password": 129}  # C buffer sizes, NUL included
BOOLS = {"audio", "recording", "discovery", "onvif_nudge", "main_only"}
NUMBERS = {"max_clients", "fps", "bitrate", "width", "height", "sub_width", "width1", "height1", "bitrate1",
           "sub_bitrate", "gop_frames", "sub_height", "sensors", "flip0", "flip1", "record_seconds", "record_max_mb"}
NO_RESTART = {"username", "password", "recording", "record_seconds", "record_max_mb"}
# The platform's versioned routes, by implementation name (src/daemon/http.c `route`).
V1 = {"v1/setup/password": "password", "v1/status": "status", "v1/network/mdns": "mdns",
      "v1/network/routes": "routes", "v1/network/quality": "network-quality", "v1/time": "time",
      "v1/network/wifi/scan": "wifi/scan", "v1/system/restart": "restart", "v1/system/restart-schedule": "restart-schedule", "v1/system/reset": "reset", "v1/network/ip": "network-ip",
      "v1/timezone": "timezone", "v1/ssh/authorized-keys": "ssh/keys"}


class Reply(Exception):
    """An HTTP answer; route code returns or raises it."""
    def __init__(self, status, body=b"", ctype="application/json", headers=()):
        super().__init__(status)
        self.status, self.body, self.ctype, self.extra = status, body, ctype, list(headers)


class Fail(Exception):
    """A refused control: 409 with the daemon's message."""


class Drop(Exception):
    """Close the connection without an answer."""


# Platform replies are compact JSON; the media daemon's end with a newline.
def js(o): return json.dumps(o, separators=(",", ":"))
def ok(o, status=200, headers=(), ctype="application/json; charset=utf-8"): return Reply(status, js(o).encode(), ctype, headers)
def perr(status, code, message): return ok({"error": {"code": code, "message": message}}, status)
def bare(status, message): return ok({"error": message}, status)  # the platform's few unstructured errors
def integration(): return perr(502, "integration_failed", "local integration operation failed")
def mok(o, status=200): return Reply(status, (js(o) + "\n").encode())
# Two small synthetic gradients, a stand-in for each lens's still.
STILLS = [base64.b64decode(x) for x in (
    "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYxLjE5LjEwMQD/2wBDAAgYGBwYHCEhISEhISckJygoKCcnJycoKCgrKysz"
    "MzMrKysoKCsrMDAzMzc5NzQ0MzQ5OTw8PEhIRUVUVFdnZ3z/xABSAAEBAQEBAAAAAAAAAAAAAAABAAIGBQEBAQEBAQAAAAAA"
    "AAAAAAAAAAEDBAIQAQAAAAAAAAAAAAAAAAAAAAARAQAAAAAAAAAAAAAAAAAAAAD/wAARCAC0AUADASIAAhEAAxEA/9oADAMB"
    "AAIRAxEAPwDhggriQAAsoAggKgmQIAFIABBMikIAgAKgmQIQUQAFLKAIICoJkCygKggCCZFIAB1YTKOcgAUgAEEAQQFQABBA"
    "VBAEABSygCCCiCZFLKAIICoJkCABUEAQQFQABBAHVhBGCAAFlACygKgABAApAAIJkUgACAFUspkCAAIAFLKAqAAEAAWUBUAA"
    "IAFIAB1YQRzoJkUgAEEBUEAQAAsoCoIAgmVUsoAggKgmQLKAqCAIJkCABUEAQQFQCBJIHUBBGCAAFlAVBAEEyKWUAQQUQTIp"
    "ZQBBAVBAEABUEAQQFQABBAEEBUEgSSBJIHShMowIAFIABBMikAAQAqllAEAAIAFLKAqAAEAAQAKgmQIAFIABJIEkgSSBJIHR"
    "BBGKAApZQBBAEEyqllAEEBUEyBAAqCAIIAgAKggCCAqAQJJAkkCSQJJAkkD3wmUZFlAEEFEEyKQACCAqCAIACoIAggKgABZQ"
    "BBAVBIEkgSSBJIEkgSSBJIHuABGRACqWUAQAAhMillAVAACEyBAAqCZAhAVAAEkgSSBJIEkgSSBJIEkgSSB7ITIzLKAIICoJ"
    "kCABUEAQQBAAVBAEEBUEASSBJIEkgSSBJIEkgSSBJIEkgesABmggKWUAQAFQQAsoCoAAWUAIAFQSBJIEkgSSBJIEkgSSBJIE"
    "kgSSBJIHpggeACADJAoBAAEChlpkACAAIFAIBJIEkgSSBJIEkgSSBJIEkgSSBJIEkgf/2Q==",
    "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYxLjE5LjEwMQD/2wBDAAgYGBwYHCEhISEhISckJygoKCcnJycoKCgrKysz"
    "MzMrKysoKCsrMDAzMzc5NzQ0MzQ5OTw8PEhIRUVUVFdnZ3z/xABQAAEBAQEBAAAAAAAAAAAAAAAAAgEDBwEBAQEAAAAAAAAA"
    "AAAAAAAAAAEDEAEAAAAAAAAAAAAAAAAAAAAAEQEAAAAAAAAAAAAAAAAAAAAA/8AAEQgAtAFAAwEiAAIRAAMRAP/aAAwDAQAC"
    "EQMRAD8A8/EDFsoQAsQCLEAKEALEALEMBYgBYhgOg5gLEALEMUdGIAWIBFjmAsQAsQAoSwFiAFiAFiAFiAFiAFCAFiAFsSAo"
    "QAsQAtiQFCBRYgEWxIChACxAC2JAUJYCxAC2JAUIAWIAdGJAUIAWIAWxLAWIAWIAWxLAWIFFiGCOjEALEALHMBYgBYhgOg5g"
    "LEALEMBYgBYgBbEsB0EALEAKEsBYgEWIAUJYCxAosQAoSwFiAFiAFCWAsQAsQAoSwRYgBYgBQgUWIAWIYDoIAWIAWOYCxACx"
    "DAdGIAWIAWIYDoxACxACxzAWIBFiAFCAFiBRYgBQgBYgBbEgKEAOggBbEgKEALEALYkBQgBYgBbEgihACxAC2JAUIAWIAWxI"
    "ooQAsQAtiWAsQAsQCLEMB0EALEALYlgLEALEMB0YgBYhgOg5gLEALEMUdBzAWIAWIAUIAWIAWxIIoSAoSA0YA0YAoSA0YA0Y"
    "A0YA1gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAA//Z")]
def merr(status, message): return mok({"error": message}, status)
def json_type(t): return t.lower()[:16] == "application/json" and t[16:17] in ("", ";")
def clamp(v, lo=-100.0, hi=100.0): return max(lo, min(hi, v))
def hexid(): return secrets.token_hex(16)


def jfield(body, key, cap=256):
    """The platform's json_field(): the first "key": "string" pair, no escapes, shorter than cap."""
    i = body.find(b'"%s"' % key.encode()) if body else -1
    if i < 0:
        return None
    p = i + len(key) + 2
    while body[p:p + 1] in (b" ", b"\t"):
        p += 1
    if body[p:p + 1] != b":":
        return None
    p += 1
    while body[p:p + 1] in (b" ", b"\t"):
        p += 1
    e = body.find(b'"', p + 1)
    if body[p:p + 1] != b'"' or e < 0:
        return None
    v = body[p + 1:e]
    return None if b"\\" in v or b"\0" in v or len(v) + 1 > cap else v.decode("utf-8", "replace")


def jint(o, key):
    """The media daemon's jipc_json_integer(): absent is 0, booleans count, integral numbers only."""
    v = o.get(key, 0)
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)) and math.isfinite(v) and v == int(v) and -2**31 <= v < 2**31:
        return int(v)
    raise ValueError(key)


def valid_label(x):
    if x is not None and len(x) > 6 and x[-6:].lower() == ".local":
        x = x[:-6]
    ok_ = x is not None and re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", x)
    return x.lower() if ok_ else None


def private_routes(text):
    """RFC1918 IPv4 (prefix 8..32) or ULA IPv6 CIDRs, comma/space separated; None if any is not."""
    out = []
    for item in filter(None, re.split("[, ]+", text or "")):
        addr, slash, pfx = item.partition("/")
        prefix = int(re.match(r"\d*", pfx).group() or 0)  # strtoul
        try:
            a = socket.inet_pton(socket.AF_INET, addr)
            good = 8 <= prefix <= 32 and (a[0] == 10 or (a[0] == 172 and a[1] & 0xF0 == 16) or a[:2] == b"\xc0\xa8")
        except OSError:
            try:
                good = 7 <= prefix <= 128 and socket.inet_pton(socket.AF_INET6, addr)[0] & 0xFE == 0xFC
            except OSError:
                good = False
        if not slash or not good:
            return None
        out.append("%s/%d" % (addr, prefix))
    return out or None  # the daemon refuses an empty list too


def ntp_server_ok(text):
    if text in ("", "gateway"):
        return True
    host, colon, port = text.partition(":")
    if colon and not (re.fullmatch(r"\d+", port) and 0 < int(port) <= 65535):
        return False
    try:
        return socket.inet_pton(socket.AF_INET, host) != b"\0\0\0\0"
    except OSError:
        return False


def preset_name_ok(name):
    try:
        n = len(name.encode("utf-8"))
    except UnicodeEncodeError:  # lone surrogates
        return False
    return 1 <= n <= 64 and all(ord(c) >= 32 and ord(c) not in (127, 0xFFFE, 0xFFFF) for c in name)


def validate_config(c, password):
    """The checks of jipc_config_validate() that public settings can reach."""
    for v in (c["username"], password):
        if any(ord(ch) < 32 or ord(ch) == 127 for ch in v) or v != v.strip():
            return "string settings require printable values without leading or trailing whitespace"
    if not c["username"] or ":" in c["username"] or " " in c["username"] or not password:
        return "set a nonempty printable username/password in the private configuration"
    if not (1 <= c["sensors"] <= 2 and 1 <= c["max_clients"] <= 8 and 1 <= c["fps"] <= 30 and 64 <= c["bitrate"] <= 8192):
        return "invalid sensors, clients, frame rate or bitrate"
    if c["flip0"] > 3 or c["flip1"] > 3:
        return "sensor flips require bitmasks 0..3"
    if c["main_only"] and c["sensors"] != 2:
        return "main_only requires two sensors"
    w, h, sw, sh, w1, h1 = (c[k] for k in ("width", "height", "sub_width", "sub_height", "width1", "height1"))
    if not (160 <= w <= 4096 and 120 <= h <= 2160 and 160 <= sw <= w and 120 <= sh <= h) or (w | h | sw | sh) & 1:
        return "invalid even video dimensions"
    if (bool(w1) != bool(h1) or (w1 and (not (sw <= w1 <= 4096 and sh <= h1 <= 2160) or (w1 | h1) & 1))
            or any(b and not 64 <= b <= 8192 for b in (c["bitrate1"], c["sub_bitrate"])) or c["gop_frames"] > 300):
        return "invalid secondary sensor geometry, bitrate or GOP"
    if not (1 <= c["record_seconds"] <= 3600 and 1 <= c["record_max_mb"] <= 4096):
        return "invalid recording retention or segment duration"
    return None


# ---- camera controls (media/src/controls.c): kept across restarts, applied by an engine ----
NIGHT_MODES, IR_MODES, FLICKER = ("day", "night", "auto", "schedule"), ("night", "off"), ("off", "50", "60")
PICTURE = ("brightness", "contrast", "saturation", "sharpness", "denoise")  # no hue: the ISP driver rejects it
EV_READ = 5.0  # in automatic mode the daemon reads the ISP exposure every 5 s


SIREN_SOUNDS = ("alarm", "two_tone", "pulse", "chime")


def control_defaults():
    return {"night": {"mode": "day", "ir": "night", "night_ev": 30000, "day_ev": 5000, "wait_s": 60,
                      "from": "18:30", "until": "06:00"},
            "white_light": {"on": False, "trigger_s": 120,
                            "schedule": {"enabled": False, "from": "19:00", "until": "23:00"}},
            "pilot_led": False, "mic_mute": False, "anti_flicker": "off", "utc_offset_min": 0,
            "picture": [dict.fromkeys(PICTURE, 128) for _ in range(2)],
            "patrol": {"enabled": False, "pause_s": 120, "stops": []},
            "siren": {"sound": "alarm", "volume": 100, "seconds": 30, "strobe": False},
            "overlay": {"enabled": False, "position": "top-left", "format": "ymd", "name": ""},
            "privacy": {"on": False, "slot": 0, "schedule": {"enabled": False, "from": "22:00", "until": "06:00"}}}


def hhmm(text):
    """parse_hhmm(): minutes after midnight, or None."""
    m = re.fullmatch(r"(\d{1,2}):(\d{1,2})", text) if isinstance(text, str) else None
    return int(m[1]) * 60 + int(m[2]) if m and int(m[1]) < 24 and int(m[2]) < 60 else None


def in_window(now, start, end):
    """Inside [start, end), which may wrap past midnight; an empty window never matches."""
    return start != end and (start <= now < end if start < end else now >= start or now < end)


class Invalid(Exception):
    """A refused controls patch: 400 with the daemon's message."""


def controls_patch(current, o):
    """jipc_controls_patch()'s checks, in its order: the patched copy, or Invalid (nothing changes)."""
    if not isinstance(o, dict):
        raise Invalid("the request must be a JSON object")
    nxt = json.loads(json.dumps(current))

    def whole(src, dst, key, lo, hi):
        if key in src:
            v = src[key]
            if isinstance(v, bool) or not isinstance(v, (int, float)) or v != int(v) or not lo <= v <= hi:
                raise Invalid("%s must be a whole number from %d to %d" % (key, lo, hi))
            dst[key] = int(v)

    def flag(src, dst, key):
        if key in src:
            if not isinstance(src[key], bool): raise Invalid("%s must be true or false" % key)
            dst[key] = src[key]

    def clock(src, dst, key):
        if key in src:
            m = hhmm(src[key])
            if m is None: raise Invalid("%s must be a time like 18:30" % key)
            dst[key] = "%02d:%02d" % divmod(m, 60)

    def choice(src, dst, key, names):
        if key in src:
            if not isinstance(src[key], str) or src[key] not in names: raise Invalid("%s has an unknown value" % key)
            dst[key] = src[key]

    if "night" in o:
        n, d = o["night"], nxt["night"]
        if not isinstance(n, dict): raise Invalid("night must be an object")
        choice(n, d, "mode", NIGHT_MODES)
        choice(n, d, "ir", IR_MODES)
        whole(n, d, "night_ev", 1, 10000000)
        whole(n, d, "day_ev", 1, 10000000)
        whole(n, d, "wait_s", 1, 3600)
        clock(n, d, "from")
        clock(n, d, "until")
        if d["day_ev"] >= d["night_ev"]: raise Invalid("the day level must be below the night level")
    if "white_light" in o:
        w, d = o["white_light"], nxt["white_light"]
        if not isinstance(w, dict): raise Invalid("white_light must be an object")
        flag(w, d, "on")
        whole(w, d, "trigger_s", 5, 3600)
        if "schedule" in w:
            s = w["schedule"]
            if not isinstance(s, dict): raise Invalid("schedule must be an object")
            flag(s, d["schedule"], "enabled")
            clock(s, d["schedule"], "from")
            clock(s, d["schedule"], "until")
    if "patrol" in o:
        t, d = o["patrol"], nxt["patrol"]
        if not isinstance(t, dict): raise Invalid("patrol must be an object")
        flag(t, d, "enabled")
        whole(t, d, "pause_s", 0, 3600)
        if "stops" in t:
            if not isinstance(t["stops"], list) or len(t["stops"]) > 16: raise Invalid("stops must be a list of at most 16")
            stops = []
            for s in t["stops"]:
                if not isinstance(s, dict) or "slot" not in s: raise Invalid("each stop needs a slot")
                q = {"slot": 1, "dwell_s": 10}
                whole(s, q, "slot", 1, 16)
                whole(s, q, "dwell_s", 3, 3600)
                stops.append(q)
            d["stops"] = stops
    if "siren" in o:
        v, d = o["siren"], nxt["siren"]
        if not isinstance(v, dict): raise Invalid("siren must be an object")
        if "sound" in v:
            if v["sound"] not in SIREN_SOUNDS: raise Invalid("sound must be alarm, two_tone, pulse or chime")
            d["sound"] = v["sound"]
        whole(v, d, "volume", 1, 100)
        whole(v, d, "seconds", 1, 300)
        flag(v, d, "strobe")
    if "overlay" in o:
        v, d = o["overlay"], nxt["overlay"]
        if not isinstance(v, dict): raise Invalid("overlay must be an object")
        flag(v, d, "enabled")
        if "position" in v:
            if v["position"] not in ("top-left", "top-right", "bottom-left", "bottom-right"):
                raise Invalid("position must be top-left, top-right, bottom-left or bottom-right")
            d["position"] = v["position"]
        if "format" in v:
            if v["format"] not in ("ymd", "dmy", "mdy"): raise Invalid("format must be ymd, dmy or mdy")
            d["format"] = v["format"]
        if "name" in v:
            if not isinstance(v["name"], str) or len(v["name"]) > 16 or any(not " " <= c <= "~" for c in v["name"]):
                raise Invalid("name must be plain text of at most 16 characters")
            d["name"] = v["name"]
    if "privacy" in o:
        v, d = o["privacy"], nxt["privacy"]
        if not isinstance(v, dict): raise Invalid("privacy must be an object")
        flag(v, d, "on")
        whole(v, d, "slot", 0, 16)
        if "schedule" in v:
            s = v["schedule"]
            if not isinstance(s, dict): raise Invalid("schedule must be an object")
            flag(s, d["schedule"], "enabled")
            clock(s, d["schedule"], "from")
            clock(s, d["schedule"], "until")
    flag(o, nxt, "pilot_led")
    flag(o, nxt, "mic_mute")
    choice(o, nxt, "anti_flicker", FLICKER)
    whole(o, nxt, "utc_offset_min", -720, 840)
    if "picture" in o:
        pics = o["picture"]
        if not isinstance(pics, list) or len(pics) > 2: raise Invalid("picture must be a list of at most two lenses")
        for i, p in enumerate(pics):
            if p is None: continue
            if not isinstance(p, dict): raise Invalid("picture entries must be objects")
            for k in PICTURE:
                whole(p, nxt["picture"][i], k, 0, 255)
    return nxt


class Mock:
    """The camera's state.  Every request runs under `lock`; time-driven state
    (motion, the speaker queue, deadlines) is brought up to date by tick()."""

    def __init__(self, args):
        now = time.time()
        self.args, self.lock, self.relays = args, threading.Lock(), 0
        self.fail, self.down, self.reboot_at, self.restart_at = args.fail, (0.0, 0.0), None, None
        # platform daemon
        self.password, self.sessions, self.buckets = args.password, {}, {}  # token -> [csrf, seen, warning]
        self.version, self.sequence, self.release = "0.2.26", 60, None
        self.hostname = self.hostname_next = "camera1"
        self.routes, self.clock_offset, self.ssh_keys = ["10.42.0.0/24"], 0.0, ""
        self.ip = {"mode": "dhcp", "address": "192.168.1.38", "prefix": 24, "gateway": "192.168.1.1"}
        self.manual_at, self.privacy_until, self.privacy_override, self.homed_at = 0.0, 0.0, False, None
        self.ntp = {"server": "gateway", "resolved": "192.168.1.1", "attempts": 1, "offset": 760454.303,
                    "synced": now - 2637, "due": None}
        self.ssid, self.trial, self.staged = "HomeNet-IoT", None, None
        self.wifi_last = None  # how the last Wi-Fi trial ended: "committed" or "rolled_back"
        self.remembered = {}  # "stay signed in": token -> [csrf, until, warning]; survives restarts
        self.restart_time = None  # the daily restart, "HH:MM" in the camera's time zone
        self.zone = None if args.no_camera_zone else time.localtime().tm_gmtoff // 60  # GmtTz, in minutes
        self.system_boot = now - 9000
        self.q0, self.qbase = now, (43163730, 1260796158, 649133, 1093779)
        # media daemon
        self.config, self.media_password, self.recording = dict(MEDIA_CONFIG), args.password, False
        self.presets = {} if args.no_presets else {k: list(v) for k, v in PRESETS.items()}
        self.aim_ok = not args.no_aim
        self.staged = None
        self.x, self.y = -14.0, 70.0
        self.controls = control_defaults()  # the daemon's state_dir/controls.json
        self.media_boot(now, args.homing, first=True)

    # ---- time ----
    def media_boot(self, t, homing, first=False):
        """(Re)start of the media daemon at t: runtime levels and lights reset, presets and position persist."""
        self.started = t - (2709.852889 if first and not homing else 0)
        self.homing_until, self.restart_at, self.motion, self.goto = t + homing, None, None, None
        self.siren = None
        self.ctl = {"night": -1, "white_light": -1, "pilot_led": int(self.controls["pilot_led"]), "speaker_enable": 0}
        self.muted = self.controls["mic_mute"]
        # the controls engine's memory: what it applied, the exposure, timers
        self.ce = {"night": -1, "white": -1, "ev": None, "ev_at": 0.0, "cond": 0.0, "switched": 0.0,
                   "trigger_until": 0.0, "override": 0, "override_until": 0.0, "reason": ""}
        self.mic = {"volume": 80, "gain": 22, "speaker_gain": 25}
        self.spk = {"volume": 60, "sessions": 3 if first else 0, "played": 7280.0 if first else 0.0,
                    "dropped": 340 if first else 0, "refused": 0, "queued": 0.0, "t": t, "session": False,
                    "empty_at": t}

    def reboot(self, start, seconds):
        self.down, self.reboot_at = (start, start + seconds), start

    def rebooted(self):
        end, self.reboot_at = self.down[1], None
        self.sessions.clear()  # they live in the platform daemon's memory (remembered ones come back)
        if self.trial:  # a reboot during a trial recovers the old network
            self.wifi_last = "rolled_back"
        self.staged = self.trial = None
        self.system_boot = end
        if self.release:
            (self.version, self.sequence), self.release = self.release, None
        self.hostname = self.hostname_next
        self.q0, self.qbase = end, (0, 0, 0, 0)
        if self.ntp["server"]:
            self.ntp.update(resolved="", attempts=0, synced=None, due=end + 2)
        self.media_boot(end, self.args.homing)

    def schedule_restart(self, now):
        """A video/audio setting changed: the media daemon restarts all its services."""
        out = self.args.restart_outage
        self.restart_at = now + (0.3 if out else 3.0)
        if out and self.reboot_at is None:
            self.down = (self.restart_at, self.restart_at + out)

    def unreachable(self, now):
        return self.fail or self.down[0] <= now < self.down[1]

    def homing(self, now):
        return now < self.homing_until

    def tick(self, now):
        self.ptz_advance(now)
        self.speaker_advance(now)
        t = self.trial
        if t and now >= t["deadline"]:  # never committed: the helper rolls back and reboots
            self.trial, self.wifi_last = None, "rolled_back"
            self.reboot(t["deadline"], 20)
        if self.reboot_at is not None and now >= self.reboot_at:
            self.rebooted()
        if (self.restart_time and self.reboot_at is None and not self.trial and now - self.system_boot >= 3600
                and int((now // 60 + (self.zone or 0)) % 1440) == hhmm(self.restart_time)):
            self.reboot(now, self.args.restart_seconds)  # jl_scheduled_restart: UTC without a camera zone
        if self.restart_at is not None and now >= self.restart_at:
            self.media_boot(self.restart_at + self.args.restart_outage, 0)
        self.controls_tick(now)
        n = self.ntp
        if n["due"] and now >= n["due"]:
            host = n["server"].partition(":")[0]
            n.update(resolved="192.168.1.1" if host == "gateway" else host, attempts=n["attempts"] + 1,
                     offset=round(random.uniform(-40, 40), 3), synced=n["due"], due=None)

    # ---- controls engine (media/src/controls.c engine()) ----
    def exposure(self, now):
        """The pan/tilt lens's ISP exposure value (higher is darker): drifts across both default
        levels every 5 minutes, so automatic switching and its wait are visible."""
        return max(1, int(22500 + 16000 * math.sin(2 * math.pi * now / 300) + random.uniform(-300, 300)))

    def local_minutes(self, now):
        return int((now // 60 + self.controls["utc_offset_min"]) % 1440)

    def next_boundary(self, now, start, end):
        """Seconds until the next start/end edge of a daily window."""
        today = (now + self.controls["utc_offset_min"] * 60) % 86400
        return min((edge * 60 - today) % 86400 or 86400 for edge in (hhmm(start), hhmm(end)))

    def controls_tick(self, now):
        c, e, n = self.controls, self.ce, self.controls["night"]
        if self.zone is not None:  # the camera's time zone wins over utc_offset_min
            c["utc_offset_min"] = self.zone
        if n["mode"] == "auto" and now - e["ev_at"] >= EV_READ:
            e["ev_at"], e["ev"] = now, self.exposure(now)
        night = e["night"]
        if n["mode"] in ("day", "night"):
            night, e["reason"] = int(n["mode"] == "night"), "%s (chosen)" % n["mode"]
        elif n["mode"] == "schedule":
            night, e["reason"] = int(in_window(self.local_minutes(now), hhmm(n["from"]), hhmm(n["until"]))), "schedule"
        elif e["ev"] is None:
            night, e["reason"] = max(night, 0), "no exposure reading; holding"
        elif night < 0:
            night = int(e["ev"] >= n["night_ev"])
            e["reason"] = "auto: started %s, level %d" % ("dark" if night else "light", e["ev"])
        else:
            want = e["ev"] <= n["day_ev"] if night else e["ev"] >= n["night_ev"]
            e["cond"] = (e["cond"] or now) if want else 0.0
            e["reason"] = ("auto: night; day at level %d or below" % n["day_ev"] if night
                           else "auto: day; night at level %d or above" % n["night_ev"])
            # the hold stops flapping when a switch changes the reading itself
            if want and now - e["cond"] >= n["wait_s"] and (not e["switched"] or now - e["switched"] >= self.args.auto_hold):
                night, e["cond"] = 1 - night, 0.0
                e["reason"] = "auto: level %d %s %d for %d s" % (e["ev"], ">=" if night else "<=",
                                                                 n["night_ev"] if night else n["day_ev"], n["wait_s"])
        # white light: a trigger, then a manual change until the next schedule edge, the schedule, the manual state
        w = c["white_light"]
        if e["trigger_until"] and now < e["trigger_until"]:
            white = 1
        else:
            e["trigger_until"] = 0.0
            if w["schedule"]["enabled"] and e["override_until"] and now < e["override_until"]:
                white = e["override"]
            elif w["schedule"]["enabled"]:
                e["override_until"] = 0.0
                white = int(in_window(self.local_minutes(now), hhmm(w["schedule"]["from"]), hhmm(w["schedule"]["until"])))
            else:
                white = int(w["on"])
        if night != e["night"]:
            e["night"], e["switched"] = night, now
        e["white"] = white
        self.ctl["night"], self.ctl["white_light"] = night, white  # what the platform daemon was told

    def controls_set(self, o, now):
        """POST /api/v1/controls (and the controls the legacy operations patch)."""
        before = self.controls
        self.controls, e = controls_patch(before, o), self.ce
        v = o.get("privacy") if isinstance(o.get("privacy"), dict) else {}
        if "on" in v and self.controls["privacy"]["schedule"]["enabled"]:  # holds until the schedule's next edge
            self.privacy_override, self.privacy_until = v["on"], now + 3600
            self.controls["privacy"]["on"] = before["privacy"]["on"]
        elif "on" in v or "schedule" in v:
            self.privacy_until = 0.0
        b, a = before["night"], self.controls["night"]
        if isinstance(o.get("white_light"), dict) and "on" in o["white_light"]:
            e["override_until"] = 0.0
        if any(b[k] != a[k] for k in ("mode", "night_ev", "day_ev")):
            e["cond"] = 0.0
        if b["ir"] != a["ir"]:
            e["night"] = -1  # re-applied with the new infrared mode
        self.muted = self.controls["mic_mute"]
        self.ctl["pilot_led"] = int(self.controls["pilot_led"])

    def controls_note(self, op, v, v2, now):
        """jipc_controls_note(): a successful manual control is remembered too."""
        c, e = self.controls, self.ce
        if op in ("night", "ircut") and v in (0, 1):
            c["night"]["mode"], e["night"] = NIGHT_MODES[v], v
        elif op == "white_light":
            s = c["white_light"]["schedule"]
            if s["enabled"]:
                e["override"], e["override_until"] = v, now + self.next_boundary(now, s["from"], s["until"])
            else:
                c["white_light"]["on"] = bool(v)
            e["trigger_until"], e["white"] = 0.0, v
        elif op in ("pilot_led", "mic_mute"):
            c[op] = bool(v)
        elif op == "anti_flicker":
            c[op] = FLICKER[v]
        elif op in PICTURE and v2 in (0, 1):
            c["picture"][v2][op] = v

    def controls_json(self, now):
        c, e = self.controls, self.ce
        auto = c["night"]["mode"] == "auto"
        return {**json.loads(json.dumps(c)), "live": {
            "night": "unknown" if e["night"] < 0 else ("day", "night")[e["night"]], "reason": e["reason"],
            "level": e["ev"] if auto else None, "since_s": int(now - e["switched"]) if e["switched"] else 0,
            "pending_s": int(now - e["cond"]) if e["cond"] else 0,
            "hold_left_s": int(self.args.auto_hold - (now - e["switched"]))
            if auto and e["switched"] and now - e["switched"] < self.args.auto_hold else 0,
            "white_light": e["white"] == 1,
            "trigger_left_s": math.ceil(e["trigger_until"] - now) if e["trigger_until"] > now else 0,
            "local_minutes": self.local_minutes(now), "zone": "browser" if self.zone is None else "camera",
            "privacy": self.privacy_active(now), "patrol": self.patrol_live(now), "error": ""}}

    def privacy_active(self, now):
        v = self.controls["privacy"]
        if self.privacy_until > now: return self.privacy_override
        s = v["schedule"]
        if s["enabled"]: return in_window(self.local_minutes(now), hhmm(s["from"]), hhmm(s["until"]))
        return v["on"]

    def patrol_live(self, now):
        """patrol_tick()'s report; each stop takes its dwell plus 3 s of moving."""
        t = self.controls["patrol"]
        if not t["enabled"] or not t["stops"]: return {"state": "off", "slot": 0, "left_s": 0}
        if self.privacy_active(now): return {"state": "privacy", "slot": t["stops"][0]["slot"], "left_s": 0}
        if self.homing(now): return {"state": "homing", "slot": t["stops"][0]["slot"], "left_s": 0}
        if t["pause_s"] and now - self.manual_at < t["pause_s"]:
            return {"state": "paused", "slot": t["stops"][0]["slot"], "left_s": math.ceil(t["pause_s"] - (now - self.manual_at))}
        at = (now - self.manual_at) % sum(s["dwell_s"] + 3 for s in t["stops"])
        for s in t["stops"]:
            if s["slot"] not in self.presets:
                continue
            if at < 3: return {"state": "moving", "slot": s["slot"], "left_s": 0}
            if at < 3 + s["dwell_s"]: return {"state": "dwell", "slot": s["slot"], "left_s": math.ceil(3 + s["dwell_s"] - at)}
            at -= 3 + s["dwell_s"]
        return {"state": "no_stops", "slot": 0, "left_s": 0}

    def ptz_advance(self, now):
        m, g = self.motion, self.goto
        if m:
            dt = min(now, m["until"]) - m["t"]
            if dt > 0:
                self.x, self.y, m["t"] = clamp(self.x + m["ux"] * dt), clamp(self.y + m["uy"] * dt), m["t"] + dt
            if now >= m["until"]:
                self.motion = None
        if g:
            f = min(1.0, (now - g["t0"]) / g["dur"])
            self.x, self.y = g["x0"] + (g["x1"] - g["x0"]) * f, g["y0"] + (g["y1"] - g["y0"]) * f
            if f >= 1:
                self.goto = None

    def speaker_advance(self, now):
        s, q = self.spk, self.spk["queued"]
        if q:
            drained = min(q, max(0.0, now - s["t"]) * 8000)  # 8 kHz A-law: one byte per 1/8 ms
            s["played"] += drained / 8
            s["queued"] = q - drained
            if s["queued"] < 0.5:
                s["queued"], s["empty_at"] = 0.0, s["t"] + q / 8000
        s["t"] = now
        if s["session"] and not s["queued"] and now - s["empty_at"] >= 0.5:  # 500 ms of quiet ends it
            s["session"] = False

    # ---- sessions ----
    def authorize(self, h, now, platform):
        """The live joan_session among the request's cookies (refreshed), plus CSRF on changes."""
        token = s = None
        for candidate in h.joan_cookies():
            found = self.sessions.get(candidate)
            if found and now - found[1] <= self.args.idle:
                token, s = candidate, found
                s[1] = now
                break
            self.sessions.pop(candidate, None)
        if not s:  # not in memory (idle or restarted): a remembered sign-in comes back as a fresh session
            for candidate in h.joan_cookies():
                found = self.remembered.get(candidate)
                if found and found[1] >= now:
                    token, s = candidate, [found[0], now, found[2]]
                    self.sessions[token] = s
                    break
        if not s:
            raise perr(401, "authentication_required", "authentication required") if platform else \
                merr(401, "Authentication required")
        given = h.headers.get("X-CSRF-Token", "").encode("utf-8", "replace")
        if h.command not in ("GET", "HEAD") and (not secrets.compare_digest(given, s[0].encode())
                                                 or (not platform and h.cross_origin())):
            raise perr(403, "csrf_rejected", "csrf token rejected") if platform else merr(403, "CSRF token required")
        return token, s

    def login(self, h, now):
        user, password = jfield(h.body, "username"), jfield(h.body, "password")
        if user is None or password is None:
            return perr(400, "invalid_json", "username and password are required")
        b = self.buckets.setdefault(h.client_address[0], [now, 0])
        if b[0] + 60 < now:
            b[:] = [now, 0]
        if b[1] >= 5:
            return perr(429, "rate_limited", "try later")
        if user != "admin" or password != self.password:
            b[1] += 1
            return perr(401, "invalid_credentials", "invalid credentials")
        b[1] = 0
        if len(self.sessions) >= 16:
            del self.sessions[min(self.sessions, key=lambda k: self.sessions[k][1])]
        token, csrf, warning = secrets.token_hex(32), secrets.token_hex(32), self.password == "admin"
        self.sessions[token] = [csrf, now, warning]
        remember = re.search(rb'"remember"[ \t]*:[ \t]*true', h.body or b"") is not None  # at most 8 are kept
        if remember:
            if len(self.remembered) >= 8:
                del self.remembered[min(self.remembered, key=lambda k: self.remembered[k][1])]
            self.remembered[token] = [csrf, now + REMEMBER_SECONDS, warning]
        return ok({"ok": True, "csrf": csrf, "default_password_warning": warning, "remember": remember},
                  ctype="application/json",
                  headers=[("Set-Cookie", "joan_session=%s; Path=/; HttpOnly; SameSite=Strict%s"
                            % (token, "; Max-Age=%d" % REMEMBER_SECONDS if remember else ""))])

    def pause(self, seconds):
        """Let other requests run while a slow helper (Wi-Fi join, package verify) works."""
        self.lock.release()
        try:
            time.sleep(seconds)
        finally:
            self.lock.acquire()

    # ---- routing ----
    def backdoor(self, target):
        u = urlsplit(target)
        if u.path != "/__mock/fail":
            return mok({"error": "use /__mock/fail?on=1 or ?on=0"}, 404)
        with self.lock:
            q = parse_qs(u.query)
            if "on" in q:
                self.fail = q["on"][0] not in ("0", "false", "off")
            return mok({"ok": True, "fail": self.fail})

    def dispatch(self, h, now):
        return self.relay(h, now) if h.path.startswith("/api/joan/") else self.media(h, now)

    def relay(self, h, now):
        """The media daemon's /api/joan/<rest> relay, then the platform daemon's /api/<rest>."""
        rest, m, ct = h.path[10:], h.command, h.headers.get("Content-Type", "")
        upload = m == "POST" and rest == "v1/update"
        if m not in ("GET", "POST", "PUT", "DELETE"): return merr(405, "Use GET, POST, PUT or DELETE")
        if not rest or ".." in rest or "//" in rest: return merr(404, "Endpoint not found")
        if h.cross_origin(): return merr(403, "Cross-origin requests are not accepted")
        if upload and ct.lower() != "application/octet-stream":
            return merr(415, "Upload the package as application/octet-stream")
        if not upload and h.body and not json_type(ct) and not ct.lower().startswith("text/plain"):
            return merr(415, "Use application/json or text/plain")
        return self.platform(h, rest.partition("?")[0], now)

    def platform(self, h, rest, now):
        m, body = h.command, h.body
        if rest == "v1/session":
            name = {"POST": "login", "DELETE": "logout"}.get(m, "session-info")
        elif rest == "v1/network/wifi":
            name = {"POST": "wifi/stage", "PUT": "wifi/commit", "GET": "wifi/state"}.get(m, "wifi/rollback")
        elif rest == "v1/update":
            if m == "GET":  # --update-status 500,failed answers those in turn, then the last one
                self.authorize(h, now, True)  # as the real route does: after a restart this answers 401
                sequence = self.args.update_status.split(",") if self.args.update_status else ["idle"]
                self.update_polls = getattr(self, "update_polls", -1) + 1
                said = sequence[min(self.update_polls, len(sequence) - 1)]
                return bare(int(said), "unavailable") if said.isdigit() else ok({"state": said, "reason": "", "restart": ""})
            name = "firmware/stage" if m == "POST" else "firmware/apply"
        else:
            name = V1.get(rest, rest)
        if name == "v1/setup/status":
            return ok({"state": "ready", "setup_required": False, "default_password_warning": self.password == "admin",
                       "temporary_user": "admin", "release_version": self.version, "release_sequence": self.sequence})
        if name == "health":
            return ok({"ok": True})
        if name == "login" and m == "POST":
            return self.login(h, now)
        token, s = self.authorize(h, now, True)
        if name == "status":
            return ok({"version": self.version, "state": "ready", "local_only": True,
                       "active_routes": {"cidrs": self.routes}, "default_password_warning": s[2],
                       "ssh_password_sync": not self.args.ssh_unsynced, "rtsp_password_sync": False,
                       "features": {"ssh": "password-synchronized", "mdns": "running", "media": "external"},
                       "ntp": self.ntp_status(now), "mqtt_bridge": "listening:127.0.0.2:1883",
                       "https_cloud_bridge": {"supported": False, "blocker": "OEM trust/pinning contract not recovered"}})
        if name == "session-info":
            return ok({"authenticated": True, "csrf": s[0], "default_password_warning": s[2]})
        if name == "logout" and m in ("POST", "DELETE"):
            self.sessions.pop(token, None)
            self.remembered.pop(token, None)
            return Reply(204, headers=[("Set-Cookie", "joan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict")])
        if name == "time" and m == "GET":
            return ok({"epoch": int(now + self.clock_offset), "ntp": self.ntp_status(now)})
        if name == "time" and m == "PUT":
            return self.set_time(body, now)
        if name == "timezone" and m == "PUT":
            tz = jfield(body, "gmt_tz", 16)
            t = tz and re.fullmatch(r"GMT[+-](\d\d):(\d\d)", tz)
            if tz is None: return perr(400, "invalid_json", "gmt_tz required")
            if not t or int(t[1]) > 14 or int(t[2]) > 59: return perr(400, "invalid_timezone", "bad gmt_tz")
            self.zone = (-1 if tz[3] == "-" else 1) * (int(t[1]) * 60 + int(t[2]))  # schedules follow it
            return ok({"ok": True, "gmt_tz": tz})
        if name == "password" and m == "POST":
            old, new = jfield(body, "old_password"), jfield(body, "new_password")
            if old is None or new is None:
                return perr(400, "invalid_json", "old_password and new_password are required")
            raw = new.encode()
            if not raw or len(raw) > 128 or any(c < 0x20 or c == 0x7F for c in raw) or old != self.password:
                return perr(400, "password_rejected", "password change was rejected")
            self.password = new
            self.sessions.clear()
            self.remembered.clear()  # a new password signs out everywhere
            return ok({"ok": True, "login_required": True, "rtsp_password_sync": "unavailable"})
        if name == "restart" and m == "POST":  # restart_blocker(): never during an update or a trial
            why = "update" if self.release else "wifi-trial" if self.trial else self.args.restart_blocked
            if why: return ok({"ok": False, "reason": why}, 409)
            self.reboot(now + 2, self.args.restart_seconds)
            return ok({"ok": True})
        if name == "reset" and m == "POST":  # jl_apply_reset() runs at the next boot; the same refusals as a restart
            confirm = jfield(body, "confirm", 64)
            if confirm is None:
                return perr(400, "invalid_json", "confirm is required: the camera's name")
            if confirm != self.hostname:
                return perr(400, "confirm_mismatch", "confirm must be the camera's name")
            why = "update" if self.release else "wifi-trial" if self.trial else self.args.restart_blocked
            if why: return ok({"ok": False, "reason": why}, 409)
            self.password = self.media_password = "admin"
            self.ssh_keys, self.restart_time, self.presets, self.controls = "", None, {}, control_defaults()
            self.remembered.clear()
            self.reboot(now + 2, self.args.restart_seconds)
            return ok({"ok": True})
        if name == "restart-schedule":
            if m == "PUT":
                when = jfield(body, "time", 8)
                if when is None:
                    return perr(400, "invalid_json", "time is required: HH:MM, or empty for no scheduled restart")
                if when and not (re.fullmatch(r"[0-2]\d:[0-5]\d", when) and int(when[:2]) <= 23):
                    return perr(400, "invalid_time", "time must be HH:MM (00:00 to 23:59)")
                self.restart_time = when or None
            if m in ("GET", "PUT"):
                return ok({"enabled": self.restart_time is not None, "time": self.restart_time})
        if name.startswith("wifi/"):
            return self.wifi(name, h, now)
        if name == "network-ip":
            return self.network_ip(h, now)
        if name == "mdns" and m == "GET":
            return ok({"hostname": self.hostname, "address": self.hostname + ".local", "status": "running"})
        if name == "mdns" and m in ("PUT", "POST"):
            label = valid_label(jfield(body, "hostname"))
            if not label: return perr(400, "invalid_hostname", "hostname must be a 1-63 character DNS label")
            self.hostname_next = label  # active after the next reboot
            return ok({"ok": True, "hostname": label, "address": label + ".local", "restart_required": True})
        if name == "routes" and m == "GET":
            return ok({"cidrs": self.routes})
        if name == "routes" and m == "PUT":
            routes = private_routes(jfield(body, "cidrs", 1024))
            if routes is None: return perr(400, "invalid_routes", "only RFC1918 and ULA CIDRs are allowed")
            self.routes = routes
            return ok({"ok": True, "id": hexid()})
        if name == "network-quality" and m == "GET":
            return ok(self.quality(now))
        if name == "ssh/keys":
            return self.ssh(h)
        if name == "firmware/stage" and m == "POST":
            if not body: return bare(400, "image required")
            self.pause(3)  # signature and package verification
            if body.startswith(b"BAD"): return integration()
            self.staged = hexid()
            return ok({"ok": True, "id": self.staged})
        if name == "firmware/apply" and m in ("POST", "PUT"):
            i = jfield(body, "id", 65)
            if i is None: return bare(400, "id required")
            if i != self.staged: return integration()
            self.staged, self.release = None, ("0.2.27", 61)
            if not self.args.update_status:  # with it, this install stops instead of restarting
                self.reboot(now + 0.5, 25)
            return ok({"ok": True, "id": i})
        return perr(404, "route_not_found", "route not found")

    def ntp_status(self, now):
        n = self.ntp
        if not n["server"]:
            return {"server": "", "synchronized": False}
        if n["synced"] is None:
            return {"server": n["server"], "resolved": n["resolved"], "synchronized": False,
                    "attempts": n["attempts"], "error": ""}
        return {"server": n["server"], "resolved": n["resolved"], "synchronized": True, "attempts": n["attempts"],
                "last_offset_ms": n["offset"], "last_sync_age_seconds": int(now - n["synced"]), "error": ""}

    def set_time(self, body, now):
        server = jfield(body, "ntp")
        if server is not None:
            if len(server) >= 64 or not ntp_server_ok(server):
                return perr(400, "invalid_ntp", "ntp must be gateway, an IPv4 address with optional :port, or empty")
            self.ntp.update(server=server, resolved="", attempts=0, synced=None, due=now + 2 if server else None)
            return ok(self.ntp_status(now))
        epoch = jfield(body, "epoch", 24)  # a JSON string of digits
        if epoch is None: return perr(400, "invalid_json", "epoch required")
        if not re.fullmatch(r"\s*[+-]?\d+", epoch) or not 1577836800 <= int(epoch) <= 4102444800:
            return perr(400, "invalid_time", "out of range")
        self.clock_offset = int(epoch) - now
        return ok({"ok": True, "epoch": int(epoch)})

    def wifi(self, name, h, now):
        m, body = h.command, h.body
        if name == "wifi/scan" and m == "GET":
            self.pause(3.5)  # a fresh scan, then the strongest entry per network; hidden ones are not listed
            current = self.trial["ssid"] if self.trial else self.ssid
            seen = {n[0] for n in SCAN}
            found = sorted(SCAN + ([] if current in seen else [(current, -60, 5180, True)]), key=lambda n: -n[1])
            return ok([{"ssid": n[0], "signal_dbm": n[1], "frequency_mhz": n[2], "secure": n[3],
                        "in_use": n[0] == current} for n in found])
        if name == "wifi/state" and m == "GET":
            t = self.trial
            if not t:
                return ok({"pending": False, "last": self.wifi_last})
            return ok({"pending": True, "seconds_left": max(0, int(t["deadline"] - now)), "ssid": t["ssid"],
                       "joined": t["joined"], "address": t["address"], "static": t.get("static")})
        if name == "wifi/stage" and m == "POST":
            if not body: return bare(400, "configuration required")
            try:  # wifi-apply.sh: printable ASCII without " or \, a password of 8 to 63 characters (no open networks)
                o = json.loads(body)
                ssid, psk = o["ssid"], o["password"]
                good = all(isinstance(v, str) and re.fullmatch(r'[ !#-\[\]-~]+', v) for v in (ssid, psk)) \
                    and len(ssid) <= 32 and 8 <= len(psk) <= 63
            except (ValueError, KeyError, TypeError, AttributeError):
                good = False
            if not good:
                self.pause(1)
                return integration()
            # The trial is durable before the camera touches the network, so any browser can already see it.
            self.wifi_last = None
            t = self.trial = {"id": hexid(), "ssid": ssid, "deadline": h.started + 90, "joined": False, "address": None}
            self.pause(1.5)  # scan and associate
            t["joined"] = True
            self.pause(1.5)  # DHCP on the new network
            t["address"] = TRIAL_ADDRESS
            now = time.time()
            if self.args.wifi_mode == "lost":  # the camera left for a network this client cannot reach
                self.down = (now, now + 15)
                raise Drop()
            return ok({"ok": True, "id": t["id"]})
        if (name == "wifi/commit" and m in ("POST", "PUT")) or (name == "wifi/rollback" and m in ("POST", "DELETE")):
            i, t = jfield(body, "id", 65), self.trial
            if i is None: return bare(400, "id required")  # present, as on the camera; its value is not checked
            if not t or now >= t["deadline"]: return integration()
            self.trial = None
            if name == "wifi/commit":
                self.ssid, self.wifi_last = t["ssid"], "committed"
                if "ip" in t: self.ip = t["ip"]
            else:
                self.wifi_last = "rolled_back"
                self.reboot(now + 0.5, 20)
            return ok({"ok": True, "id": i})
        return perr(404, "route_not_found", "route not found")

    def network_ip(self, h, now):
        m, body = h.command, h.body
        if m == "GET":
            return ok({"mode": self.ip["mode"], "address": self.ip["address"], "prefix": self.ip["prefix"], "gateway": self.ip["gateway"]})
        if m != "PUT": return perr(404, "route_not_found", "route not found")
        mode = jfield(body, "mode", 8)
        if mode is None: return perr(400, "invalid_json", "mode is required: dhcp or static")
        new = {"mode": "dhcp", "address": "192.168.1.38", "prefix": 24, "gateway": "192.168.1.1"}
        if mode == "static":  # static_ip_ok() in src/daemon/http.c
            a, p, g = jfield(body, "address", 16), jfield(body, "prefix", 3), jfield(body, "gateway", 16)
            try:
                net = ipaddress.ip_network(f"{a}/{int(p)}", strict=False)
                aa, gg = ipaddress.ip_address(a), ipaddress.ip_address(g)
                good = 8 <= int(p) <= 30 and aa.is_private and gg in net and aa != gg and \
                    not {aa, gg} & {net.network_address, net.broadcast_address}
            except (ValueError, TypeError):
                good = False
            if not good:
                return perr(400, "invalid_address", "a private address, a prefix from 8 to 30 and a gateway on the same subnet are required")
            new = {"mode": "static", "address": a, "prefix": int(p), "gateway": g}
        elif mode != "dhcp":
            return perr(400, "invalid_address", "a private address, a prefix from 8 to 30 and a gateway on the same subnet are required")
        why = "update" if self.release else "wifi-trial" if self.trial else self.args.restart_blocked
        if not why and mode == "static":
            why = {"192.168.1.60": "address-in-use"}.get(new["address"]) or {"192.168.1.254": "gateway-silent"}.get(new["gateway"])
        if why: return ok({"ok": False, "reason": why}, 409)
        self.wifi_last = None
        self.pause(1)
        self.trial = {"id": hexid(), "ssid": self.ssid, "deadline": now + 90, "joined": True, "address": new["address"],
                      "static": new["address"] if mode == "static" else None, "ip": new}
        return ok({"ok": True})

    def ssh(self, h):
        m, body = h.command, h.body
        if m == "GET":
            return ok({"ok": True, "authorized_keys": self.ssh_keys[:1800]})
        if m == "POST":
            if not body: return bare(400, "invalid key")
            line = body.decode("utf-8", "replace").rstrip("\r\n")
            if not line.startswith("ssh-ed25519 ") or "\n" in line or "\r" in line or len(line.split()) < 2:
                return integration()
            keys = self.ssh_keys.splitlines()
            if not any(k.split()[1] == line.split()[1] for k in keys):  # a key already present changes nothing
                if len(keys) >= 8: return integration()  # up to 8 keys
                self.ssh_keys += line + "\n"
            return ok({"ok": True, "id": hexid()})
        if m == "DELETE":
            i = jfield(body, "id", 65)
            if i is None: return bare(400, "id required")
            if i == "all":
                self.ssh_keys = ""
            elif re.fullmatch("[1-8]", i) and self.ssh_keys:  # the 1-based line of the key to remove
                self.ssh_keys = "".join(k + "\n" for n, k in enumerate(self.ssh_keys.splitlines(), 1) if n != int(i))
            else:
                return integration()
            return ok({"ok": True, "id": i})
        return perr(404, "route_not_found", "route not found")

    def quality(self, now):
        t, (rx, tx, rxp, txp) = now - self.q0, self.qbase
        dbm = round(-65 + 5 * math.sin(t / 23) + 2 * math.sin(t / 5.3))
        return {"wifi": {"associated": True, "ssid": self.trial["ssid"] if self.trial else self.ssid,
                         "signal_dbm": dbm, "rate_mbps": 72.2 if dbm > -62 else 52.0, "frequency_mhz": 5180,
                         "band": "5 GHz", "address": self.trial["address"] if self.trial and self.trial["address"] else self.ip["address"],
                         "gateway": self.ip["gateway"], "rx_bytes": str(rx + int(t * 16000)), "tx_bytes": str(tx + int(t * 420000)),
                         "rx_packets": str(rxp + int(t * 240)), "tx_packets": str(txp + int(t * 330))},
                "ethernet": {"link": False, "speed_mbps": None, "rx_bytes": "0", "tx_bytes": "168",
                             "rx_packets": "0", "tx_packets": "2"}}

    # ---- media daemon ----
    def media(self, h, now):
        t, m = h.path, h.command
        if t in ("/health", "/healthz") and m == "GET":
            return mok({"ok": True})
        self.authorize(h, now, False)
        if h.cross_origin(): return merr(403, "Cross-origin requests are not accepted")
        if m not in ("GET", "POST"): return merr(405, "Use GET or POST")
        if m == "POST" and not json_type(h.headers.get("Content-Type", "")) and t not in ("/api/v1/talk", "/api/v1/update/media"):
            return merr(415, "Content-Type application/json is required")
        if t == "/api/v1/status" and m == "GET":
            return mok(self.media_status(now))
        if t == "/api/v1/config/password" and m == "GET":
            # The admin UI's own signed-in fetch only (it sends this header; other sites cannot).
            if h.headers.get("X-Requested-With") != "jipc-ui" or h.cross_origin():
                return merr(403, "Sign in to the admin UI to see the stream password")
            return mok({"username": self.config["username"], "password": self.media_password})
        if t == "/api/v1/config":
            return mok(self.config) if m == "GET" else self.config_post(h.body, now)
        if t == "/api/v1/controls":
            if m == "POST":
                try:
                    self.controls_set(json.loads(h.body), now)
                except ValueError:
                    return merr(400, "the request must be a JSON object")
                except Invalid as e:
                    return merr(400, str(e))
            return mok(self.controls_json(now))
        if t == "/api/v1/control" and m == "POST":
            return self.control(h.body, now)
        if t == "/api/v1/recording" and m == "POST":
            try:
                enabled = jint(json.loads(h.body), "enabled") if b'"enabled"' in h.body else -1
            except (ValueError, AttributeError):
                enabled = -1
            if enabled not in (0, 1): return merr(400, "enabled must be 0 or 1")
            self.recording = bool(enabled)
            return mok({"ok": True})
        if t == "/api/v1/recordings" and m == "GET":
            return mok({"recordings": [], "truncated": False})
        if t == "/api/v1/update/media" and m == "GET":
            return mok({"state": "staged" if self.staged else "idle", "release": self.staged, "error": ""})
        if t == "/api/v1/update/media" and m == "POST":
            data = h.body or b""
            if not data.startswith(b"JIPCMEDIA2 "): return merr(400, "This is not a media package")
            self.staged = "cafe0123-8b374e7a"
            return mok({"release": self.staged})
        if t == "/api/v1/update/media/apply" and m == "POST":
            if not self.staged: return merr(409, "That package is no longer here: upload it again")
            self.staged = None
            return mok({"ok": True})
        if t == "/api/v1/aim":
            sx, sy = 2040 - self.x * 20.4, 615 - self.y * 6.15
            if m == "GET":
                if not self.aim_ok: return mok({"calibrated": False, "steps": [round(sx), round(sy)], "view": None, "x": [], "y": []})
                # camera1's calibration, without its small curved terms, is enough for a mock
                u, v = (sx - 2021) / -457, (sy - 385) / 220
                return mok({"calibrated": True, "steps": [round(sx), round(sy)], "x": [2021, -457, 0, 0, 0, 0], "y": [385, 0, 220, 0, 0, 0],
                            "view": {"u": (u + 1) / 2, "v": (v + 1) / 2, "w": 0.886, "h": 1.0}})
            o = json.loads(h.body or b"null")
            if not isinstance(o, dict) or not (0 <= o.get("u", -1) <= 1 and 0 <= o.get("v", -1) <= 1):
                return merr(400, "Send u and v, steps, or a calibration")
            if not self.aim_ok: return merr(409, "Aiming is not calibrated")
            if self.privacy_active(time.time()): return merr(409, "privacy mode is on")
            sx, sy = 2021 - 457 * (o["u"] * 2 - 1), 385 + 220 * (o["v"] * 2 - 1)
            now = time.time()
            self.goto = {"t0": now, "dur": 2.0, "x0": self.x, "y0": self.y, "x1": (2040 - sx) / 20.4, "y1": (615 - sy) / 6.15}
            return mok({"ok": True})
        if (t == "/api/v1/snapshot" or t.startswith("/api/v1/snapshot?channel=")) and m == "GET":
            ch = t.partition("=")[2] if "?" in t else "0"
            if not ch.isdigit() or int(ch) >= 4: return merr(400, "Invalid snapshot channel")
            if int(ch) >= self.config["sensors"] * 2: return merr(503, "invalid snapshot channel")
            if self.privacy_active(time.time()): return merr(503, "privacy mode is on")
            return Reply(200, STILLS[int(ch) // 2], "image/jpeg")
        if t == "/api/v1/talk" and m == "POST":
            return self.talk(h)
        return merr(404, "Endpoint not found")

    def media_status(self, now):
        c, up, homing, s = self.config, now - self.started, self.homing(now), self.spk
        streams = []
        for ch in range(c["sensors"] * 2):
            st = {"channel": ch}
            if ch % 2 and c["main_only"]:
                st["alias_of"] = ch - 1
            if ch % 2 == 0 or c["main_only"]:
                w, hh = (c["width"], c["height"]) if ch < 2 else (c["width1"] or c["width"], c["height1"] or c["height"])
            else:
                w, hh = c["sub_width"], c["sub_height"]
            st.update(frames=int(up * c["fps"]), width=w, height=hh, codec_ready=True)
            streams.append(st)
        if c["audio"]:
            streams.append({"channel": 4, "frames": int(up * 25), "width": 1920, "height": 1080, "codec_ready": True})
        platform = {"backend": "imp", "position_known": True, "position_x": round(self.x), "position_y": round(self.y),
                    "moving": bool(self.motion or self.goto or homing), "preset_motion": bool(self.goto),
                    "homing": homing, "homed": not homing, **self.ctl, "manual_timeout_ms": 1200, "last_error": "",
                    "privacy": self.privacy_active(now), "homed_ago_s": int(now - self.homed_at) if self.homed_at else None,
                    "presets": [{"slot": k, "name": p[0], "x": p[1], "y": p[2]} for k, p in sorted(self.presets.items())]}
        if c["audio"]:
            mic = {"state": "running", **({} if self.args.old_audio else self.mic), "frames": int(up * 25),
                   "silence_fill": 0, "dropped": 0, "restarts": 0, "error": ""}
            if self.siren and now >= self.siren["until"]: self.siren = None
            siren = {"sound": self.siren["sound"], "left_s": math.ceil(self.siren["until"] - now)} if self.siren else None
            speaker = {"state": "siren" if siren else "talking" if s["session"] else "idle", "siren": siren,
                       "gate_released": not self.args.no_speaker,
                       "volume": s["volume"], "half_duplex": True, "sessions": s["sessions"],
                       "played_ms": int(s["played"]), "dropped_ms": s["dropped"], "refused": s["refused"],
                       "faults": 0, "queued_ms": int(s["queued"] // 8), "error": ""}
        else:
            mic, speaker = {"state": "off"}, {"state": "off", "gate_released": True}
        audio = {"enabled": c["audio"], "microphone": mic, "speaker": speaker}
        if not self.args.old_audio:
            audio["muted"], audio["level"] = self.muted, self.level(now) if c["audio"] else None
        system = {} if self.args.no_system else {"system": {
            "uptime_s": int(now - self.system_boot), "memfree_kb": 6963, "state_free_kb": 100, "boot_mode": "native"}}
        return {"version": "0.1.0", "backend": "imp", "hardware_profile": "JA-A12-T23N-CV2005-CV2005S1",
                "hardware_validated": False, "uptime_seconds": round(up, 6), **system, "ready": True,
                "backend_error": "", "recording": self.recording, "record_error": "",
                "restart_pending": self.restart_at is not None,
                "streams": streams, "platform": platform, "clock": {"source": "none"},
                "audio": audio}

    def level(self, now):
        """audio.level: the last second of what the NVR receives; silence (talk or mute) is -96 dBFS."""
        if self.muted or self.spk["session"]:
            return {"peak_dbfs": -96.0, "rms_dbfs": -96.0}
        rms = -38 + 7 * math.sin(now / 3.1) + 4 * math.sin(now / 0.7)
        return {"peak_dbfs": round(min(0.0, rms + 14 + 3 * math.sin(now / 1.3)), 1), "rms_dbfs": round(rms, 1)}

    def config_post(self, body, now):
        if b"\\u0000" in body:
            return merr(400, "invalid JSON configuration patch")
        duplicate = []

        def pairs(items):
            duplicate.extend([True] if len({k for k, _ in items}) != len(items) else [])
            return dict(items)
        try:
            o = json.loads(body, object_pairs_hook=pairs)
        except ValueError:
            o = None
        if not isinstance(o, dict) or not o:
            return merr(400, "patch must be a nonempty JSON object")
        error = GENERIC if duplicate else self.patch(o, now)
        return merr(400, error) if error else mok({"ok": True})

    def patch(self, o, now):
        """jipc_config_patch(): returns an error message, or None once applied."""
        nxt, password, restart = dict(self.config), self.media_password, False
        for k, v in o.items():
            if k in STRINGS:
                if not isinstance(v, str) or len(v.encode()) >= STRINGS[k]: return GENERIC
                if k == "password":
                    password = v
                else:
                    nxt[k] = v
            elif k in BOOLS:
                if not isinstance(v, bool): return GENERIC
                nxt[k] = v
            elif k in NUMBERS:
                if (isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v)
                        or not 0 <= v <= 0xFFFFFFFF or v != int(v)):
                    return GENERIC
                nxt[k] = int(v)
            else:  # bind, ports, backend, realm, dirs, libimp, ntp_server, motor_home, input*, tls_*, ...
                return GENERIC
            restart |= k not in NO_RESTART
        error = validate_config(nxt, password)
        if error:
            return error
        self.config, self.media_password = nxt, password
        if "recording" in o:
            self.recording = nxt["recording"]
        if restart:
            self.schedule_restart(now)
        return None

    def control(self, body, now):
        try:
            o = json.loads(body)
        except ValueError:
            o = None
        o = o if isinstance(o, dict) else {}
        op, name = o.get("operation"), o.get("name")
        if not isinstance(op, str) or not op or len(op.encode()) >= 64:
            return merr(400, "A control operation is required")
        if not re.fullmatch("[a-z_]+", op):
            return merr(400, "Invalid control operation")
        try:
            value, value2 = jint(o, "value"), jint(o, "value2")
        except ValueError:
            return merr(400, "Control values must be integers")
        if "name" in o and (not isinstance(name, str) or len(name.encode("utf-8", "surrogatepass")) >= 65):
            return merr(400, "Invalid preset name")
        try:
            self.backend_control(op, value, value2, name if op in ("preset_set", "preset_rename") else None, now)
        except Fail as e:
            return merr(409, str(e))
        self.controls_note(op, value, value2, now)  # manual changes survive a restart
        return mok({"ok": True})

    def backend_control(self, op, v, v2, name, now):
        """jipc_backend_control() with the IMP backend: ISP and audio here, the rest on the platform."""
        sensors, audio = self.config["sensors"], self.config["audio"]
        if op == "idr":
            if not 0 <= v < sensors * 2: raise Fail("invalid stream channel")
        elif op == "night" and v in (2, 3):  # automatic or scheduled: the controls engine decides
            self.controls_set({"night": {"mode": NIGHT_MODES[v]}}, now)
        elif op == "night":
            if v not in (0, 1): raise Fail("night requires 0 (day), 1 (night), 2 (auto) or 3 (schedule)")
            self.platform_control(op, v, v2, None, now)
        elif op == "isp_night":
            if v not in (0, 1):
                raise Fail("night mode requires 0 (day) or 1 (night); automatic exposure mode is unsupported")
        elif op == "ir_mode":
            if v not in (0, 1): raise Fail("ir_mode requires 0 (infrared at night) or 1 (always off)")
            self.controls_set({"night": {"ir": IR_MODES[v]}}, now)
        elif op == "privacy":
            if v not in (0, 1): raise Fail("privacy takes value 1 (on) or 0 (off)")
            self.controls_set({"privacy": {"on": bool(v)}}, now)
        elif op == "strobe":
            if not 0 <= v <= 300: raise Fail("strobe takes 1 to 300 seconds, or 0 to stop")
        elif op in ("siren", "siren_test"):
            s = self.controls["siren"]
            if op == "siren" and v not in (0, 1): raise Fail("siren takes value 1 (on) or 0 (off)")
            if op == "siren" and not 0 <= v2 <= 300: raise Fail("the siren can sound for 1 to 300 seconds")
            if op == "siren_test" and not 0 <= v <= 3: raise Fail("siren_test takes a sound from 0 to 3")
            if op == "siren" and not v: self.siren = None
            elif not audio: raise Fail("the speaker is unavailable (audio is disabled)")
            elif op == "siren": self.siren = {"sound": s["sound"], "until": now + (v2 or s["seconds"])}
            else: self.siren = {"sound": SIREN_SOUNDS[v], "until": now + 3}
        elif op == "white_light_trigger":  # on for v seconds (0: trigger_s), then back to the schedule or manual state
            if v and not 1 <= v <= 3600: raise Fail("the light can stay on for 1 to 3600 seconds")
            self.ce["trigger_until"] = now + (v or self.controls["white_light"]["trigger_s"])
        elif op == "anti_flicker":
            if not 0 <= v <= 2: raise Fail("anti_flicker requires 0 (off), 1 (50 Hz) or 2 (60 Hz)")
        elif op == "volume" and v2 == 1:
            if not -30 <= v <= 120: raise Fail("speaker volume must be -30..120 (60 is unity gain)")
            if not audio: raise Fail("the speaker is unavailable (audio is disabled)")
            self.spk["volume"] = v
        elif op in ("flip", "hflip", "vflip"):
            if not 0 <= v2 < sensors or not 0 <= v <= (3 if op == "flip" else 1):
                raise Fail("invalid sensor or flip bitmask")
            flip, bit = self.config["flip%d" % v2], 1 if op == "hflip" else 2
            error = self.patch({"flip%d" % v2: v if op == "flip" else (flip & ~bit) | (bit if v else 0)}, now)
            if error: raise Fail(error)  # persisted, then an ordered restart
        elif op == "volume":
            if not -30 <= v <= 120 or v2 not in (0, 1):
                raise Fail("volume requires -30..120 and target 0 (microphone) or 1 (speaker)")
            if not audio: raise Fail("audio is off or libimp lacks the microphone volume control")
            self.mic["volume"] = v
        elif op == "gain":
            if not 0 <= v <= 31 or v2 not in (0, 1):
                raise Fail("gain requires 0..31 and target 0 (microphone) or 1 (speaker)")
            if not audio: raise Fail("audio is off or libimp lacks the gain control")
            self.mic["speaker_gain" if v2 else "gain"] = v
        elif op == "mic_mute":  # the RTSP audio track keeps running and carries silence
            if not audio: raise Fail("audio is off")
            if v not in (0, 1): raise Fail("mic_mute requires 0 or 1")
            self.muted = bool(v)
        elif op in PICTURE:
            if not 0 <= v2 < sensors: raise Fail("invalid sensor index")
            if not 0 <= v <= 255: raise Fail("denoise must be in 0..255" if op == "denoise" else "image control must be in 0..255")
        else:  # ptz*, preset_*, ircut, lights, speaker_enable, and the "unsupported" message
            self.platform_control(op, v, v2, name, now)

    def platform_control(self, op, v, v2, name, now):
        """media/src/platform.c platform_control(): its validation order and messages."""
        motion = op in ("ptz", "ptz_nudge")
        preset = op in ("preset_set", "preset_goto", "preset_remove", "preset_rename")
        recalibrate = op == "ptz_recalibrate"
        if not motion and not preset and not recalibrate and op not in ("ptz_stop", "night", "ircut", "white_light", "pilot_led",
                                                                        "speaker_enable"):
            raise Fail("unsupported platform operation: " + op)
        if motion and not (-100 <= v <= 100 and -100 <= v2 <= 100):
            raise Fail("PTZ velocity must be between -100 and 100")
        if preset and not 1 <= v <= 16:
            raise Fail("preset slot must be between 1 and 16")
        if op in ("night", "ircut") and v == 2:
            raise Fail("automatic day/night requires an unverified sensor exposure ABI; select day=0 or night=1")
        if not motion and not preset and op != "ptz_stop" and v not in (0, 1):
            raise Fail("control value must be 0 or 1")
        if name is not None and not preset_name_ok(name):
            raise Fail("preset name requires 1..64 printable UTF-8 bytes")
        if self.homing(now) and (motion or recalibrate or (preset and op != "preset_rename") or op == "ptz_stop"):
            raise Fail("pan/tilt homing in progress")
        if self.privacy_active(now) and (motion or recalibrate or op in ("preset_goto", "preset_set")):
            raise Fail("privacy mode is on")
        if motion or op == "preset_goto": self.manual_at = now
        if recalibrate:
            self.homing_until = self.homed_at = now + max(self.args.homing, 5)
            self.motion = self.goto = None
            return
        if motion:  # the dominant axis wins; an identical repeat renews the lease
            vx, vy = (v, 0) if abs(v) >= abs(v2) else (0, v2)
            nudge, m = op == "ptz_nudge", self.motion
            if m and m["v"] == (vx, vy) and m["nudge"] == nudge:
                if not nudge:
                    m["until"] = now + LEASE
                return
            self.motion = self.goto = None
            if vx or vy:
                rate = 540 if nudge else clamp((abs(vx) + abs(vy)) * 9, 100, 900)  # motor steps/s
                self.motion = {"v": (vx, vy), "nudge": nudge, "t": now, "until": now + (NUDGE if nudge else LEASE),
                               "ux": math.copysign(rate * 200 / PAN_STEPS, vx) if vx else 0.0,
                               "uy": math.copysign(rate * 200 / TILT_STEPS, vy) if vy else 0.0}
        elif op in ("ptz_stop", "preset_set", "preset_remove"):
            self.motion = self.goto = None
            if op == "preset_set":
                old = self.presets.get(v)
                label = name if name is not None else old[0] if old else "Preset %d" % v
                if any(k != v and p[0].encode().lower() == label.encode().lower() for k, p in self.presets.items()):
                    raise Fail("preset name already exists")
                self.presets[v] = [label, round(self.x), round(self.y)]
            elif op == "preset_remove":
                self.presets.pop(v, None)
        elif op == "preset_rename":  # the name only; the saved position stays
            if name is None: raise Fail("preset_rename requires a name")
            if v not in self.presets: raise Fail("preset slot is empty")
            if any(k != v and p[0].encode().lower() == name.encode().lower() for k, p in self.presets.items()):
                raise Fail("preset name already exists")
            self.presets[v][0] = name
        elif op == "preset_goto":
            p = self.presets.get(v)
            if not p: raise Fail("preset slot is empty")
            self.motion, dist = None, math.hypot(p[1] - self.x, p[2] - self.y)
            self.goto = {"t0": now, "dur": 2 + min(1.0, dist / 150), "x0": self.x, "y0": self.y, "x1": p[1], "y1": p[2]}
        else:
            self.ctl["night" if op == "ircut" else op] = v

    def talk(self, h):
        s, n = self.spk, len(h.body)
        if h.headers.get("Content-Type", "").lower() not in ("audio/pcma", "audio/pcma;rate=8000"):
            return merr(415, "Use audio/PCMA at 8000 Hz mono")
        if not n: return merr(400, "Audio body is empty")
        if not self.config["audio"]: return merr(503, "the speaker is unavailable (audio is disabled)")
        if self.args.no_speaker: return merr(503, "Speaker unavailable")
        if self.args.talk_busy:
            s["refused"] += 1
            return merr(409, "another talker is using the speaker")
        if s["queued"] + n > QUEUE_BYTES: return merr(409, "the speaker queue is full; send shorter chunks")
        if not s["session"]:
            s["session"], s["sessions"] = True, s["sessions"] + 1
        s["queued"] += n
        if self.args.talk_capture:
            with open(self.args.talk_capture, "ab") as f:
                f.write(h.body)
        return mok({"ok": True})


class Handler(BaseHTTPRequestHandler):
    protocol_version, timeout, body = "HTTP/1.1", 60, b""

    def log_message(self, *_):
        pass

    def parse_request(self):
        # A space inside the target would split the request line; fold it into
        # "%20" so the target check answers it the way the camera does.
        words = self.raw_requestline.rstrip(b"\r\n").split(b" ")
        if len(words) > 3 and words[-1].startswith(b"HTTP/"):
            self.raw_requestline = b" ".join((words[0], b"%20".join(words[1:-1]), words[-1])) + b"\r\n"
        return super().parse_request()

    def joan_cookies(self):
        pairs = (p.strip().partition("=") for c in self.headers.get_all("Cookie") or [] for p in c.split(";"))
        return [v for k, _, v in pairs if k == "joan_session" and re.fullmatch("[0-9A-Fa-f]{64}", v)]

    def cross_origin(self):
        """A lenient same_origin(): a foreign Origin or Sec-Fetch-Site refuses, a missing one passes."""
        site, origin = self.headers.get("Sec-Fetch-Site"), self.headers.get("Origin")
        return bool(site and site not in ("same-origin", "none")) or \
            bool(origin and origin != "http://" + self.headers.get("Host", ""))

    def log(self, result):
        sys.stderr.write("%s %s %s -> %s %.0f ms\n" % (time.strftime("%H:%M:%S"), self.command, self.path,
                                                      result, (time.time() - self.started) * 1000))

    def serve(self):
        self.started, mock, args = time.time(), self.server.mock, self.server.args
        if self.path.startswith("/__mock/"):
            return self.send(mock.backdoor(self.path), secure=False)
        with mock.lock:
            mock.tick(self.started)
            down = mock.unreachable(self.started)
        if down:
            return self.drop(args.fail_mode == "hang")
        try:
            if any(c in self.path for c in "%\\#"):
                raise merr(400, "Invalid request target")
            self.body = self.read_body()
            if self.command == "GET" and (self.path == "/" or self.path.startswith("/ui/")):
                reply = self.static()
            else:
                relay = self.path.startswith("/api/joan/")
                with mock.lock:  # the camera relays two platform requests at a time
                    if relay and mock.relays >= 2:
                        raise merr(503, "The camera is busy; retry in a moment")
                    mock.relays += relay
                try:
                    time.sleep(args.latency / 1000)
                    with mock.lock:
                        now = time.time()
                        mock.tick(now)
                        reply = mock.dispatch(self, now)
                finally:
                    with mock.lock:
                        mock.relays -= relay
        except Reply as e:
            reply = e
        except Drop:
            return self.drop(False)
        self.send(reply)

    do_GET = do_HEAD = do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = serve

    def drop(self, hang):
        if hang:
            time.sleep(30)
        self.close_connection = True
        self.log("dropped (not answering)")

    def read_body(self):
        length, m = self.headers.get("Content-Length"), self.command
        if self.headers.get("Transfer-Encoding"): raise merr(400, "Bad Request")
        if length is None:
            if m in ("POST", "PUT"): raise merr(411, "Length Required")
            return b""
        if not re.fullmatch("[0-9]+", length.strip()): raise merr(400, "Bad Request")
        n = int(length)
        if m not in ("POST", "PUT", "DELETE"):
            if n: raise merr(400, "Bad Request")
            return b""
        if m == "POST" and self.path == "/api/joan/v1/update":
            limit, error = UPDATE_LIMIT, perr(413, "payload_too_large", "request body exceeds route limit")
        elif m == "POST" and self.path == "/api/v1/talk":
            limit, error = TALK_LIMIT, merr(413, "Audio chunk exceeds 8000 bytes")
        else:
            limit, error = JSON_LIMIT, merr(413, "Content Too Large")
        if n > limit:
            # The camera answers and closes without reading; draining (bounded)
            # lets the client see the 413 instead of a reset connection.
            self.close_connection = True
            try:
                self.connection.settimeout(2)
                left = min(n, 4 << 20)
                while left > 0:
                    chunk = self.rfile.read(min(65536, left))
                    left = left - len(chunk) if chunk else 0
            except OSError:
                pass
            raise error
        return self.rfile.read(n)

    def static(self):
        name = "index.html" if self.path == "/" else self.path[4:]
        if not name or "/" in name or ".." in name or name.startswith("."):
            return merr(404, "Not found")
        try:
            with open(os.path.join(self.server.args.ui_dir, name), "rb") as f:
                data = f.read()
        except OSError:
            return Reply(200, FALLBACK, TYPES[".html"]) if name == "index.html" else merr(404, "Not found")
        return Reply(200, data, TYPES.get(os.path.splitext(name)[1].lower(), "text/plain"))

    def send(self, r, secure=True):
        headers = [("Content-Type", r.ctype), ("Content-Length", str(len(r.body))),
                   ("Connection", "close" if self.close_connection else "keep-alive"), ("Cache-Control", "no-store")]
        if secure:
            headers += [("X-Content-Type-Options", "nosniff"), ("X-Frame-Options", "DENY"),
                        ("Referrer-Policy", "no-referrer"), ("Content-Security-Policy", CSP)]
        if r.status == 401 and not self.headers.get("Cookie") and not self.headers.get("X-Requested-With"):
            headers.append(("WWW-Authenticate", 'Basic realm="jooanipc-open", charset="UTF-8"'))
        try:
            self.send_response_only(r.status)
            for k, v in headers + r.extra:
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(r.body)
        except OSError:
            self.close_connection = True
        self.log(r.status)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    p = argparse.ArgumentParser(description="Mock camera1's HTTP API and serve the admin UI for local development.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--password", default="admin", help="platform sign-in password (user admin)")
    p.add_argument("--fail", action="store_true", help='start "not answering" (toggle: /__mock/fail?on=0|1)')
    p.add_argument("--fail-mode", choices=("drop", "hang"), default="drop",
                   help="not answering: close at once, or hold the request 30 s and then close")
    p.add_argument("--latency", type=int, default=0, metavar="MS", help="delay every API response")
    p.add_argument("--old-audio", action="store_true", help="status without microphone volume/gain/speaker_gain, audio.muted and audio.level")
    p.add_argument("--no-presets", action="store_true", help="start with no PTZ presets")
    p.add_argument("--no-aim", action="store_true", help="aiming from the fixed lens is not calibrated")
    p.add_argument("--homing", type=float, default=0, metavar="SECONDS", help="pan/tilt homing after each boot")
    p.add_argument("--talk-busy", action="store_true", help="talk: another talker holds the speaker (409)")
    p.add_argument("--no-speaker", action="store_true", help="talk: speaker unavailable (503)")
    p.add_argument("--talk-capture", metavar="PATH", help="append accepted talk audio (raw A-law) to PATH")
    p.add_argument("--ssh-unsynced", action="store_true", help="status: ssh_password_sync false")
    p.add_argument("--idle", type=float, default=1800, metavar="SECONDS", help="session idle timeout")
    p.add_argument("--wifi-mode", choices=("ok", "lost"), default="ok",
                   help="lost: joining drops the connection and the camera is unreachable for 15 s")
    p.add_argument("--restart-outage", type=float, default=0, metavar="SECONDS",
                   help="after a config change that restarts services, stop answering this long (the real daemon "
                        "restarts its HTTP server too); 0: restart_pending for 3 s instead")
    p.add_argument("--no-system", action="store_true", help="status without \"system\" (older firmware)")
    p.add_argument("--no-camera-zone", action="store_true",
                   help="the camera has no time zone: schedules use utc_offset_min (zone \"browser\")")
    p.add_argument("--auto-hold", type=float, default=300, metavar="SECONDS",
                   help="the least time between automatic day/night switches")
    p.add_argument("--restart-seconds", type=float, default=20, metavar="SECONDS",
                   help="how long a requested or scheduled restart keeps the camera away")
    p.add_argument("--restart-blocked", choices=("update", "boot-trial"),
                   help="system/restart refuses with this reason (a Wi-Fi trial refuses by itself)")
    p.add_argument("--update-status", help="what GET /api/v1/update answers, in turn: a state, or a status code")
    p.add_argument("--ui-dir", default=os.path.normpath(os.path.join(here, "..", "ui")))
    args = p.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads, server.mock, server.args = True, Mock(args), args
    print("camera1 UI mock: http://%s:%d/  (admin / %s; UI from %s)"
          % (args.host, server.server_address[1], args.password, args.ui_dir), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
