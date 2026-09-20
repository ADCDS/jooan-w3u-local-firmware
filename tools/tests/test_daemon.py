#!/usr/bin/env python3
import base64, ctypes, ctypes.util, hashlib, http.client, json, os, pathlib, re, socket, struct, subprocess, tempfile, threading, time
ROOT=pathlib.Path(__file__).resolve().parents[2];BIN=ROOT/'src/daemon/joan-daemon';HELPER=ROOT/'tools/tests/fake-integration.sh'
SPS=base64.b64decode('Z2QAKKzZQFAFuhAAAAMAEAAAAwDxgxHg');PPS=base64.b64decode('aO48gA==')
LIBCRYPT=ctypes.CDLL(ctypes.util.find_library('crypt'));LIBCRYPT.crypt.argtypes=(ctypes.c_char_p,ctypes.c_char_p);LIBCRYPT.crypt.restype=ctypes.c_char_p
def crypt_verify(password,hashed):return LIBCRYPT.crypt(password.encode(),hashed.encode()).decode()==hashed
def request(method,path,body=None,cookie='',csrf='',ctype='application/json',origin=''):
 c=http.client.HTTPConnection('127.0.0.1',18081,timeout=12);h={'Content-Type':ctype}
 if cookie:h['Cookie']=cookie
 if csrf:h['X-CSRF-Token']=csrf
 if origin:h['Origin']=origin
 if isinstance(body,dict):body=json.dumps(body)
 c.request(method,path,body=body,headers=h);r=c.getresponse();data=r.read();return r.status,dict(r.getheaders()),data
def rtp(seq,stamp,nal):
 p=struct.pack('!BBHII',0x80,0x80|96,seq,stamp,0x12345678)+nal;return b'$\x00'+struct.pack('!H',len(p))+p
def mqtt_packet(sock):
 h=sock.recv(1);assert h;remaining=0;mult=1
 while True:
  c=sock.recv(1)[0];remaining+=(c&127)*mult
  if not c&128:break
  mult*=128
 data=b''
 while len(data)<remaining:data+=sock.recv(remaining-len(data))
 return h[0],data
def websocket_status(cookie,protocol):
 s=socket.create_connection(('127.0.0.1',18081),2);request=(f'GET /api/v1/audio/mic HTTP/1.1\r\nHost: 127.0.0.1:18081\r\nOrigin: http://127.0.0.1:18081\r\nCookie: {cookie}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: {protocol}\r\n\r\n').encode();s.sendall(request);line=s.recv(128).split(b'\r\n',1)[0];s.close();return int(line.split()[1])
def raw_status(request_bytes):
 s=socket.create_connection(('127.0.0.1',18081),2);s.sendall(request_bytes);line=s.recv(256).split(b'\r\n',1)[0];s.close();return int(line.split()[1])
def rtsp_client(c):
 data=b'';challenged=False
 try:
  while True:
   while b'\r\n\r\n' not in data:
    x=c.recv(4096)
    if not x:return
    data+=x
   raw,data=data.split(b'\r\n\r\n',1);line=raw.split(b'\r\n',1)[0];cseq=b'1'
   for h in raw.split(b'\r\n')[1:]:
    if h.lower().startswith(b'cseq:'):cseq=h.split(b':',1)[1].strip()
   if line.startswith(b'DESCRIBE'):
    if not challenged:
     challenged=True;c.sendall(b'RTSP/1.0 401 Unauthorized\r\nCSeq: '+cseq+b'\r\nWWW-Authenticate: Digest realm="camera", nonce="abcdef", qop="auth"\r\nContent-Length: 0\r\n\r\n');continue
    assert b'Authorization: Digest username="admin"' in raw and b'qop=auth' in raw and b'uri="rtsp://127.0.0.1:18554/live/ch' in raw
    auth=next(h.decode() for h in raw.split(b'\r\n') if h.startswith(b'Authorization:'));fields={m.group(1):m.group(2) or m.group(3) for m in re.finditer(r'(\w+)=(?:"([^"]*)"|([^,\s]+))',auth)};ha1=hashlib.md5(b'admin:camera:change-me-password').hexdigest();ha2=hashlib.md5(f'DESCRIBE:{fields["uri"]}'.encode()).hexdigest();expected=hashlib.md5(f'{ha1}:abcdef:{fields["nc"]}:{fields["cnonce"]}:auth:{ha2}'.encode()).hexdigest();assert fields['response']==expected
    sdp=b'v=0\r\na=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;sprop-parameter-sets='+base64.b64encode(SPS)+b','+base64.b64encode(PPS)+b'\r\na=control:trackID=1\r\nm=audio 0 RTP/AVP 8\r\na=control:trackID=2\r\n'
    channel=b'ch1' if b'/live/ch1 ' in line else b'ch0';c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: '+cseq+b'\r\nContent-Base: rtsp://127.0.0.1:18554/'+channel+b'/\r\nContent-Length: '+str(len(sdp)).encode()+b'\r\n\r\n'+sdp)
   elif line.startswith(b'SETUP'):
    assert b'/ch0/trackID=1 ' in line or b'/ch1/trackID=1 ' in line
    c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: '+cseq+b'\r\nSession: test\r\nTransport: RTP/AVP/TCP;interleaved=0-1\r\n\r\n')
   elif line.startswith(b'PLAY'):
    c.sendall(b'RTSP/1.0 200 OK\r\nCSeq: '+cseq+b'\r\nSession: test\r\n\r\n');time.sleep(.1)
    for seq,stamp,nal in [(1,90000,b'\x65\x88'),(2,96000,b'\x41\x9a'),(3,102000,b'\x65\x99'),(4,108000,b'\x41\xaa'),(5,114000,b'\x65\xbb')]:c.sendall(rtp(seq,stamp,nal));time.sleep(.03)
    time.sleep(3);return
 finally:c.close()
