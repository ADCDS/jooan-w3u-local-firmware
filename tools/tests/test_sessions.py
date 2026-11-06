#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Remembered sign-ins and the system routes, on the host-built daemon: a
remembered cookie survives a daemon restart, an ordinary one does not; sign-out
and a password change end them; the file holds hashes, never cookies. Restart is
refused with its reason as a 409. Never touches a camera."""
import http.client, json, os, pathlib, stat, subprocess, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / 'src/daemon/joan-daemon'
PORT = 18098


def call(method, path, body=None, cookie=None, csrf=None):
    c = http.client.HTTPConnection('127.0.0.1', PORT, timeout=10)
    headers = {}
    if body is not None: headers['Content-Type'] = 'application/json'
    if cookie: headers['Cookie'] = cookie
    if csrf: headers['X-CSRF-Token'] = csrf
    c.request(method, path, body=json.dumps(body) if body is not None else None, headers=headers)
    r = c.getresponse(); data = r.read(); set_cookie = r.getheader('Set-Cookie')
    c.close()
    return r.status, data, set_cookie


def sign_in(remember):
    body = {'username': 'admin', 'password': password}
    if remember: body['remember'] = True
    status, data, cookie = call('POST', '/api/v1/session', body)
    assert status == 200, data
    return cookie.split(';')[0], json.loads(data)['csrf'], cookie, json.loads(data)


with tempfile.TemporaryDirectory() as state, tempfile.TemporaryDirectory() as staging, tempfile.TemporaryDirectory() as run:
    helper = pathlib.Path(run, 'helper.sh')
    helper.write_text('#!/bin/sh\ncase "$1" in\n'
                      '  restart) cat "$FAKE_RESTART" ;;\n'
                      '  factory-reset) : > "$FAKE_RESET_CALLED"; cat "$FAKE_RESTART" ;;\n'
                      '  restart-schedule|restart-schedule-set) printf \'{"enabled":true,"time":"%s"}\\n\' "$3" ;;\n'
                      '  wifi-state) echo \'{"pending":false,"last":null}\' ;;\n'
                      '  wifi-scan) echo \'[{"ssid":"Home"}]\' ;;\n'
                      '  *) exit 0 ;;\nesac\n')
    helper.chmod(0o755)
    restart_reply = pathlib.Path(run, 'restart.json')
    reset_called = pathlib.Path(run, 'reset-called')
    gpio = pathlib.Path(run, 'gpio62')
    gpio.mkdir()
    button = gpio / 'value'
    button.write_text('0\n')    # stuck pressed from the start
    env = os.environ | {'JOAN_STATE_DIR': state, 'JOAN_STAGING_DIR': staging, 'JOAN_RUN_DIR': run,
                        'JOAN_WEB_DIR': str(ROOT / 'web'), 'JOAN_INTEGRATION_HELPER': str(helper),
                        'JOAN_MQTT_PORT': '0', 'JOAN_MDNS': '0', 'FAKE_RESTART': str(restart_reply),
                        'FAKE_RESET_CALLED': str(reset_called), 'JOAN_BUTTON_GPIO_DIR': str(gpio)}
    password = 'admin'

    def start():
        d = subprocess.Popen([BIN, '--plain-mqtt', '--bind', '127.0.0.1', '--port', str(PORT)], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                if call('GET', '/api/health')[0] == 200: return d
            except OSError:
                time.sleep(0.05)
        raise AssertionError('daemon did not listen')

    daemon = start()
    try:
        plain, _, set_plain, reply = sign_in(False)
        assert 'Max-Age' not in set_plain and reply['remember'] is False, set_plain
        kept, kept_csrf, set_kept, reply = sign_in(True)
        assert 'Max-Age=2592000' in set_kept and reply['remember'] is True, set_kept
        db = pathlib.Path(state, 'sessions.db')
        assert kept.split('=')[1] not in db.read_text() and stat.S_IMODE(db.stat().st_mode) == 0o600
        assert call('GET', '/api/v1/session', cookie=kept)[0] == 200
        # Across a restart of the daemon only the remembered one survives.
        daemon.terminate(); daemon.wait(timeout=10); daemon = start()
        assert call('GET', '/api/v1/session', cookie=plain)[0] == 401
        status, data, _ = call('GET', '/api/v1/session', cookie=kept)
        assert status == 200 and json.loads(data)['csrf'] == kept_csrf, data
        # CSRF still applies to a restored sign-in.
        assert call('POST', '/api/v1/system/restart', {}, cookie=kept)[0] == 403
        # Restart: refused with the reason, or accepted.
        restart_reply.write_text('{"ok":false,"reason":"update"}\n')
        status, data, _ = call('POST', '/api/v1/system/restart', {}, cookie=kept, csrf=kept_csrf)
        assert status == 409 and json.loads(data)['reason'] == 'update', (status, data)
        restart_reply.write_text('{"ok":true}\n')
        assert call('POST', '/api/v1/system/restart', {}, cookie=kept, csrf=kept_csrf)[0] == 200
        assert call('POST', '/api/v1/system/restart', {})[0] == 401
        # Schedule validation happens in the daemon before the helper runs.
        for bad in ('24:00', '4:30', '12:60', 'noon'):
            assert call('PUT', '/api/v1/system/restart-schedule', {'time': bad}, cookie=kept, csrf=kept_csrf)[0] == 400, bad
        status, data, _ = call('PUT', '/api/v1/system/restart-schedule', {'time': '04:30'}, cookie=kept, csrf=kept_csrf)
        assert status == 200 and json.loads(data)['time'] == '04:30', data
        status, data, _ = call('PUT', '/api/v1/system/restart-schedule', {'time': ''}, cookie=kept, csrf=kept_csrf)
        assert status == 200 and json.loads(data)['time'] == 'off', data
        assert call('GET', '/api/v1/network/wifi', cookie=kept)[1].startswith(b'{"pending"')
        assert json.loads(call('GET', '/api/v1/network/wifi/scan', cookie=kept)[1])[0]['ssid'] == 'Home'
        # Reset to defaults: typing the camera's name confirms it, with a restart's refusals.
        label = json.loads(call('GET', '/api/v1/network/mdns', cookie=kept)[1])['hostname']
        assert call('POST', '/api/v1/system/reset', {}, cookie=kept, csrf=kept_csrf)[0] == 400
        status, data, _ = call('POST', '/api/v1/system/reset', {'confirm': label + 'x'}, cookie=kept, csrf=kept_csrf)
        assert status == 400 and b'confirm_mismatch' in data, data
        assert not reset_called.exists()
        restart_reply.write_text('{"ok":false,"reason":"wifi-trial"}\n')
        assert call('POST', '/api/v1/system/reset', {'confirm': label}, cookie=kept, csrf=kept_csrf)[0] == 409
        reset_called.unlink()
        restart_reply.write_text('{"ok":true}\n')
        status, data, _ = call('POST', '/api/v1/system/reset', {'confirm': label}, cookie=kept, csrf=kept_csrf)
        assert status == 200 and reset_called.exists(), data
        reset_called.unlink()
        # The button: stuck pressed since the start, it must never have fired, even
        # after more than 5 s; a short press neither; 5 s after a release, yes.
        time.sleep(5.5)
        assert not reset_called.exists(), 'a button stuck from the start must not reset'
        button.write_text('1\n'); time.sleep(0.6)
        button.write_text('0\n'); time.sleep(2)
        button.write_text('1\n'); time.sleep(0.6)
        assert not reset_called.exists(), 'a short press must not reset'
        button.write_text('0\n'); time.sleep(5.8)
        assert reset_called.exists(), 'holding the button for 5 s must reset'
        # A second device has no trial id; the camera acts on the one running trial.
        assert call('PUT', '/api/v1/network/wifi', {'id': 'current'}, cookie=kept, csrf=kept_csrf)[0] == 200
        # Sign-out ends a remembered sign-in for good.
        assert call('DELETE', '/api/v1/session', cookie=kept, csrf=kept_csrf)[0] == 204
        assert not db.exists()
        daemon.terminate(); daemon.wait(timeout=10); daemon = start()
        assert call('GET', '/api/v1/session', cookie=kept)[0] == 401
        # A new password ends every remembered sign-in.
        kept, kept_csrf, _, _ = sign_in(True)
        assert db.exists()
        assert call('POST', '/api/v1/setup/password', {'old_password': 'admin', 'new_password': 'second'},
                    cookie=kept, csrf=kept_csrf)[0] == 200
        assert not db.exists() and call('GET', '/api/v1/session', cookie=kept)[0] == 401
        print('sessions and system routes: PASS')
    finally:
        daemon.terminate(); daemon.wait(timeout=10)
