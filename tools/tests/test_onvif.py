#!/usr/bin/env python3
"""ONVIF device/media/PTZ subset, end to end through the host-built daemon.

Starts src/daemon/joan-daemon (built against mbedTLS; see ci/check_onvif.sh)
with a fake jooanipc: an MQTT client that keeps a preset table the way the
OEM does, and the OEM's loopback ONVIF PTZ service, which the daemon drives
the motor through, logging each jog and stop. Requests are raw SOAP 1.2 with
WS-Security digests, written the way zeep (Frigate, Home Assistant) writes
them. Never touches a camera.

The fake camera is importable, so a client library can be pointed at it:
    with FakeCamera(bin, tls=True) as cam: ... cam.port ...
"""
import base64, datetime, hashlib, http.client, json, os, pathlib, re, socket, ssl
import struct, subprocess, sys, tempfile, threading, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
PASSWORD = 'admin'  # the public initial password of a fresh state dir


def mqtt_read(sock):
    head = sock.recv(1)
    if not head:
        return None, b''
    remaining, mult = 0, 1
    while True:
        c = sock.recv(1)[0]
        remaining += (c & 127) * mult
        if not c & 128:
            break
        mult *= 128
    data = b''
    while len(data) < remaining:
        chunk = sock.recv(remaining - len(data))
        if not chunk:
            return None, b''
        data += chunk
    return head[0], data


def mqtt_publish(topic, payload):
    body = struct.pack('!H', len(topic)) + topic + payload
    n, length = len(body), b''
    while True:
        byte, n = n & 127, n >> 7
        length += bytes([byte | (128 if n else 0)])
        if not n:
            return b'\x30' + length + body


