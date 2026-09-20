#!/usr/bin/env python3
import concurrent.futures, http.client, os, pathlib, socket, ssl, subprocess, sys, tempfile, threading, time

BIN=pathlib.Path(sys.argv[1]).resolve()
ROOT=pathlib.Path(__file__).resolve().parents[2]
HELPER=ROOT/'tools/tests/fake-integration.sh'
context=ssl.create_default_context();context.check_hostname=False;context.verify_mode=ssl.CERT_NONE

def https_get(path):
    connection=http.client.HTTPSConnection('127.0.0.1',18443,context=context,timeout=5)
    connection.request('GET',path)
    response=connection.getresponse();response.read();connection.close();return response.status

with tempfile.TemporaryDirectory() as state,tempfile.TemporaryDirectory() as staging:
    env=os.environ|{'JOAN_STATE_DIR':state,'JOAN_STAGING_DIR':staging,'JOAN_WEB_DIR':str(ROOT/'web'),'JOAN_INTEGRATION_HELPER':str(HELPER),'JOAN_PORT':'18443','JOAN_REDIRECT_PORT':'0','JOAN_MQTT_PORT':'0','JOAN_RTSP_PORT':'18555','JOAN_MDNS':'0'}
    process=subprocess.Popen([BIN,'--bind','127.0.0.1'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            try:
                if https_get('/api/health')==200:break
            except OSError:time.sleep(.05)
        else:raise AssertionError('TLS daemon did not listen')

        slow=[]
        for _ in range(4):slow.append(socket.create_connection(('127.0.0.1',18443),2))
        result=[]
        def queued_tls():
            raw=socket.create_connection(('127.0.0.1',18443),2)
            with context.wrap_socket(raw,server_hostname='camera.local') as tls:
                tls.sendall(b'GET /api/health HTTP/1.1\r\nHost: 127.0.0.1:18443\r\n\r\n')
                result.append(tls.recv(512))
        queued=threading.Thread(target=queued_tls);queued.start();time.sleep(.25)
        assert queued.is_alive(),'fifth TLS request was not backpressured'
        slow.pop().close();queued.join(4);assert not queued.is_alive() and result and b'HTTP/1.1 200 ' in result[0]
        for connection in slow:connection.close()
        time.sleep(.1)

        paths=['/','/styles.css','/app.js','/icon.svg','/manifest.webmanifest','/sw.js','/api/v1/setup/status','/api/v1/session']
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(paths)) as pool:
            statuses=list(pool.map(https_get,paths))
        assert statuses[:-1]==[200]*(len(paths)-1) and statuses[-1]==401,statuses
        print('PASS: queued TLS overflow and parallel initial assets/API')
    finally:
        process.terminate()
        try:process.wait(3)
        except subprocess.TimeoutExpired:process.kill()
