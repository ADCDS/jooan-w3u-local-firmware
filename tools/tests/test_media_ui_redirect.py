#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Port 80 points at the media daemon's HTTPS UI when the media layer is
installed (JOAN_MEDIA_UI_MARKER exists), and serves its own pages otherwise.
The API answers either way. Runs the host-built daemon on loopback.
"""
import http.client, json, os, pathlib, subprocess, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / 'src/daemon/joan-daemon'
HELPER = ROOT / 'tools/tests/fake-integration.sh'
PORT = 18097


def get(host, path='/'):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=10)
    c.putrequest('GET', path, skip_host=True)
    c.putheader('Host', host)
    c.endheaders()
    r = c.getresponse()
    return r.status, r.getheader('Location'), r.read()


with tempfile.TemporaryDirectory() as state, tempfile.TemporaryDirectory() as staging, \
        tempfile.TemporaryDirectory() as run:
    pathlib.Path(state, 'hostname').write_text('camera1\n')
    marker = pathlib.Path(run, 'deployment.env')
    env = os.environ | {
        'JOAN_STATE_DIR': state, 'JOAN_STAGING_DIR': staging, 'JOAN_RUN_DIR': run,
        'JOAN_WEB_DIR': str(ROOT / 'web'), 'JOAN_INTEGRATION_HELPER': str(HELPER),
        'JOAN_MQTT_PORT': '0', 'JOAN_MDNS': '0', 'JOAN_MEDIA_UI_MARKER': str(marker),
    }
    daemon = subprocess.Popen([BIN, '--plain-mqtt', '--bind', '127.0.0.1', '--port', str(PORT)],
                              env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                if get('127.0.0.1', '/api/health')[0] == 200:
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError('daemon did not listen')
        # Platform only: its own pages.
        status, location, body = get('camera1.lan')
        assert status == 200 and location is None and b'<' in body, status
        # Media layer installed: every page points at https://<host>/, port dropped.
        marker.write_text('JIPC_RELEASE=/mnt/mtd/jooanipc-open/releases/x\n')
        for host, target in [('camera1.local', 'https://camera1.local/'),
                             ('camera1.lan:80', 'https://camera1.lan/'),
                             ('192.168.20.38', 'https://192.168.20.38/'),
                             ('[fd00::5]:80', 'https://[fd00::5]/')]:
            for path in ['/', '/index.html', '/app.js']:
                status, location, _ = get(host, path)
                assert status == 302 and location == target, (host, path, status, location)
        # The host check still comes first, and the API is unchanged.
        assert get('evil.example')[0] == 403
        status, _, body = get('camera1.lan', '/api/health')
        assert status == 200 and json.loads(body) == {'ok': True}, body
        marker.unlink()
        assert get('camera1.lan')[0] == 200
        print('media UI redirect: PASS')
    finally:
        daemon.terminate()
        daemon.wait(timeout=10)
