#!/usr/bin/env python3
"""Large parent-domain Cookie headers must never hide the camera session.

Runs the host-built daemon (src/daemon/joan-daemon) in plain-HTTP mode against
a temporary state directory and checks session lookup through the real HTTP
header parser. Never touches a camera.
"""
import http.client, json, os, pathlib, subprocess, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / 'src/daemon/joan-daemon'
HELPER = ROOT / 'tools/tests/fake-integration.sh'
PORT = 18091


def request(method, path, body=None, cookie='', csrf=''):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=10)
    headers = {'Content-Type': 'application/json'}
    if cookie:
        headers['Cookie'] = cookie
    if csrf:
        headers['X-CSRF-Token'] = csrf
    c.request(method, path, body=json.dumps(body) if isinstance(body, dict) else body, headers=headers)
    r = c.getresponse()
    return r.status, dict(r.getheaders()), r.read()


with tempfile.TemporaryDirectory() as state, tempfile.TemporaryDirectory() as staging:
    env = os.environ | {
        'JOAN_STATE_DIR': state, 'JOAN_STAGING_DIR': staging,
        'JOAN_WEB_DIR': str(ROOT / 'web'), 'JOAN_INTEGRATION_HELPER': str(HELPER),
        'JOAN_MQTT_PORT': '0', 'JOAN_CONNECTIVITY_PORT': '0', 'JOAN_RTSP_PORT': '18594', 'JOAN_MDNS': '0',
    }
    daemon = subprocess.Popen([BIN, '--plain-http', '--bind', '127.0.0.1', '--port', str(PORT)],
                              env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                if request('GET', '/api/health')[0] == 200:
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError('daemon did not listen')
        status, headers, body = request('POST', '/api/v1/session', {'username': 'admin', 'password': 'admin'})
        assert status == 200, (status, body)
        session = headers['Set-Cookie'].split(';', 1)[0]
        csrf = json.loads(body)['csrf']
        pad = '; '.join(f'other{i}=' + 'x' * 60 for i in range(40))  # ~2.8 KB of unrelated cookies
        stale = 'joan_session=' + '0' * 64

        assert request('GET', '/api/v1/status', cookie=session)[0] == 200
        assert request('GET', '/api/v1/status', cookie=f'{pad}; {session}')[0] == 200, 'session after large cookies'
        assert request('GET', '/api/v1/status', cookie=f'{session}; {pad}')[0] == 200, 'session before large cookies'
        assert request('GET', '/api/v1/status', cookie=f'{stale}; {pad}; {session}')[0] == 200, 'stale sibling cookie'
        assert request('GET', '/api/v1/status', cookie=f'x{session}')[0] == 401, 'suffix match must not authenticate'
        assert request('GET', '/api/v1/status', cookie=pad)[0] == 401
        assert request('PUT', '/api/v1/network/mdns', {'hostname': 'cam'}, f'{pad}; {session}', csrf)[0] == 200
        denied = request('PUT', '/api/v1/network/mdns', {'hostname': 'cam'}, f'{pad}; {session}', 'wrong')
        assert denied[0] == 403 and json.loads(denied[2])['error']['code'] == 'csrf_rejected'
        print('cookie header: large, reordered, stale-sibling and CSRF cases PASS')
    finally:
        daemon.terminate()
        try:
            daemon.wait(3)
        except subprocess.TimeoutExpired:
            daemon.kill()