def rtsp_server(stop):
 s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1',18554));s.listen(8);s.settimeout(.2)
 try:
  while not stop.is_set():
   try:c,_=s.accept()
   except socket.timeout:continue
   threading.Thread(target=rtsp_client,args=(c,),daemon=True).start()
 finally:s.close()
stop=threading.Event();rtsp=threading.Thread(target=rtsp_server,args=(stop,),daemon=True);rtsp.start();time.sleep(.1)
with tempfile.TemporaryDirectory() as state,tempfile.TemporaryDirectory() as staging:
 sequence=pathlib.Path(state)/'release-sequence';sequence.write_text('7\n')
 env=os.environ|{'JOAN_STATE_DIR':state,'JOAN_STAGING_DIR':staging,'JOAN_RELEASE_SEQUENCE_PATH':str(sequence),'JOAN_WEB_DIR':str(ROOT/'web'),'JOAN_INTEGRATION_HELPER':str(HELPER),'JOAN_MQTT_PORT':'18883','JOAN_CONNECTIVITY_PORT':'0','JOAN_RTSP_PORT':'18554','JOAN_MDNS':'1','JOAN_MDNS_PORT':'15353'}
 p=subprocess.Popen([BIN,'--plain-http','--bind','127.0.0.1','--port','18081'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 try:
  for _ in range(100):
   try:
    if request('GET','/api/health')[0]==200:break
   except OSError:time.sleep(.05)
  else:raise AssertionError('daemon did not listen')
  ver,rounds,salt,want,warning=(pathlib.Path(state)/'auth.db').read_text().strip().split(':');assert ver=='v1' and warning=='1';assert hashlib.pbkdf2_hmac('sha256',b'change-me-password',bytes.fromhex(salt),int(rounds)).hex()==want
  user,sh=(pathlib.Path(state)/'ssh/passwd').read_text().strip().split(':',1);assert user=='admin' and crypt_verify('change-me-password',sh)
  setup_status,setup_headers,setup_body=request('GET','/api/v1/setup/status');setup=json.loads(setup_body);assert setup_status==200 and setup['setup_required'] is False and setup['default_password_warning'] is True and setup['release_version']=='0.1.0' and setup['release_sequence']==7;assert "media-src 'self' blob:" in setup_headers['Content-Security-Policy']
  login_body=b'{"username":"admin","password":"change-me-password"}'
  assert raw_status(b'POST /api/v1/session HTTP/1.1\r\nhost: 127.0.0.1:18081\r\ncontent-length: '+str(len(login_body)).encode()+b'\r\ncontent-type: application/json\r\n\r\n'+login_body)==200
  smuggled=b'Host: 127.0.0.1:18081\r\nCookie: joan_session=attacker'
  assert raw_status(b'POST /api/v1/session HTTP/1.1\r\nContent-Length: '+str(len(smuggled)).encode()+b'\r\n\r\n'+smuggled)==400
  assert raw_status(b'POST /api/v1/network/mdns HTTP/1.1\r\nHost: 127.0.0.1:18081\r\nContent-Length: 16385\r\n\r\n')==413
  assert raw_status(b'POST /api/v1/update HTTP/1.1\r\nHost: 127.0.0.1:18081\r\nContent-Length: 2097345\r\n\r\n')==413
  idle=socket.create_connection(('127.0.0.1',18081),2);idle.shutdown(socket.SHUT_WR);idle.settimeout(2);assert idle.recv(128)==b'';idle.close()
  slow=[]
  for _ in range(8):
   s=socket.create_connection(('127.0.0.1',18081),2);s.sendall(b'G');slow.append(s)
  time.sleep(.1);overflow=socket.create_connection(('127.0.0.1',18081),2);overflow.sendall(b'GET /api/health HTTP/1.1\r\nHost: 127.0.0.1:18081\r\n\r\n');overflow.settimeout(.2)
  try:overflow.recv(128);raise AssertionError('queued ninth request ran before a worker slot was free')
  except socket.timeout:pass
  slow.pop().close();overflow.settimeout(2);assert b' 200 ' in overflow.recv(256);overflow.close()
  for s in slow:s.close()
  time.sleep(.1)
  status,h,b=request('POST','/api/v1/session',{'username':'admin','password':'change-me-password'});assert status==200,(status,b);cookie=h['Set-Cookie'].split(';',1)[0];csrf=json.loads(b)['csrf'];assert json.loads(b)['default_password_warning'] is True
  resumed=json.loads(request('GET','/api/v1/session',cookie=cookie)[2]);assert resumed['authenticated'] is True and resumed['csrf']==csrf and resumed['default_password_warning'] is True
  assert request('GET','/api/v1/streams',cookie=cookie)[0]==200
  streams=json.loads(request('GET','/api/v1/streams',cookie=cookie)[2])['streams'];assert streams[0]['mime'].endswith('avc1.640032"') and streams[1]['mime'].endswith('avc1.640016"')
  denied=json.loads(request('PUT','/api/v1/network/mdns',{'hostname':'x'},cookie,'wrong')[2]);assert denied['error']['code']=='authentication_required'
  assert request('GET','/api/v1/status',cookie=cookie,origin='https://evil.invalid')[0]==403
  assert request('POST','/api/v1/setup/password',{'old_password':'change-me-password','new_password':'x'*129},cookie,csrf)[0]==400
  assert request('POST','/api/v1/setup/password',{'old_password':'change-me-password','new_password':'twelve-chars\n'},cookie,csrf)[0]==400
  assert request('POST','/api/v1/setup/password',{'old_password':'change-me-password','new_password':'correct horse battery staple'},cookie,csrf)[0]==200
  user,sh=(pathlib.Path(state)/'ssh/passwd').read_text().strip().split(':',1);assert crypt_verify('correct horse battery staple',sh)
  status,h,b=request('POST','/api/v1/session',{'username':'admin','password':'correct horse battery staple'});assert status==200;cookie=h['Set-Cookie'].split(';',1)[0];csrf=json.loads(b)['csrf'];assert json.loads(b)['default_password_warning'] is False
  assert websocket_status(cookie,'jaud.v1')==403
  assert websocket_status(cookie,'jaud.v1, jaud.auth.'+'0'*64)==403
  assert websocket_status(cookie,f'jaud.v1, jaud.auth.{csrf}, jaud.auth.{csrf}')==403
  assert websocket_status(cookie,f'jaud.v1, jaud.auth.{csrf}')==502
  m=socket.create_connection(('127.0.0.2',18883),2);m.sendall(b'\x10\x0c\x00\x04MQTT\x04\x02\x00\x1e\x00\x00');assert m.recv(4)==b'\x20\x02\x00\x00';topic=b'qaiot/mqtt/device/command';sub=b'\x00\x01'+struct.pack('!H',len(topic))+topic+b'\x00';m.sendall(b'\x82'+bytes([len(sub)])+sub);assert m.recv(5)==b'\x90\x03\x00\x01\x00'
  for expected in (66516,66517):
   _,command=mqtt_packet(m);assert str(expected).encode() in command
   response=json.dumps({'cmd':expected,'cmd_type':'response','status':0},separators=(',',':')).encode();reply_topic=b'qaiot/mqtt/user/device/reply'
   if expected==66517:
    bogus=json.dumps({'cmd':expected,'cmd_type':'request','status':0},separators=(',',':')).encode();publish=struct.pack('!H',len(reply_topic))+reply_topic+bogus;m.sendall(b'\x30'+bytes([len(publish)])+publish);time.sleep(.03);assert json.loads(request('GET','/api/v1/status',cookie=cookie)[2])['rtsp_password_sync'] is False
   publish=struct.pack('!H',len(reply_topic))+reply_topic+response;m.sendall(b'\x30'+bytes([len(publish)])+publish)
  time.sleep(.1);assert json.loads(request('GET','/api/v1/status',cookie=cookie)[2])['rtsp_password_sync'] is True
  status,_,b=request('PUT','/api/v1/network/mdns',{'hostname':'Nursery-Cam'},cookie,csrf);assert status==200 and json.loads(b)['address']=='nursery-cam.local'
  md=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);md.settimeout(3);q=b'\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x06_https\x04_tcp\x05local\x00\x00\x0c\x00\x01';md.sendto(q,('127.0.0.1',15353));found=b''
  for _ in range(8):
   try:packet,_=md.recvfrom(1500)
   except socket.timeout:break
   if b'_https' in packet and b'_ssh' in packet and b'_rtsp' in packet and b'_jooan-camera' in packet:found=packet;break
  md.close();assert found and socket.inet_aton('127.0.0.1') not in found,'DNS-SD service announcement missing or advertised peer address'
  assert request('POST','/api/v1/network/wifi',{'ssid':'lab'},cookie,csrf)[0]==200
  assert request('PUT','/api/v1/network/routes',{'cidrs':'10.42.0.0/24, fd00::/64'},cookie,csrf)[0]==200
  assert request('PUT','/api/v1/network/routes',{'cidrs':'0.0.0.0/0'},cookie,csrf)[0]==400
  assert request('PUT','/api/v1/network/routes',{'cidrs':'8.8.8.0/24'},cookie,csrf)[0]==400
  lease=json.loads(request('POST','/api/v1/ptz/lease',{},cookie,csrf)[2])['lease'];assert request('POST','/api/v1/ptz/move',{'lease':lease,'command':'left','duration_ms':250,'speed':3},cookie,csrf)[0]==200;assert request('POST','/api/v1/ptz/move',{'lease':lease,'command':'left','duration_ms':49,'speed':3},cookie,csrf)[0]==400
  assert request('POST','/api/v1/ptz/home',{'action':'set'},cookie,csrf)[0]==202
  _,command=mqtt_packet(m);assert b'66485' in command
  preset=request('GET','/api/v1/ptz/presets',cookie=cookie);assert preset[0]==202;operation=json.loads(preset[2])['operation'];_,command=mqtt_packet(m);assert b'66486' in command
  assert request('GET','/api/v1/ptz/presets',cookie=cookie)[0]==409
  response=b'{"cmd":66486,"cmd_type":"response","status":0,"ptz_coordinate":[]}';telemetry_topic=b'qaiot/mqtt/device/reply';publish=struct.pack('!H',len(telemetry_topic))+telemetry_topic+response;m.sendall(b'\x30'+bytes([len(publish)])+publish);time.sleep(.05);assert json.loads(request('GET',f'/api/v1/operations/{operation}',cookie=cookie)[2])['state']=='pending'
  reply_topic=b'qaiot/mqtt/user/device/reply';not_response=response.replace(b'"response"',b'"request"');publish=struct.pack('!H',len(reply_topic))+reply_topic+not_response;m.sendall(b'\x30'+bytes([len(publish)])+publish);time.sleep(.05);assert json.loads(request('GET',f'/api/v1/operations/{operation}',cookie=cookie)[2])['state']=='pending'
  publish=struct.pack('!H',len(reply_topic))+reply_topic+response;m.sendall(b'\x30'+bytes([len(publish)])+publish);time.sleep(.05)
  completed=json.loads(request('GET',f'/api/v1/operations/{operation}',cookie=cookie)[2]);assert completed['state']=='complete' and completed['response']['cmd']==66486
  init=request('GET','/api/v1/video/main/init.mp4',cookie=cookie);assert init[0]==200 and b'ftyp' in init[2] and b'moov' in init[2]
  frag=request('GET','/api/v1/video/main/fragment.mp4?after=0',cookie=cookie);assert frag[0]==200 and b'moof' in frag[2] and b'mdat' in frag[2] and int(frag[1]['X-Joan-Sequence'])>0
  assert request('GET','/api/v1/snapshot',cookie=cookie)[0]==501
  manifest=json.loads((ROOT/'web/manifest.webmanifest').read_text());assert manifest['display']=='standalone'
  html=(ROOT/'web/index.html').read_text();pattern=re.search(r'name="hostname"[^>]*pattern="([^"]+)"',html).group(1);js=f"const r=new RegExp('^(?:'+{json.dumps(pattern)}+')$','v');const good=['a','camera-1','A1-b',{'a'*63!r}];const bad=['-camera','camera-','bad.name',{'a'*64!r}];if(!good.every(x=>r.test(x))||bad.some(x=>r.test(x)))process.exit(1);";subprocess.run(['node','-e',js],check=True,capture_output=True);assert 'name="username" value="admin" autocomplete="username"' in html
  sw=(ROOT/'web/sw.js').read_text();static=next(line for line in sw.splitlines() if line.startswith('const STATIC='));assert '/api/' not in static and 'audio' not in static and 'video-player.js' in static and "CACHE='jooan-ui-v2'" in sw and "fetch(event.request,{cache:'no-store'})" in sw
  player=(ROOT/'web/video-player.js').read_text();assert 'this.failures >= 3' in player and 'await this.initialize(next)' in player and 'this.sequence = 0' in player and "response.status === 401" in player
  app=(ROOT/'web/app.js').read_text();assert 'r.status===204?null' in app and 'r.status===401' in app and 'requireLogin()' in app and 'if(!ptzHeld)releasePtz()' in app
  audio_client=(ROOT/'web/audio-client.js').read_text();assert 'try { element.setPointerCapture?.(event.pointerId); }' in audio_client
  for script in ('sw.js','video-player.js','app.js'):subprocess.run(['node','--check',str(ROOT/'web'/script)],check=True,capture_output=True)
  subprocess.run(['node',str(ROOT/'web/video-player.test.js')],check=True,capture_output=True)
  m.close()
  migrated_auth=(pathlib.Path(state)/'auth.db').read_bytes()
  print('PASS: warning auth/SSH sync/CSRF/origin/helpers/PTZ/fMP4/MQTT')
 finally:
  p.terminate()
  try:p.wait(3)
  except subprocess.TimeoutExpired:p.kill()
with tempfile.TemporaryDirectory() as migrated,tempfile.TemporaryDirectory() as migrated_stage:
 pathlib.Path(migrated,'auth.db').write_bytes(migrated_auth)
 env=os.environ|{'JOAN_STATE_DIR':migrated,'JOAN_STAGING_DIR':migrated_stage,'JOAN_WEB_DIR':str(ROOT/'web'),'JOAN_INTEGRATION_HELPER':str(HELPER),'JOAN_MQTT_PORT':'0','JOAN_RTSP_PORT':'18554','JOAN_MDNS':'0'}
 p=subprocess.Popen([BIN,'--plain-http','--bind','127.0.0.1','--port','18082'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 try:
  for _ in range(100):
   try:
    c=http.client.HTTPConnection('127.0.0.1',18082,timeout=2);c.request('GET','/api/health');r=c.getresponse();r.read()
    if r.status==200:break
   except OSError:time.sleep(.05)
  c=http.client.HTTPConnection('127.0.0.1',18082,timeout=3);c.request('POST','/api/v1/session',json.dumps({'username':'admin','password':'correct horse battery staple'}),{'Content-Type':'application/json'});r=c.getresponse();body=json.loads(r.read());cookie=dict(r.getheaders())['Set-Cookie'].split(';',1)[0]
  c=http.client.HTTPConnection('127.0.0.1',18082,timeout=3);c.request('GET','/api/v1/status',headers={'Cookie':cookie});r=c.getresponse();status=json.loads(r.read());assert status['ssh_password_sync'] is False and status['rtsp_password_sync'] is False;assert not pathlib.Path(migrated,'ssh/passwd').exists() and not pathlib.Path(migrated,'rtsp.password').exists()
 finally:
  p.terminate();p.wait(3)
stop.set();rtsp.join(2)
