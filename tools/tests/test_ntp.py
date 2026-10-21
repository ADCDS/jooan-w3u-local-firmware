#!/usr/bin/env python3
"""Network time: configuration, gateway resolution, forged replies and steps.

Runs the host-built daemon (src/daemon/joan-daemon) against a loopback SNTP
server. Clock steps reach only the fake integration helper, which records the
requested epoch: the host clock is never changed. Never touches a camera.
"""
import http.client, json, os, pathlib, socket, struct, subprocess, tempfile, threading, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BIN = ROOT / 'src/daemon/joan-daemon'
HELPER = ROOT / 'tools/tests/fake-integration.sh'
PORT = 18093


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


class Sntp:
    """Answers with this host's clock plus `offset`, after a forged reply."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.offset = 0.0
        self.requests = self.forged = 0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    @staticmethod
    def stamp(t):
        return struct.pack('!II', (int(t) + 2208988800) & 0xffffffff, int((t % 1) * 2**32))

    def serve(self):
        while not self.stop.is_set():
            try:
                packet, peer = self.sock.recvfrom(512)
            except socket.timeout:
                continue
            if len(packet) < 48 or packet[0] & 7 != 3:
                continue
            self.requests += 1
            received = self.stamp(time.time() + self.offset)
            self.sock.sendto(bytes([0x24, 2]) + bytes(30) + received + self.stamp(time.time() + self.offset), peer)
            self.forged += 1
            self.sock.sendto(bytes([0x24, 2]) + bytes(22) + packet[40:48] + received
                             + self.stamp(time.time() + self.offset), peer)

    def close(self):
        self.stop.set(); self.thread.join(); self.sock.close()


def wait_for(cookie, predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while True:
        ntp = json.loads(request('GET', '/api/v1/time', cookie=cookie)[2])['ntp']
        if predicate(ntp) or time.monotonic() > deadline:
            return ntp
        time.sleep(0.1)


with tempfile.TemporaryDirectory() as state, tempfile.TemporaryDirectory() as staging, \
        tempfile.TemporaryDirectory() as run:
    log = pathlib.Path(run) / 'helper.log'
    pathlib.Path(run, 'default-gateway').write_text('127.0.0.1 lo\n')
    sntp = Sntp()
    env = os.environ | {
        'JOAN_STATE_DIR': state, 'JOAN_STAGING_DIR': staging, 'JOAN_RUN_DIR': run,
        'JOAN_WEB_DIR': str(ROOT / 'web'), 'JOAN_INTEGRATION_HELPER': str(HELPER),
        'JOAN_MQTT_PORT': '0', 'JOAN_MDNS': '0', 'FAKE_HELPER_LOG': str(log),
    }
    daemon = subprocess.Popen([BIN, '--plain-mqtt', '--bind', '127.0.0.1', '--port', str(PORT)],
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
        cookie = headers['Set-Cookie'].split(';', 1)[0]
        csrf = json.loads(body)['csrf']

        # Media belongs to the media daemon now.
        for path in ('/api/v1/streams', '/api/v1/system/audio-pause'):
            assert request('GET', path, cookie=cookie)[0] == 404, path
        assert request('POST', '/onvif/device_service', '<Envelope/>')[0] == 404
        info = json.loads(request('GET', '/api/v1/status', cookie=cookie)[2])
        assert set(info['features']) == {'ssh', 'mdns', 'media'} and info['features']['media'] == 'external', info
        assert info['ntp'] == {'server': '', 'synchronized': False}, info['ntp']

        # Off by default; only gateway or IPv4[:port] are accepted.
        assert json.loads(request('GET', '/api/v1/time', cookie=cookie)[2])['ntp']['server'] == ''
        for bad in ('pool.ntp.org', '0.0.0.0', '10.0.0.1:0', '10.0.0.1:x', ' gateway'):
            assert request('PUT', '/api/v1/time', {'ntp': bad}, cookie, csrf)[0] == 400, bad
        assert request('PUT', '/api/v1/time', {'ntp': 'gateway'}, cookie)[0] == 403  # CSRF required

        # gateway follows the supervisor's default route (here loopback, port 123: no server).
        assert request('PUT', '/api/v1/time', {'ntp': 'gateway'}, cookie, csrf)[0] == 200
        ntp = wait_for(cookie, lambda n: n.get('attempts', 0) >= 1)
        assert ntp['server'] == 'gateway' and ntp['resolved'] == '127.0.0.1' and not ntp['synchronized'] and ntp['error'], ntp
        assert (pathlib.Path(state) / 'ntp-server').read_text() == 'gateway\n'

        # Never step while an OEM jooanipc process exists: it dies mid-bootstrap.
        sntp.offset = 100.0
        server = f'127.0.0.1:{sntp.port}'
        oem = pathlib.Path(run) / 'jooanipc'
        oem.write_bytes(pathlib.Path('/bin/sleep').read_bytes()); oem.chmod(0o755)
        fake = subprocess.Popen([str(oem), '60'])
        try:
            assert request('PUT', '/api/v1/time', {'ntp': server}, cookie, csrf)[0] == 200
            ntp = wait_for(cookie, lambda n: n.get('attempts', 0) >= 1)
            assert not ntp['synchronized'] and 'OEM application' in ntp['error'], ntp
            assert not log.exists() or 'time-set' not in log.read_text()
        finally:
            fake.kill(); fake.wait()

        # A server 100 s ahead, preceded by a forged reply: one step via the helper.
        before = time.time()
        assert request('PUT', '/api/v1/time', {'ntp': server}, cookie, csrf)[0] == 200
        ntp = wait_for(cookie, lambda n: n.get('synchronized'))
        assert ntp['synchronized'] and ntp['error'] == '' and abs(ntp['last_offset_ms'] - 100000) < 1000, ntp
        assert sntp.forged >= 1
        steps = [int(x.split()[1]) for x in log.read_text().splitlines() if x.startswith('time-set ')]
        assert len(steps) == 1 and abs(steps[0] - (before + 100)) <= 3, (steps, before)
        assert abs(time.time() - before) < 30, 'the host clock must not move'

        # Below the step threshold: synchronized without another time-set.
        sntp.offset = 0.5
        assert request('PUT', '/api/v1/time', {'ntp': server}, cookie, csrf)[0] == 200
        ntp = wait_for(cookie, lambda n: n.get('synchronized'))
        assert ntp['synchronized'] and abs(ntp['last_offset_ms'] - 500) < 400, ntp
        assert sum(x.startswith('time-set ') for x in log.read_text().splitlines()) == 1

        # The status endpoint carries the same report; empty switches it off.
        assert json.loads(request('GET', '/api/v1/status', cookie=cookie)[2])['ntp']['server'] == server
        assert request('PUT', '/api/v1/time', {'ntp': ''}, cookie, csrf)[0] == 200
        assert json.loads(request('GET', '/api/v1/time', cookie=cookie)[2])['ntp']['server'] == ''
        assert not (pathlib.Path(state) / 'ntp-server').exists()
        print(f'ntp: removed media routes, validation, gateway, OEM gate, forged reply, step and no-step cases PASS '
              f'({sntp.requests} requests)')
    finally:
        daemon.terminate()
        try:
            daemon.wait(3)
        except subprocess.TimeoutExpired:
            daemon.kill()
        sntp.close()
