#!/usr/bin/env python3
"""Real daemon/API/RTSP tests, entirely on loopback and temporary state."""
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
BIN = ROOT / 'src/daemon/joan-daemon'
ROUTE = '/api/v1/system/audio-pause'


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Fixture:
    def __init__(self, base):
        self.base = Path(base)
        self.http, self.rtsp, self.upstream = port(), port(), port()
        self.state, self.run = self.base / 'state', self.base / 'run'
        self.state.mkdir(); self.run.mkdir()
        self.process = None
        self.clients = []
        self.send_media = threading.Event(); self.send_media.set()
        self.stop = threading.Event()
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', self.upstream)); self.listener.listen(4)
        self.listener.settimeout(.2)
        threading.Thread(target=self.serve_upstream, daemon=True).start()

    def serve_upstream(self):
        while not self.stop.is_set():
            try:
                s, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.media, args=(s,), daemon=True).start()

    def media(self, s):
        with s:
            try:
                s.recv(8192)
                s.sendall(b'RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n')
                while not self.stop.is_set():
                    if self.send_media.is_set():
                        s.sendall(b'$\x00\x03\xfc' + b'\x80' * 1020)
                    time.sleep(.02)
            except OSError:
                pass

    def request(self, method, body=None, authenticated=True, csrf=True):
        c = http.client.HTTPConnection('127.0.0.1', self.http, timeout=4)
        headers = {'Content-Type': 'application/json'}
        if authenticated: headers['Cookie'] = self.cookie
        if csrf: headers['X-CSRF-Token'] = self.csrf
        if isinstance(body, dict): body = json.dumps(body)
        c.request(method, ROUTE, body, headers)
        r = c.getresponse(); result = r.status, json.loads(r.read()); c.close()
        return result

    def start(self):
        env = dict(os.environ, JOAN_STATE_DIR=str(self.state), JOAN_RUN_DIR=str(self.run),
                   JOAN_STAGING_DIR=str(self.base / 'staging'), JOAN_MQTT_PORT='0',
                   JOAN_MDNS='0', JOAN_RTSP_PORT=str(self.upstream), JOAN_RTSP_PROXY_PORT=str(self.rtsp))
        self.process = subprocess.Popen([str(BIN), '--plain-mqtt', '--bind', '127.0.0.1', '--port', str(self.http)],
                                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                c = http.client.HTTPConnection('127.0.0.1', self.http, timeout=.5)
                c.request('POST', '/api/v1/session', json.dumps({'username': 'admin', 'password': 'admin'}),
                          {'Content-Type': 'application/json'})
                r = c.getresponse(); data = json.loads(r.read())
                assert r.status == 200, data
                self.cookie = r.getheader('Set-Cookie').split(';')[0]
                self.csrf = data['csrf']; c.close(); return
            except OSError:
                time.sleep(.05)
        raise AssertionError('daemon not ready')

    def stop_daemon(self, sig=signal.SIGTERM):
        if self.process and self.process.poll() is None:
            self.process.send_signal(sig); self.process.wait(timeout=5)
        for s in self.clients: s.close()
        self.clients.clear()

    def video(self, channel):
        s = socket.create_connection(('127.0.0.1', self.rtsp), timeout=3)
        s.settimeout(3)
        uri = f'rtsp://127.0.0.1/live/ch{channel}'
        s.sendall(f'DESCRIBE {uri} RTSP/1.0\r\nCSeq: 1\r\n\r\n'.encode())
        challenge = s.recv(4096).decode()
        nonce = re.search(r'nonce="([^"]+)"', challenge)[1]
        md5 = lambda x: hashlib.md5(x.encode()).hexdigest()
        ha1 = md5('admin:joan-rtsp:admin')
        response = md5(f'{ha1}:{nonce}:00000001:test:auth:{md5("DESCRIBE:" + uri)}')
        auth = f'Digest username="admin", realm="joan-rtsp", nonce="{nonce}", uri="{uri}", response="{response}", qop=auth, nc=00000001, cnonce="test"'
        s.sendall(f'DESCRIBE {uri} RTSP/1.0\r\nCSeq: 2\r\nAuthorization: {auth}\r\n\r\n'.encode())
        self.clients.append(s)
        def drain():
            try:
                while s.recv(65536): pass
            except OSError:
                pass
        threading.Thread(target=drain, daemon=True).start()

    def wait_for(self, condition, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition(): return
            time.sleep(.1)
        raise AssertionError('condition timed out')

    def close(self):
        self.stop_daemon(); self.stop.set(); self.listener.close()


with tempfile.TemporaryDirectory() as temp:
    f = Fixture(temp)
    try:
        f.start()
        assert f.request('GET')[1] == {'enabled': False, 'active': False, 'state': 'running', 'recovery_reason': ''}
        assert f.request('GET', authenticated=False)[0] == 401
        assert f.request('PUT', {'enabled': True}, csrf=False)[0] == 403
        for bad in ({}, {'enabled': 'true'}, {'enabled': 1}, {'enabled': None},
                    {'enabled': True, 'extra': 1}, '{"enabled":true,"enabled":false}', '{"enabled":true}junk', b'{"enabled":true}\0ignored'):
            assert f.request('PUT', bad)[0] == 400, bad
        assert f.request('PUT', {'enabled': True})[1]['enabled']
        assert f.request('GET')[1]['state'] == 'waiting_for_video'
        assert not (f.run / 'audio-pause.lease').exists()
        f.stop_daemon(); f.start()
        assert f.request('GET')[1]['enabled'], 'setting did not persist'
        f.video(0); f.video(1)
        f.wait_for(lambda: (f.run / 'audio-pause.lease').exists())
        assert (f.state / 'audio-pause.pending').exists()
        # Simulate the guard acknowledgement, not a fake effective UI state.
        (f.run / 'audio-pause.active').write_text(f'v1 {os.getpid()} {int(time.monotonic())}\n')
        assert f.request('GET')[1]['active']
        ack_stop = threading.Event()
        def acknowledge():
            while not ack_stop.is_set():
                ack = f.run / 'stall-ack.new'
                ack.write_text(f'v1 {os.getpid()} {int(time.monotonic())}\n')
                ack.replace(f.run / 'audio-pause.active')
                time.sleep(.1)
        ack_thread = threading.Thread(target=acknowledge, daemon=True)
        ack_thread.start()
        f.send_media.clear()
        try:
            f.wait_for(lambda: not f.request('GET')[1]['enabled'], timeout=12)
            assert f.request('GET')[1]['recovery_reason'] == 'video_stalled'
        finally:
            ack_stop.set(); ack_thread.join(timeout=2)
        assert not (f.run / 'audio-pause.lease').exists()
        assert (f.state / 'audio-pause').read_text() == '0\n'
        # Fresh video permits an explicit retry; abrupt loss during its first
        # minute must disable the setting when the daemon next starts.
        f.send_media.set(); time.sleep(.2)
        f.request('PUT', {'enabled': True})
        f.wait_for(lambda: (f.state / 'audio-pause.pending').exists())
        f.stop_daemon(signal.SIGKILL); f.start()
        state = f.request('GET')[1]
        assert not state['enabled'] and state['recovery_reason'] == 'previous_start_interrupted', state
        assert (f.state / 'audio-pause').stat().st_mode & 0o777 == 0o600
        # This CI build uses 2-second ACK / 4-second trial thresholds. The
        # release build retains 15 / 60 seconds and is checked on hardware.
        f.video(0); f.video(1)
        f.request('PUT', {'enabled': True})
        f.wait_for(lambda: (f.state / 'audio-pause.pending').exists())
        (f.run / 'audio-pause.active').unlink(missing_ok=True)
        f.wait_for(lambda: not f.request('GET')[1]['enabled'], timeout=6)
        assert f.request('GET')[1]['recovery_reason'] == 'callback_unavailable'
        f.request('PUT', {'enabled': True})
        f.wait_for(lambda: (f.state / 'audio-pause.pending').exists())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            ack = f.run / 'test-ack.new'
            ack.write_text(f'v1 {os.getpid()} {int(time.monotonic())}\n')
            ack.replace(f.run / 'audio-pause.active')
            time.sleep(.1)
        assert f.request('GET')[1]['active']
        assert not (f.state / 'audio-pause.pending').exists(), 'healthy trial marker not cleared'
        f.stop_daemon(); f.start()
        assert f.request('GET')[1]['enabled'], 'healthy clean restart lost setting'
        assert f.request('PUT', {'enabled': False})[1]['state'] == 'running'
        print('audio pause: auth/CSRF/JSON, persistence, video stall and interrupted trial recovery PASS')
    finally:
        f.close()
