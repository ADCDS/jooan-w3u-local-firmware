#!/usr/bin/env python3
"""One owned replay stack: protocol, real decode, persistence and recording checks.

No camera access. --qemu executes the rebuilt MIPS application in userspace
against workstation-generated files, never an OEM application or kernel.
"""
import argparse
import base64
import http.client
import json
from pathlib import Path
import secrets
import signal
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor
import time


def port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                            timeout=kwargs.pop("timeout", 90), **kwargs)
    if result.returncode:
        raise AssertionError(f"Command failed ({result.returncode}): {command[0]}\n{result.stdout}\n{result.stderr}")
    return result


def sntp_server(stop):
    """Loopback SNTP server answering with this host's own clock.

    Every request first receives a forged reply with the wrong origin cookie,
    which the daemon must ignore. The genuine reply's offset is below the
    daemon's step threshold, so the test never moves the host clock.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0.2)
    counts = {"requests": 0, "forged": 0}

    def stamp(now):
        return struct.pack("!II", (int(now) + 2208988800) & 0xffffffff, int((now % 1) * 2**32))

    def serve():
        while not stop.is_set():
            try:
                request, peer = sock.recvfrom(512)
            except socket.timeout:
                continue
            if len(request) < 48 or request[0] & 7 != 3:
                continue
            counts["requests"] += 1
            received = stamp(time.time())
            sock.sendto(bytes([0x24, 2]) + bytes(30) + received + stamp(time.time()), peer)
            counts["forged"] += 1
            sock.sendto(bytes([0x24, 2]) + bytes(22) + request[40:48] + received + stamp(time.time()), peer)

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return sock, thread, counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--qemu")
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--tls", action="store_true")
    parser.add_argument("--main-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    prefix = [args.qemu, "-L", str(args.sysroot.resolve())] if args.qemu and args.sysroot else []
    if args.qemu and not args.sysroot:
        parser.error("--qemu requires --sysroot")
    here = Path(__file__).resolve().parent
    result = {"binary": str(binary), "mode": "qemu-replay" if prefix else "host-replay",
              "tls": args.tls, "main_only": args.main_only, "camera_access": False, "checks": []}
    with tempfile.TemporaryDirectory(prefix="jipc-integration-") as temp:
        root = Path(temp)
        fixture = root / "source.h264"
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             "testsrc2=size=320x240:rate=15", "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
             "-x264-params", "threads=1:keyint=15:min-keyint=15:scenecut=0:aud=1:repeat-headers=1",
             "-pix_fmt", "yuv420p", "-f", "h264", fixture])
        fixed_fixture = root / "fixed.h264"
        if args.main_only:
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                 "testsrc2=size=160x120:rate=15", "-t", "2", "-c:v", "libx264", "-preset", "ultrafast",
                 "-x264-params", "threads=1:keyint=15:min-keyint=15:scenecut=0:aud=1:repeat-headers=1",
                 "-pix_fmt", "yuv420p", "-f", "h264", fixed_fixture])
        http_port, rtsp_port, discovery_port = port(), port(), port(socket.SOCK_DGRAM)
        secret = "test-" + secrets.token_hex(12)
        ntp_stop = threading.Event()
        ntp_socket, ntp_thread, ntp_counts = sntp_server(ntp_stop)
        ntp_port = ntp_socket.getsockname()[1]
        config = root / "camera.ini"
        # Empty file-side input intentionally exercises CLI override bootstrap/reload.
        config.write_text(f"username=admin\npassword={secret}\nbackend=replay\ninput0=\n"
                          f"width=320\nheight=240\nsub_width=320\nsub_height=240\n"
                          f"state_dir={root / 'state'}\nrecord_dir={root / 'recordings'}\n"
                          f"record_seconds=1\nrecord_max_mb=2\ndiscovery_port={discovery_port}\n"
                          f"ntp_server=127.0.0.1:{ntp_port}\n")
        if args.main_only:
            text = config.read_text().replace("sub_width=320", "sub_width=160").replace("sub_height=240", "sub_height=120")
            config.write_text(text + f"main_only=1\nwidth1=160\nheight1=120\ninput2={fixed_fixture}\n")
        config.chmod(0o600)
        context = None
        if args.tls:
            key, cert = root / "key.pem", root / "cert.pem"
            run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                 "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
                 "-keyout", key, "-out", cert])
            with config.open("a") as handle:
                handle.write(f"tls_cert={cert}\ntls_key={key}\n")
            context = ssl.create_default_context(cafile=str(cert))
        command = prefix + [str(binary), "--config", str(config), "--input", str(fixture),
                            "--http-port", str(http_port), "--rtsp-port", str(rtsp_port), "--duration", "180"]
        log = (root / "daemon.log").open("w")
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=root)

        def request(method, path, data=None, password=None):
            password = secret if password is None else password
            client = (http.client.HTTPSConnection("127.0.0.1", http_port, context=context, timeout=5)
                      if args.tls else http.client.HTTPConnection("127.0.0.1", http_port, timeout=5))
            headers = {"Authorization": "Basic " + base64.b64encode(f"admin:{password}".encode()).decode()}
            body = json.dumps(data) if data is not None else None
            if body is not None:
                headers["Content-Type"] = "application/json"
            try:
                client.request(method, path, body=body, headers=headers)
                reply = client.getresponse()
                return reply.status, reply.read()
            finally:
                client.close()

        def wait_ready():
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError("Daemon exited:\n" + (root / "daemon.log").read_text())
                try:
                    status, body = request("GET", "/api/v1/status")
                    if status == 200 and json.loads(body)["ready"]:
                        return json.loads(body)
                except (OSError, http.client.HTTPException, ValueError):
                    pass
                time.sleep(0.05)
            raise AssertionError("Daemon did not become ready:\n" + (root / "daemon.log").read_text())

        try:
            status = wait_ready()
            assert status["backend"] == "replay" and "platform" in status
            result["checks"].append("CLI input override and ready status")
            deadline = time.monotonic() + 10
            while True:
                clock = json.loads(request("GET", "/api/v1/status")[1])["clock"]
                if clock["synchronized"] or time.monotonic() > deadline:
                    break
                time.sleep(0.1)
            assert clock["source"] == "ntp" and clock["synchronized"], clock
            assert clock["error"] == "" and abs(clock["last_offset_ms"]) < 1000, clock
            assert ntp_counts["forged"] >= 1, ntp_counts
            client = (http.client.HTTPSConnection("127.0.0.1", http_port, context=context, timeout=5)
                      if args.tls else http.client.HTTPConnection("127.0.0.1", http_port, timeout=5))
            try:
                client.request("POST", "/onvif/device_service", headers={"Content-Type": "application/soap+xml"},
                               body='<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
                                    '<GetSystemDateAndTime xmlns="http://www.onvif.org/ver10/device/wsdl"/>'
                                    '</s:Body></s:Envelope>')
                assert b"<tt:DateTimeType>NTP</tt:DateTimeType>" in client.getresponse().read()
            finally:
                client.close()
            result["clock"] = clock
            result["checks"].append("SNTP sync ignoring a forged-origin reply; ONVIF reports NTP time")
            protocol = run(["python3", here / "test_rtsp.py", "--port", rtsp_port,
                            "--username", "admin", "--password", secret, "--audio"], timeout=100)
            result["rtsp_protocol"] = protocol.stderr.strip()
            http_args = ["python3", here / "test_http_onvif.py", "--port", http_port,
                         "--username", "admin", "--password", secret]
            if args.tls:
                http_args += ["--tls", "--ca", root / "cert.pem"]
            protocol = run(http_args, timeout=100)
            result["http_onvif_protocol"] = protocol.stderr.strip()
            discovery_args = ["python3", here / "test_discovery.py", "--port", discovery_port,
                              "--http-port", http_port]
            if args.tls:
                discovery_args += ["--tls"]
            protocol = run(discovery_args, timeout=20)
            result["discovery_protocol"] = protocol.stderr.strip()
            def decode(transport, channel):
                url = f"rtsp://admin:{secret}@127.0.0.1:{rtsp_port}/live/ch{channel}"  # hygiene: allow-test-vector
                run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-threads", "1",
                     "-rtsp_transport", transport, "-i", url, "-t", "1", "-map", "0:v:0", "-threads:v", "1", "-f", "null", "-"], timeout=25)
                return f"FFmpeg decoded channel{channel} via {transport}"
            if args.main_only:
                # Two simultaneous subscribers exercise each alias pair.
                for pair in ((0,1),(2,3)):
                    with ThreadPoolExecutor(max_workers=2) as pool:
                        jobs=[pool.submit(decode,transport,ch) for transport,ch in zip(("tcp","udp"),pair)]
                        result["checks"].extend(j.result() for j in jobs)
                client = (http.client.HTTPSConnection("127.0.0.1",http_port,context=context,timeout=5)
                          if args.tls else http.client.HTTPConnection("127.0.0.1",http_port,timeout=5))
                headers={"Authorization":"Basic "+base64.b64encode(f"admin:{secret}".encode()).decode(),"Content-Type":"application/soap+xml"}
                def soap(action, inner=""):
                    client.request("POST","/onvif/media_service",body=f"<Envelope><Body><{action}>{inner}</{action}></Body></Envelope>",headers=headers)
                    reply=client.getresponse();return reply.status,reply.read()
                code,body=soap("GetProfiles");assert code==200
                profiles=ET.fromstring(body).findall(".//{http://www.onvif.org/ver10/media/wsdl}Profiles")
                ns={"tt":"http://www.onvif.org/ver10/schema"}
                for i,p in enumerate(profiles):
                    enc=p.find("tt:VideoEncoderConfiguration",ns)
                    size=(int(enc.findtext("tt:Resolution/tt:Width",namespaces=ns)),int(enc.findtext("tt:Resolution/tt:Height",namespaces=ns)))
                    assert size==((320,240) if i<2 else (160,120)),size
                    assert soap("GetSnapshotUri",f"<ProfileToken>{p.attrib['token']}</ProfileToken>")[0]==500
                assert len(profiles)==4
                code,body=soap("GetServiceCapabilities");assert code==200 and b'SnapshotUri="false"' in body
                client.close()
                streams=json.loads(request("GET","/api/v1/status")[1])["streams"]
                assert streams[1]["alias_of"]==0 and streams[3]["alias_of"]==2
                result["checks"].append("all logical profiles report effective geometry; alias subscribers decode concurrently; JPEG capability disabled")
            else:
                for transport,channel in (("tcp",0),("udp",3)):
                    result["checks"].append(decode(transport,channel))
            assert request("POST", "/api/v1/control", {"operation": "white_light", "value": 1})[0] == 200
            assert request("POST", "/api/v1/control", {"operation": "preset_set", "value": 1})[0] == 200
            assert request("POST", "/api/v1/control", {"operation": "ptz", "value": 50, "value2": -25})[0] == 200
            time.sleep(1.5)
            status = json.loads(request("GET", "/api/v1/status")[1])
            assert not status["platform"]["moving"]
            result["checks"].append("replay PTZ deadman, presets and light control")
            assert request("POST", "/api/v1/recording", {"enabled": 1})[0] == 200
            time.sleep(2.5)
            assert request("POST", "/api/v1/recording", {"enabled": 0})[0] == 200
            time.sleep(0.2)
            listing = json.loads(request("GET", "/api/v1/recordings")[1])["recordings"]
            if args.main_only:
                assert not any(item["name"].startswith(("stream-1-","stream-3-")) for item in listing)
                assert all(any(item["name"].startswith(f"stream-{ch}-") for item in listing) for ch in (0,2))
            video = next(item["name"] for item in listing if item["name"].endswith(".h264") and item["size"] > 1024)
            code, body = request("GET", "/api/v1/recordings/" + video)
            assert code == 200 and len(body) > 1024
            recording = root / "download.h264"
            recording.write_bytes(body)
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-xerror", "-threads", "1",
                 "-i", recording, "-f", "null", "-"], timeout=15)
            result["checks"].append("recording rotation, authenticated download and FFmpeg decode")
            new_secret = "changed-" + secrets.token_hex(12)
            assert request("POST", "/api/v1/config", {"password": new_secret})[0] == 200
            assert request("GET", "/api/v1/status", password=secret)[0] == 401
            secret = new_secret
            assert request("GET", "/api/v1/status")[0] == 200
            contents = config.read_text()
            for key, value in [("input0", ""), ("http_port", "8080"), ("rtsp_port", "8554")]:
                contents = "\n".join(f"{key}={value}" if line.startswith(key + "=") else line
                                     for line in contents.splitlines()) + "\n"
            config.write_text(contents)
            process.send_signal(signal.SIGHUP)
            time.sleep(0.5)
            wait_ready()
            assert (config.stat().st_mode & 0o777) == 0o600
            result["checks"].append("credential persistence, old-password rejection, CLI overrides retained on reload")
            # Last: this locks 127.0.0.1 out for a minute. Earlier negative
            # tests may already have counted, so the block can come sooner.
            statuses = [request("GET", "/api/v1/status", password="not-" + secret)[0] for _ in range(10)]
            assert set(statuses) <= {401, 429}, statuses
            # After ten failures even the right password is not checked.
            assert request("GET", "/api/v1/status")[0] == 429
            assert request("GET", "/health")[0] == 200
            result["checks"].append("failed-login throttle blocks an address after repeated wrong passwords")
        finally:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
                raise AssertionError("Daemon did not shut down within15seconds")
            ntp_stop.set(); ntp_thread.join(); ntp_socket.close()
            log.close()
            result["daemon_exit"] = process.returncode
            result["daemon_log"] = (root / "daemon.log").read_text()
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(result, indent=2) + "\n")
        assert process.returncode == 0, result["daemon_log"]
    print(json.dumps({key: value for key, value in result.items() if key != "daemon_log"}, indent=2))


if __name__ == "__main__":
    main()