class FakeCamera:
    """joan-daemon plus a fake jooanipc on the loopback MQTT sink."""

    def __init__(self, binary, tls=False, port=18095, mqtt_port=18893, motor_port=18899):
        self.binary, self.tls, self.port, self.mqtt_port, self.motor_port = binary, tls, port, mqtt_port, motor_port
        self.presets = {0: 'RUA BAIXO', 1: 'RUA CIMA', 2: 'PORTAO & GATE'}
        self.commands = []
        self.motor = []          # (monotonic time, 'ContinuousMove' | 'Stop', x, y)
        self.motor_refuse = []   # operations to answer with a SOAP fault, once each

    def __enter__(self):
        self.dir = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.dir.name)
        (base / 'state').mkdir()
        (base / 'staging').mkdir()
        self.motor_server = socket.create_server(('127.0.0.1', self.motor_port))
        threading.Thread(target=self._motor, daemon=True).start()
        env = os.environ | {
            'JOAN_STATE_DIR': str(base / 'state'), 'JOAN_STAGING_DIR': str(base / 'staging'),
            'JOAN_WEB_DIR': str(ROOT / 'web'), 'JOAN_INTEGRATION_HELPER': str(ROOT / 'tools/tests/fake-integration.sh'),
            'JOAN_OEM_ONVIF_PORT': str(self.motor_port),
            'JOAN_PORT': str(self.port), 'JOAN_REDIRECT_PORT': '0', 'JOAN_MQTT_PORT': str(self.mqtt_port),
            'JOAN_CONNECTIVITY_PORT': '0', 'JOAN_RTSP_PORT': '18596', 'JOAN_MDNS': '0',
        }
        args = [str(self.binary), '--bind', '127.0.0.1'] + ([] if self.tls else ['--plain-http'])
        self.daemon = subprocess.Popen(args, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self._connect()
        except BaseException:
            self.__exit__(None, None, None)
            raise
        threading.Thread(target=self._device, daemon=True).start()
        return self

    def _connect(self):
        for _ in range(200):
            try:
                socket.create_connection(('127.0.0.1', self.port), 0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError('daemon did not listen')
        time.sleep(0.1)
        # Something else on the port (a leftover daemon) would answer instead.
        assert self.daemon.poll() is None, 'daemon exited: port in use?'
        self.mqtt = socket.create_connection(('127.0.0.2', self.mqtt_port), 2)
        if self.tls:  # the HTTPS daemon's MQTT sink is TLS too, as jooanipc expects
            context = ssl.create_default_context()
            context.check_hostname, context.verify_mode = False, ssl.CERT_NONE
            self.mqtt = context.wrap_socket(self.mqtt)
        self.mqtt.sendall(b'\x10\x0c\x00\x04MQTT\x04\x02\x00\x1e\x00\x00')
        assert self.mqtt.recv(4) == b'\x20\x02\x00\x00'
        topic = b'qaiot/mqtt/device/command'
        sub = b'\x00\x01' + struct.pack('!H', len(topic)) + topic + b'\x00'
        self.mqtt.sendall(b'\x82' + bytes([len(sub)]) + sub)
        assert self.mqtt.recv(5) == b'\x90\x03\x00\x01\x00'
        self.mqtt.settimeout(None)

    def __exit__(self, *exc):
        self.daemon.terminate()
        try:
            self.daemon.wait(3)
        except subprocess.TimeoutExpired:
            self.daemon.kill()
        if getattr(self, 'mqtt', None):
            self.mqtt.close()
        self.motor_server.close()
        self.dir.cleanup()

    def _motor(self):
        """jooanipc's ONVIF PTZ service: it only parses its own literal prefixes."""
        while True:
            try:
                conn, _ = self.motor_server.accept()
            except OSError:
                return
            with conn:
                data = b''
                while b'</s:Envelope>' not in data:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                text = data.decode()
                op = 'ContinuousMove' if '<tptz:ContinuousMove>' in text else 'Stop' if '<tptz:Stop>' in text else '?'
                move = re.search(r'<tt:PanTilt x="([^"]*)" y="([^"]*)"/>', text)
                self.motor.append((time.monotonic(), op) + (move.groups() if move else ()))
                if op in self.motor_refuse:
                    self.motor_refuse.remove(op)
                    conn.sendall(b'HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n<env:Fault></env:Fault>')
                else:
                    conn.sendall(b'HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n<env:Body><tptz:%sResponse></tptz:%sResponse></env:Body>'
                                 % (op.encode(), op.encode()))

    def _device(self):
        while True:
            try:
                kind, data = mqtt_read(self.mqtt)
            except OSError:
                return
            if kind is None:
                return
            if kind >> 4 != 3:
                continue
            topic_len = struct.unpack('!H', data[:2])[0]
            request = json.loads(data[2 + topic_len:])
            self.commands.append(request)
            reply = {'cmd': request['cmd'], 'cmd_type': 'response', 'status': 0}
            cmd = request['cmd']
            if cmd == 66486:
                reply['ptz_coordinate'] = [{'coordinateID': k, 'name': v} for k, v in sorted(self.presets.items())]
            elif cmd == 66485:
                free = [i for i in range(6) if i not in self.presets]
                if free:
                    self.presets[free[0]] = request['name']
                    reply |= {'name': request['name'], 'coordinateID': free[0]}
                else:
                    reply['status'] = 1
            elif cmd == 66489:
                self.presets[request['coordinateID']] = request['name']
            elif cmd == 66490:
                self.presets.pop(request['ptz_coordinate'][0]['coordinateID'], None)
            # jooanipc prints compact JSON (cJSON unformatted), as the daemon expects.
            self.mqtt.sendall(mqtt_publish(b'qaiot/mqtt/user/device/reply', json.dumps(reply, separators=(',', ':'), ensure_ascii=False).encode()))



def envelope(operation, body='', namespace='http://www.onvif.org/ver20/ptz/wsdl', password=PASSWORD,
             user='admin', auth=True, created=None, nonce=None):
    """A request shaped like zeep's: soap-env/ns0 prefixes, WS-Security first."""
    security = ''
    if auth:
        nonce = nonce or os.urandom(16)
        created = created or datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')
        digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + password.encode()).digest()).decode()
        security = (
            '<wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">'
            f'<wsse:UsernameToken><wsse:Username>{user}</wsse:Username>'
            '<wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">'
            f'{digest}</wsse:Password><wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">'
            f'{base64.b64encode(nonce).decode()}</wsse:Nonce>'
            '<wsu:Created xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">'
            f'{created}</wsu:Created></wsse:UsernameToken></wsse:Security>')
    return ("<?xml version='1.0' encoding='utf-8'?>"
            '<soap-env:Envelope xmlns:soap-env="http://www.w3.org/2003/05/soap-envelope">'
            f'<soap-env:Header>{security}</soap-env:Header><soap-env:Body>'
            f'<ns0:{operation} xmlns:ns0="{namespace}">{body}</ns0:{operation}>'
            '</soap-env:Body></soap-env:Envelope>')


