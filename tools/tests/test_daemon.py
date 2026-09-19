#!/usr/bin/env python3
import hashlib, http.client, json, os, pathlib, socket, subprocess, tempfile, time

ROOT=pathlib.Path(__file__).resolve().parents[2]
BIN=ROOT/'src/daemon/joan-daemon'
HELPER=ROOT/'tools/tests/fake-integration.sh'

def request(method,path,body=None,cookie='',csrf='',ctype='application/json'):
    c=http.client.HTTPConnection('127.0.0.1',18081,timeout=4)
    headers={'Content-Type':ctype}
    if cookie: headers['Cookie']=cookie
    if csrf: headers['X-CSRF-Token']=csrf
    if isinstance(body,dict): body=json.dumps(body)
    c.request(method,path,body=body,headers=headers)
    r=c.getresponse(); data=r.read(); return r.status,dict(r.getheaders()),data

with tempfile.TemporaryDirectory() as state:
    env=os.environ|{'JOAN_STATE_DIR':state,'JOAN_WEB_DIR':str(ROOT/'web'),
      'JOAN_INTEGRATION_HELPER':str(HELPER),'JOAN_MQTT_PORT':'18883','JOAN_MDNS':'0'}
    p=subprocess.Popen([BIN,'--plain-http','--bind','127.0.0.1','--port','18081'],env=env,
                       stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        for _ in range(50):
            try:
                if request('GET','/api/health')[0]==200: break
            except OSError: time.sleep(.05)
        else: raise AssertionError('daemon did not listen')
        ver,rounds,salt,want,must=(pathlib.Path(state)/'auth.db').read_text().strip().split(':')
        assert ver=='v1' and must=='1'
        assert hashlib.pbkdf2_hmac('sha256',b'change-me-now',bytes.fromhex(salt),int(rounds)).hex()==want  # hygiene: allow-test-vector
        assert json.loads(request('GET','/api/v1/setup/status')[2])['setup_required'] is True
        status,h,b=request('POST','/api/v1/session',{'username':'admin','password':'change-me-now'})  # hygiene: allow-test-vector
        assert status==200,(status,b)
        cookie=h['Set-Cookie'].split(';',1)[0]; csrf=json.loads(b)['csrf']
        assert request('GET','/api/v1/status',cookie=cookie)[0]==200
        assert request('GET','/api/v1/streams',cookie=cookie)[0]==428
        assert request('POST','/api/v1/setup/password',{'old_password':'change-me-now','new_password':'correct horse battery staple'},cookie,csrf)[0]==200  # hygiene: allow-test-vector
        status,h,b=request('POST','/api/v1/session',{'username':'admin','password':'correct horse battery staple'})
        assert status==200
        cookie=h['Set-Cookie'].split(';',1)[0]; csrf=json.loads(b)['csrf']
        assert request('GET','/api/v1/streams',cookie=cookie)[0]==200
        status,_,b=request('GET','/api/v1/network/mdns',cookie=cookie)
        assert status==200 and json.loads(b)['hostname']=='camera'
        status,_,b=request('PUT','/api/v1/network/mdns',{'hostname':'Nursery-Cam'},cookie,csrf)
        assert status==200 and json.loads(b)['address']=='nursery-cam.local'
        assert (pathlib.Path(state)/'hostname').read_text()=='nursery-cam\n'
        assert request('PUT','/api/v1/network/mdns',{'hostname':'bad.name'},cookie,csrf)[0]==400
        assert request('POST','/api/v1/network/wifi',{'ssid':'lab'},cookie,csrf)[0]==200
        m=socket.create_connection(('127.0.0.1',18883),2)
        m.sendall(b'\x10\x0c\x00\x04MQTT\x04\x02\x00\x1e\x00\x00')
        assert m.recv(4)==b'\x20\x02\x00\x00';m.close()
        print('PASS: auth/bootstrap/CSRF/helper/MQTT smoke test')
    finally:
        p.terminate()
        try:p.wait(3)
        except subprocess.TimeoutExpired:p.kill()
