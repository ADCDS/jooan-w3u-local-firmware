#!/usr/bin/env python3
import concurrent.futures, http.client, os, pathlib, socket, subprocess, sys, tempfile, threading, time

BIN=pathlib.Path(sys.argv[1]).resolve()
ROOT=pathlib.Path(__file__).resolve().parents[2]
HELPER=ROOT/'tools/tests/fake-integration.sh'
PORT=18480

def http_get(path):
    connection=http.client.HTTPConnection('127.0.0.1',PORT,timeout=5)
    connection.request('GET',path)
    response=connection.getresponse();response.read();connection.close();return response.status

with tempfile.TemporaryDirectory() as state,tempfile.TemporaryDirectory() as staging:
    env=os.environ|{'JOAN_STATE_DIR':state,'JOAN_STAGING_DIR':staging,'JOAN_WEB_DIR':str(ROOT/'web'),'JOAN_INTEGRATION_HELPER':str(HELPER),'JOAN_PORT':str(PORT),'JOAN_MQTT_PORT':'0','JOAN_RTSP_PORT':'18555','JOAN_MDNS':'0'}
    process=subprocess.Popen([BIN,'--plain-mqtt','--bind','127.0.0.1'],env=env,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try:
                if http_get('/api/health')==200:break
            except OSError:time.sleep(.05)
        else:raise AssertionError('daemon did not listen')
        assert process.poll() is None,'daemon exited: port in use?'

        slow=[]
        for _ in range(8):slow.append(socket.create_connection(('127.0.0.1',PORT),2))
        result=[]
        def queued_request():
            with socket.create_connection(('127.0.0.1',PORT),2) as raw:
                raw.settimeout(5)
                raw.sendall(b'GET /api/health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n')
                result.append(raw.recv(512))
        queued=threading.Thread(target=queued_request);queued.start();time.sleep(.25)
        assert queued.is_alive(),'ninth request was not backpressured'
        slow.pop().close();queued.join(4);assert not queued.is_alive() and result and b'HTTP/1.1 200 ' in result[0]
        for connection in slow:connection.close()
        time.sleep(.1)

        paths=['/','/styles.css','/app.js','/api/v1/setup/status','/api/v1/session']
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(paths)) as pool:
            statuses=list(pool.map(http_get,paths))
        assert statuses[:-1]==[200]*(len(paths)-1) and statuses[-1]==401,statuses

        body=b'{"username":"admin","password":"admin"}'
        headers=(b'POST /api/v1/session HTTP/1.1\r\n'
                 b'Host: 127.0.0.1\r\n'
                 b'Origin: http://127.0.0.1\r\n'
                 b'Content-Type: application/json\r\n'
                 b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n')
        with socket.create_connection(('127.0.0.1',PORT),2) as raw:
            raw.sendall(headers)
            time.sleep(.1)
            raw.sendall(body[:7])
            time.sleep(.1)
            raw.sendall(body[7:])
            response=b''
            while b'\r\n\r\n' not in response:response+=raw.recv(1024)
            assert b'HTTP/1.1 200 ' in response,response
        print('PASS: queued overflow and parallel initial assets/API')
    finally:
        process.terminate()
        try:process.wait(3)
        except subprocess.TimeoutExpired:process.kill()