class Client:
    def __init__(self, cam):
        self.cam = cam
        self.context = ssl.create_default_context()
        self.context.check_hostname = False
        self.context.verify_mode = ssl.CERT_NONE

    def post(self, path, xml, headers=None):
        if self.cam.tls:
            c = http.client.HTTPSConnection('127.0.0.1', self.cam.port, context=self.context, timeout=15)
        else:
            c = http.client.HTTPConnection('127.0.0.1', self.cam.port, timeout=15)
        c.request('POST', path, body=xml.encode(), headers={'Content-Type': 'application/soap+xml; charset=utf-8', **(headers or {})})
        r = c.getresponse()
        data = r.read().decode()
        c.close()
        return r.status, data

    def ptz(self, operation, body='', **kw):
        return self.post('/onvif/ptz', envelope(operation, body, **kw))


def main(binary):
    device_ns = 'http://www.onvif.org/ver10/device/wsdl'
    media_ns = 'http://www.onvif.org/ver10/media/wsdl'
    with FakeCamera(binary) as cam:
        client = Client(cam)
        base = f'http://127.0.0.1:{cam.port}'

        # Time is public, so clients can correct their token timestamps.
        status, xml = client.post('/onvif/device_service', envelope('GetSystemDateAndTime', namespace=device_ns, auth=False))
        assert status == 200 and '<tt:UTCDateTime>' in xml, xml

        # Everything else needs the administrator's digest.
        status, xml = client.post('/onvif/device_service', envelope('GetCapabilities', '<ns0:Category>All</ns0:Category>', device_ns, auth=False))
        assert status == 400 and 'ter:NotAuthorized' in xml, xml
        status, xml = client.post('/onvif/device_service', envelope('GetCapabilities', namespace=device_ns, password='wrong'))
        assert status == 400 and 'ter:NotAuthorized' in xml, xml
        replayed = envelope('GetCapabilities', namespace=device_ns)
        status, xml = client.post('/onvif/device_service', replayed)
        assert status == 200, xml
        for service in ('device_service', 'media', 'ptz'):
            assert f'<tt:XAddr>{base}/onvif/{service}</tt:XAddr>' in xml, xml
        assert client.post('/onvif/device_service', replayed)[0] == 400, 'a token must not work twice'

        # Prefixes are the client's choice: none at all must work too.
        plain = envelope('GetProfiles', namespace=media_ns).replace('ns0:', '').replace('xmlns:ns0=', 'xmlns=')
        status, xml = client.post('/onvif/media', plain)
        assert status == 200 and xml.count('<trt:Profiles ') == 1, xml
        assert '<tt:PTZConfiguration token="ptz0">' in xml and '<tt:Encoding>H264</tt:Encoding>' in xml, 'sensor A, on the head'
        assert 'VelocityGenericSpace' in xml and 'Zoom' not in xml and 'Translation' not in xml, 'no zoom or relative moves'

        # A foreign page cannot drive the camera through the browser.
        assert client.post('/onvif/ptz', envelope('Stop'), {'Origin': 'http://evil.example'})[0] == 403

        # Frigate does not call the rest; the flash budget keeps them out.
        for operation in ('AbsoluteMove', 'GetStreamUri', 'GetServiceCapabilities'):
            status, xml = client.ptz(operation)
            assert status == 500 and 'ter:ActionNotSupported' in xml, (operation, xml)

        # A press is one coarse nudge the daemon times itself (the client's
        # Stop is not waited for), on the dominant axis, via the OEM service.
        started = time.monotonic()
        status, xml = client.ptz('ContinuousMove', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:Velocity><ns0:PanTilt x="-0.5" y="0.2"/></ns0:Velocity>')
        assert status == 200 and time.monotonic() - started >= 0.35, xml
        assert [m[1:] for m in cam.motor] == [('ContinuousMove', '-0.6', '0.0'), ('Stop',)], cam.motor
        assert 0.35 <= cam.motor[1][0] - cam.motor[0][0] < 0.5, cam.motor
        assert client.ptz('Stop', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PanTilt>true</ns0:PanTilt>')[0] == 200
        assert len(cam.motor) == 2, 'Stop has nothing left to stop'
        assert client.ptz('ContinuousMove', '<ns0:Velocity><ns0:PanTilt x="0.1" y="1"/></ns0:Velocity>')[0] == 200
        assert cam.motor[2][1:] == ('ContinuousMove', '0.0', '0.6'), cam.motor
        # An unconfirmed Stop blocks further moves and is retried until it holds.
        cam.motor_refuse = ['Stop']
        cam.motor.clear()
        status, xml = client.ptz('ContinuousMove', '<ns0:Velocity><ns0:PanTilt x="0" y="-1"/></ns0:Velocity>')
        assert status == 500 and 'did not confirm' in xml, xml
        status, xml = client.ptz('ContinuousMove', '<ns0:Velocity><ns0:PanTilt x="0" y="-1"/></ns0:Velocity>')
        assert status == 500 and 'pending' in xml, 'no move while a stop is unconfirmed'
        time.sleep(1.6)
        assert [m[1] for m in cam.motor] == ['ContinuousMove', 'Stop', 'Stop'], cam.motor
        # A refused move is stopped too, in case the OEM started it anyway.
        cam.motor_refuse = ['ContinuousMove']
        cam.motor.clear()
        assert client.ptz('ContinuousMove', '<ns0:Velocity><ns0:PanTilt x="1" y="0"/></ns0:Velocity>')[0] == 500
        time.sleep(0.6)
        assert [m[1] for m in cam.motor] == ['ContinuousMove', 'Stop'], cam.motor

        status, xml = client.ptz('GetPresets', '<ns0:ProfileToken>ch0</ns0:ProfileToken>')
        assert status == 200 and '<tptz:Preset token="2"><tt:Name>PORTAO &amp; GATE</tt:Name></tptz:Preset>' in xml, xml
        status, xml = client.ptz('SetPreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetName>Rua &amp; Portão</ns0:PresetName>')
        assert status == 200 and '<tptz:PresetToken>3</tptz:PresetToken>' in xml and cam.presets[3] == 'Rua & Portão', xml
        status, xml = client.ptz('SetPreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetToken>3</ns0:PresetToken><ns0:PresetName>RUA</ns0:PresetName>')
        assert status == 200 and cam.commands[-1]['cmd'] == 66489 and cam.presets[3] == 'RUA', xml
        status, xml = client.ptz('SetPreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetName>say "hi"</ns0:PresetName>')
        assert status == 400 and 'InvalidArgVal' in xml, 'a quote cannot reach the OEM JSON'
        status, xml = client.ptz('RemovePreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetToken>9</ns0:PresetToken>')
        assert status == 400 and 'InvalidArgVal' in xml, xml
        status, xml = client.ptz('RemovePreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetToken>3</ns0:PresetToken>')
        assert status == 200 and 3 not in cam.presets, xml

        # A goto opens the travel window: nothing else may move the head.
        status, xml = client.ptz('GotoPreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetToken>2</ns0:PresetToken>')
        assert status == 200 and cam.commands[-1] == {'cmd': 66491, 'cmd_type': 'request', 'coordinateID': 2, 'mot_index': 0}, xml
        cam.motor.clear()
        status, xml = client.ptz('ContinuousMove', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:Velocity><ns0:PanTilt x="0.5" y="0"/></ns0:Velocity>')
        assert status == 500 and 'pending' in xml, xml
        assert client.ptz('GotoPreset', '<ns0:ProfileToken>ch0</ns0:ProfileToken><ns0:PresetToken>1</ns0:PresetToken>')[0] == 500
        assert not cam.motor, 'nothing moved during travel'

        # Wrong passwords share the Web login's per-address budget.
        for _ in range(5):
            client.post('/onvif/device_service', envelope('GetCapabilities', namespace=device_ns, password='guess'))
        assert client.post('/onvif/device_service', envelope('GetCapabilities', namespace=device_ns))[0] == 400
    print('onvif: auth/replay/prefixes/origin/profile/nudge/stop-retry/presets/travel/throttle PASS')


if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'src/daemon/joan-daemon').resolve())
