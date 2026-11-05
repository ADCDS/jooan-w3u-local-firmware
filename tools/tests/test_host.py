#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host check: which names may reach the platform, login and pages included.

Runs the host-built daemon (src/daemon/joan-daemon) on loopback with the mDNS
label set to camera1. Never touches a camera.
"""
import http.client, json, os, pathlib, subprocess, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / 'src/daemon/joan-daemon'
HELPER = ROOT / 'tools/tests/fake-integration.sh'
PORT = 18094


def get(host, path='/api/health'):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=10)
    c.putrequest('GET', path, skip_host=True)
    c.putheader('Host', host)
    c.endheaders()
    r = c.getresponse()
    return r.status, r.read()


with tempfile.TemporaryDirectory() as state, tempfile.TemporaryDirectory() as staging, \
        tempfile.TemporaryDirectory() as run:
    pathlib.Path(state, 'hostname').write_text('camera1\n')
    env = os.environ | {
        'JOAN_STATE_DIR': state, 'JOAN_STAGING_DIR': staging, 'JOAN_RUN_DIR': run,
        'JOAN_WEB_DIR': str(ROOT / 'web'), 'JOAN_INTEGRATION_HELPER': str(HELPER),
        'JOAN_MQTT_PORT': '0', 'JOAN_MDNS': '0',
    }
    daemon = subprocess.Popen([BIN, '--plain-mqtt', '--bind', '127.0.0.1', '--port', str(PORT)],
                              env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                if get('127.0.0.1')[0] == 200:
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError('daemon did not listen')
        allowed = ['127.0.0.1', '127.0.0.1:18094', '192.168.20.38', '10.1.2.3', '172.16.0.9',
                   '[::1]', '[fd00::5]:80', 'camera1', 'camera1.local', 'CAMERA1.LOCAL', 'camera1.lan',
                   'camera1.lan.', 'camera1.home.arpa', 'camera1.internal', 'camera1.lan:80']
        refused = ['evil.example', 'camera1.evil.example', 'camera1.lan.evil.example', 'camera10.lan',
                   'xcamera1.lan', 'camera1.com', 'jooan-w3u.local', '8.8.8.8', '[2001:db8::1]']
        for host in allowed:
            assert get(host)[0] == 200, host
        for host in refused:
            status, body = get(host)
            assert status == 403 and json.loads(body)['error']['code'] == 'host_rejected', (host, status)
        assert get('')[0] in (400, 403)  # an empty Host is malformed before it is checked
        # Login and static pages are behind the same check.
        assert get('evil.example', '/')[0] == 403
        assert get('camera1.lan', '/')[0] == 200
        c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=10)
        c.putrequest('POST', '/api/v1/session', skip_host=True)
        body = json.dumps({'username': 'admin', 'password': 'admin'}).encode()
        c.putheader('Host', 'evil.example'); c.putheader('Content-Type', 'application/json')
        c.putheader('Content-Length', str(len(body))); c.endheaders(body)
        assert c.getresponse().status == 403
        print('host check: IP literals, label bare/.local/.lan/.home.arpa/.internal allowed; '
              'other names, login and pages refused PASS')
    finally:
        daemon.terminate()
        daemon.wait(timeout=10)
