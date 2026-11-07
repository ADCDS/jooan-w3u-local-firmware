#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install one JA-A12 camera release bundle: both layers, one transaction.

A bundle directory holds bundle.json plus the artifacts it names:

  jooan_local    signed local-firmware package (JOOAN_FW_PKG), applied through
                 the platform's authenticated update API; the camera reboots.
  jooanipc_open  rebuilt appfs image carrying one jooanipc-open release,
                 flashed by upgrade-appfs.sh (native->native); the camera reboots.
  config         file-level changes: camera.ini keys to set/unset, platform
                 state files to write or delete, stale files to remove.

Config changes are staged first, so a single reboot applies everything. Every
touched file is copied into BUNDLE/backups/<UTC time>/ before any change
(those copies contain the camera password: keep bundles private). A failure
before the first reboot restores them. Nothing runs without --yes.

A bundle from build-appfs.sh --first instead installs the first media release on
a camera that has only the platform, and is flashed by first-appfs.sh. That path has
not yet been run on a real camera, so it also needs --yes-untested and
STREAM_PASSWORD, the password its streams will use.

Credentials come from the environment: CAMERA_PASSWORD (SSH and the platform
API) and MEDIA_PASSWORD (jooanipc-open's API; defaults to CAMERA_PASSWORD).
With CAMERA_PASSWORD set, SSH uses sshpass when it is installed, or otherwise the
ssh program's own SSH_ASKPASS, which is what the OpenSSH in Windows offers.
"""
import argparse
import base64
import datetime
import hashlib
import http.client
import json
import os
import pathlib
import shlex
import shutil
import ssl
import subprocess
import sys
import tempfile
import time

INI = "/opt/jooanipc-open/camera.ini"
ENV = "/opt/jooanipc-open/deployment.env"
HANDOFF = "/opt/jooanipc-open/boot-handoff.sh"
NATIVE_BOOT = "/opt/jooanipc-open/native-boot.sh"
RELEASES = "/mnt/mtd/jooanipc-open/releases"
MEDIA_CERT = "/opt/jooanipc-open/cert.pem"
NATIVE = "/opt/jooanipc-open/native"
WIFI_JSON = "/opt/custom/jooan-local/config/wifi.json"
FIRST_PLATFORM_SEQUENCE = 70


def log(message):
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def askpass_helper(directory):
    """A program for ssh's SSH_ASKPASS that prints the password from the environment. This
    is what works with the OpenSSH that Windows ships, where there is no sshpass. It needs
    OpenSSH 8.4 or later for SSH_ASKPASS_REQUIRE. The password is never written to disk."""
    code = "import os; print(os.environ['CAMERA_ASKPASS'])"
    if os.name == "nt":
        path = pathlib.Path(directory) / "askpass.cmd"
        path.write_text(f'@"{sys.executable}" -c "{code}"\n')  # text mode adds the \r
    else:
        path = pathlib.Path(directory) / "askpass.sh"
        path.write_text(f'#!/bin/sh\nexec "{sys.executable}" -c "{code}"\n')
        path.chmod(0o700)
    return path


class Camera:
    def __init__(self, host, password, media_password):
        self.host, self.password, self.media_password = host, password, media_password
        self.prefix = (["sshpass", "-e"] if password and shutil.which("sshpass") else [])
        self._media_cert = None
        self._askpass = None

    def ssh_environment(self):
        if self.prefix:
            return os.environ | {"SSHPASS": self.password}
        if not self.password:
            return dict(os.environ)  # an agent or a key, or ssh asks
        if self._askpass is None:
            self._askpass_dir = tempfile.TemporaryDirectory(prefix="camera-ssh-")
            self._askpass = askpass_helper(self._askpass_dir.name)
        return os.environ | {"SSH_ASKPASS": str(self._askpass), "SSH_ASKPASS_REQUIRE": "force", "DISPLAY": "none",
                             "CAMERA_ASKPASS": self.password}

    def ssh_bytes(self, script, data=None, timeout=120, check=True):
        env = self.ssh_environment()
        command = self.prefix + ["ssh", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
                                 "-o", "ServerAliveCountMax=3", f"admin@{self.host}", script]
        result = subprocess.run(command, input=data, capture_output=True, timeout=timeout, env=env)
        if check and result.returncode:
            raise RuntimeError(f"camera command failed ({result.returncode}): {script[:80]}\n"
                               + result.stderr.decode(errors="replace"))
        return result.stdout

    def ssh(self, script, data=None, timeout=120, check=True):
        return self.ssh_bytes(script, data, timeout, check).decode(errors="replace")

    def boot_id(self):
        """Changes with every boot, so it tells a reboot from a dropped connection."""
        value = self.ssh("cat /proc/sys/kernel/random/boot_id", timeout=15).strip()
        if len(value) != 36:
            raise RuntimeError(f"unexpected boot ID {value!r}")
        return value

    def up(self):
        try:
            return self.ssh("echo ok", timeout=15, check=False).strip() == "ok"
        except subprocess.TimeoutExpired:
            return False

    def platform(self, method, path, body=None, session=None, timeout=30):
        c = http.client.HTTPConnection(self.host, 80, timeout=timeout)
        headers = {"Host": self.host}
        if session:
            headers |= {"Cookie": session[0], "X-CSRF-Token": session[1]}
        if isinstance(body, (dict, list)):
            body, headers["Content-Type"] = json.dumps(body), "application/json"
        elif body is not None:
            headers["Content-Type"] = "application/octet-stream"
        c.request(method, path, body=body, headers=headers)
        r = c.getresponse()
        data = r.read()
        return r.status, dict(r.getheaders()), data

    def login(self):
        status, headers, data = self.platform("POST", "/api/v1/session",
                                              {"username": "admin", "password": self.password})
        if status != 200:
            raise RuntimeError(f"platform login failed: {status} {data[:200]!r}")
        return headers["Set-Cookie"].split(";", 1)[0], json.loads(data)["csrf"]

    def media_context(self):
        """Trust exactly the media daemon's own certificate, read over SSH.

        SSH authenticates the camera by its host key, so the certificate it
        returns is the one the daemon serves; the Basic credentials below are
        then only sent to that certificate's holder. The name is not checked:
        the certificate is self-signed and the camera is addressed by IP."""
        if self._media_cert is None:
            pem = self.ssh(f"cat {MEDIA_CERT}")
            if "-----BEGIN CERTIFICATE-----" not in pem:
                raise RuntimeError(f"no media certificate at {MEDIA_CERT}")
            self._media_cert = pem
        context = ssl.create_default_context(cadata=self._media_cert)
        context.check_hostname = False
        return context

    def media_status(self):
        c = http.client.HTTPSConnection(self.host, 18443, timeout=15, context=self.media_context())
        token = base64.b64encode(f"admin:{self.media_password}".encode()).decode()
        c.request("GET", "/api/v1/status", headers={"Authorization": "Basic " + token})
        r = c.getresponse()
        return r.status, json.loads(r.read() or b"{}")


def wait(predicate, seconds, what, step=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except (OSError, http.client.HTTPException, ValueError, RuntimeError, subprocess.TimeoutExpired):
            pass
        time.sleep(step)
    raise RuntimeError(f"timed out waiting for {what}")


def boot_mode(cam):
    """'native' or 'handoff' once this boot's media qualified; None until then."""
    logs = cam.ssh("cat /tmp/jipc-native.log /tmp/jipc-handoff.log 2>/dev/null", check=False)
    if "native boot qualified" in logs:
        return "native"
    if "boot handoff qualified" in logs:
        return "handoff"
    if "leaving current owner alone" in logs:
        raise RuntimeError("the boot handoff gave up:\n" + logs[-600:])
    return None


def preflight_problems(facts, platform, local, appfs, config):
    """Reasons the bundle must not be applied to the camera as it is now."""
    problems = []
    if facts.get("appfs_size") != "3211264" or facts.get("appfs_name") != "appfs":
        problems.append("not the qualified JA-A12 flash layout")
    if int(facts.get("opt_free", 0) or 0) < 64:
        problems.append("/opt has less than 64 KiB free")
    if facts.get("trial") != "no":
        problems.append("a failed jooanipc-open boot trial is latched")
    if facts.get("native_failed"):
        problems.append("native-failed is latched (" + facts["native_failed"].strip() + "): the last native "
                        f"boot failed and the camera runs the OEM handoff; fix the cause, then delete {NATIVE}-failed")
    if facts.get("qualified", "0") == "0":
        problems.append("jooanipc-open did not qualify on this boot (reboot to fall back first)")
    files = config.get("files", {})
    # A bundle may create (any content) or delete (None) the native marker.
    native_after = native_expected(facts, config)
    if native_after and facts.get("wifi_json") != "yes":
        problems.append(f"native boot needs a committed Wi-Fi network ({WIFI_JSON}); without it the boot "
                        "fails after 300 s and latches native-failed. Commit one through the platform first")
    if local and platform.get("release_sequence", 0) >= local["release_sequence"]:
        problems.append(f"platform already at sequence {platform.get('release_sequence')}")
    if appfs and facts.get("appfs_md5") != appfs["expected_current_appfs_md5"]:
        problems.append(f"appfs is {facts.get('appfs_md5')}, bundle expects {appfs['expected_current_appfs_md5']}")
    # The boot checks libimp against deployment.env: a new library that does not
    # bring its sums would fail that check and drop the camera to its fallback.
    if appfs and "release" in appfs and "-" in appfs["release"]:
        library = appfs["release"].rsplit("-", 1)[1]
        if not facts.get("libimp_sha256", "").startswith(library) and not (
                appfs.get("imp_md5") and appfs.get("imp_sha256", "").startswith(library)):
            problems.append(f"release {appfs['release']} changes libimp, but the bundle has no matching "
                            "imp_md5 and imp_sha256")
    return problems


def first_problems(facts, platform, appfs):
    """Why a camera cannot take its first media release."""
    problems = []
    if int(platform.get("release_sequence") or 0) < FIRST_PLATFORM_SEQUENCE:
        problems.append(f"the platform is sequence {platform.get('release_sequence')}: a first install needs "
                        f"{FIRST_PLATFORM_SEQUENCE} or later, because older ones cannot boot the media layer as this expects")
    if facts.get("sensors") != "cv2005 cv2005s1":
        problems.append(f"this camera's sensors read {facts.get('sensors')!r}, not 'cv2005 cv2005s1': the image keeps "
                        "only those sensors' drivers, and it would have no video")
    if facts.get("appfs_size") != "3211264" or facts.get("appfs_name") != "appfs":
        problems.append("this is not a JA-A12 app partition")
    if facts.get("appfs_md5") != appfs["expected_current_appfs_md5"]:
        problems.append(f"appfs is {facts.get('appfs_md5')}: the bundle was built from another copy "
                        f"({appfs['expected_current_appfs_md5']}); build it from this camera's own")
    if platform.get("state") != "ready":
        problems.append("install the platform first, and set its password")
    if facts.get("native") == "yes" or facts.get("enabled") == "yes" or facts.get("daemon") != "0":
        problems.append("this camera already has a media layer: use an ordinary update")
    if facts.get("oem") != "1":
        problems.append("the stock application is not running as expected")
    if facts.get("wifi_json") != "yes":
        problems.append(f"native boot needs a committed Wi-Fi network ({WIFI_JSON}); without it the boot "
                        "fails after 300 s. Commit one through the platform first")
    if int(facts.get("opt_free") or 0) < 96:
        problems.append("/opt has under 96 KiB free; the media layer's files need room and the stock boot needs 50 KiB left")
    return problems


def stream_password_problem(password):
    """The boot's configuration check refuses these, and by then the flash is done."""
    if not password:
        return "set STREAM_PASSWORD, the password the camera's video streams will use"
    if len(password.encode()) > 128:
        return "STREAM_PASSWORD is over 128 bytes"
    if password != password.strip():
        return "STREAM_PASSWORD starts or ends with a space, which the camera would drop"
    if any(ord(c) < 32 or ord(c) == 127 for c in password):
        return "STREAM_PASSWORD holds a control character"
    return ""


def after_commit(why):
    """A line from the flash script about something past its point of no return."""
    return "rollback failed" in why or "config space low" in why


def platform_stopped(cam, session):
    """Why a platform update stopped, if it did: otherwise the wait would just run out, 20 minutes on."""
    try:
        status, _, data = cam.platform("GET", "/api/v1/update", None, session, timeout=10)
        update = json.loads(data) if status == 200 else {}
    except (OSError, ValueError, http.client.HTTPException):
        return ""
    if update.get("state") == "failed":
        return f"the platform update stopped ({update.get('reason') or 'no reason given'})"
    if update.get("restart") == "needed":
        return "the platform update finished, but the camera has not restarted"
    return ""


def stopped_message(why):
    if why.startswith("the platform update"):
        return (f"{why}.\nDO NOT restart or unplug the camera until you have looked at it: docs/RECOVERY.md, "
                "'An update stopped'")
    if "rollback failed" in why:
        return (f"the flash failed and so did putting the old partition back: {why}\nDO NOT restart or unplug the "
                "camera: it would not boot. See docs/RECOVERY.md, 'Appfs reflash recovery'")
    if "config space low" in why:
        return f"the new release is written, but /opt is nearly full: {why}\nFree space over SSH, then restart it"
    return f"nothing was changed; the camera's installer stopped: {why}"


def first_install(args, cam, bundle, spec):
    """The first media release on a camera with only the platform. Not yet run on a camera:
    first-appfs.sh and this have only been tested offline, so it needs --yes-untested."""
    appfs = spec["jooanipc_open"]
    for name, field in ((appfs["image"], "image_md5"), (appfs["installer"], "installer_md5")):
        if md5(bundle / name) != appfs[field]:
            raise SystemExit(f"{name} checksum mismatch")
    facts = dict(line.split("=", 1) for line in cam.ssh(
        "echo appfs_size=$(cat /sys/class/mtd/mtd4/size);"
        "echo appfs_name=$(cat /sys/class/mtd/mtd4/name);"
        "echo appfs_md5=$(md5sum /dev/mtdblock4 | cut -c1-32);"
        "echo opt_free=$(df -k /opt | awk 'END {print $4}');"
        f"echo native=$([ -e {NATIVE} ] && echo yes || echo no);"
        "echo enabled=$([ -e /opt/jooanipc-open/enabled ] && echo yes || echo no);"
        "echo sensors=$(cat /tmp/sensor1_name /tmp/sensor2_name 2>/dev/null);"
        f"echo wifi_json=$([ -s {WIFI_JSON} ] && echo yes || echo no);"
        "n=0; d=0; for f in /proc/[0-9]*/comm; do c=$(cat $f 2>/dev/null); "
        "[ \"$c\" = jooanipc ] && n=$((n+1)); [ \"$c\" = jooanipc-open ] && d=$((d+1)); done; "
        "echo oem=$n; echo daemon=$d").splitlines() if "=" in line)
    status, _, data = cam.platform("GET", "/api/v1/setup/status")
    platform = json.loads(data) if status == 200 else {}
    problems = first_problems(facts, platform, appfs)
    log(f"first release {appfs['release']} over appfs {facts.get('appfs_md5')}")
    if problems:
        raise SystemExit("not installing:\n  " + "\n  ".join(problems))
    stream_password = os.environ.get("STREAM_PASSWORD", "")
    problem = stream_password_problem(stream_password)
    if problem or args.yes_untested is not True:
        raise SystemExit("preflight passed. A first install has not yet been run on a real camera: its flash script is "
                         f"only tested offline. To go ahead, pass --yes-untested.{' And ' + problem + '.' if problem else ''}")
    if not shutil.which("openssl"):
        raise SystemExit("a first install needs the openssl program, to make the camera's certificate")
    root = "/opt/jooanipc-open"
    release = f"{RELEASES}/{appfs['release']}"
    example = (pathlib.Path(__file__).resolve().parent.parent / "config/ja-a12.example.ini").read_text()
    camera_ini = ini_edit(example, {"libimp": f"{release}/libimp.so", "username": "admin", "password": stream_password,
                                    "bind": "0.0.0.0", "audio": "1"}, [])
    deployment = (f"JIPC_RELEASE={release}\nJIPC_LIBIMP_SHA256={appfs['imp_sha256']}\nJIPC_APP_MD5={appfs['app_md5']}\n"
                  f"JIPC_IMP_MD5={appfs['imp_md5']}\nJIPC_ATOMIC_MD5={appfs['atomic_md5']}\nJIPC_WIFI_BSSID=\nJIPC_WIFI_FREQ=\n")
    tidy = (f"rm -f {root}/camera.ini.next {root}/deployment.env.next {root}/boot-handoff.sh.next "
            "/tmp/jipc-flash-candidate.squashfs /tmp/first-appfs.sh; sync")
    try:
        with tempfile.TemporaryDirectory(prefix="camera-tls-") as temporary:
            key, cert = pathlib.Path(temporary) / "key.pem", pathlib.Path(temporary) / "cert.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "7300", "-subj", "/CN=camera",
                            "-keyout", str(key), "-out", str(cert)], check=True, capture_output=True)
            cam.ssh(f"mkdir -p {root} && chmod 700 {root}")
            write_remote(cam, f"{root}/key.pem", key.read_bytes())
            write_remote(cam, f"{root}/cert.pem", cert.read_bytes(), "644")
        write_remote(cam, f"{root}/native-boot.sh", (bundle / "native-boot.sh").read_bytes(), "700")
        write_remote(cam, f"{root}/boot-handoff.sh.next", (bundle / "boot-handoff.sh").read_bytes(), "700")
        write_remote(cam, f"{root}/camera.ini.next", camera_ini)
        write_remote(cam, f"{root}/deployment.env.next", deployment)
        cam.ssh("cat > /tmp/jipc-flash-candidate.squashfs", data=(bundle / appfs["image"]).read_bytes(), timeout=300)
        cam.ssh("cat > /tmp/first-appfs.sh && chmod 700 /tmp/first-appfs.sh", data=(bundle / appfs["installer"]).read_bytes())
        before = cam.boot_id()
    except Exception:
        cam.ssh(tidy, timeout=20, check=False)  # the staged copies take up the small settings partition
        raise
    log("flashing the first release (it verifies, keeps a RAM copy of the stock partition and reboots)")
    cam.ssh(f"( trap '' HUP; exec sh /tmp/first-appfs.sh {appfs['image_md5']} {facts['appfs_md5']} ) "
            "</dev/null >/tmp/jipc-upgrade.log 2>&1 &")

    def aborted():
        try:
            why = cam.ssh("grep ' ABORT: ' /tmp/jipc-upgrade.log || :", timeout=10).strip()
            if why and not after_commit(why):  # past that, recovery needs the images in /tmp
                cam.ssh(tidy, timeout=20)
            return why
        except Exception:
            return ""

    def flashed():
        # A camera it backed out of is back on stock, and no media daemon will ever qualify.
        now = cam.ssh("md5sum /dev/mtdblock4 | cut -c1-32").strip()
        if now != appfs["image_md5"]:
            cam.ssh(tidy, timeout=20)
            raise SystemExit(f"appfs is {now} after the reboot: the first release was not installed, "
                             "and the camera is on its stock application")

    try:
        mode = wait_reboot(cam, before, aborted, after_boot=flashed)
    except InstallerStopped as stopped:
        raise SystemExit(str(stopped)) from None
    if mode != "native":
        raise SystemExit(f"installed, but this boot took the {mode} path: see docs/RECOVERY.md")
    log(f"first media release {appfs['release']} running")
    return 0


def native_expected(facts, config):
    """Whether the camera should boot native once this bundle is in."""
    files = (config or {}).get("files", {})
    return files[NATIVE] is not None if NATIVE in files else facts.get("native") == "yes"


class InstallerStopped(Exception):
    """The camera's installer gave up before changing anything; not worth retrying."""


def wait_reboot(cam, before, aborted=None, seconds=600, after_boot=None):
    """Waits for a boot other than `before`. Only a new boot ID says the camera restarted:
    a connection can fail while it is flashing, stopping services or just on bad Wi-Fi,
    and treating that as a restart would act on a camera that is still working."""
    # A platform apply runs its whole maintenance transaction before rebooting.
    def restarted():
        try:
            if cam.boot_id() != before:
                return True
        except (OSError, RuntimeError, subprocess.TimeoutExpired):
            return False
        why = aborted() if aborted else ""
        if why:
            raise InstallerStopped(stopped_message(why))
        return False
    wait(restarted, 600 + seconds, "the camera to restart", step=2)
    log("camera restarted")
    if after_boot:
        after_boot()
    log("SSH is back; waiting for the media daemon to qualify")
    deadline = time.monotonic() + seconds
    while (mode := boot_mode(cam)) is None:
        if time.monotonic() > deadline:
            raise RuntimeError("timed out waiting for the media daemon to qualify")
        time.sleep(5)
    log(f"media qualified ({mode} boot)")
    return mode


def check_appfs_artifacts(bundle, appfs):
    """What an update needs on this host. The boot script goes with its release: updates that
    left it behind kept every fix to it from reaching the camera."""
    if not appfs.get("native_boot"):
        raise SystemExit("this bundle has no native-boot.sh; rebuild it")
    for name, field in ((appfs["image"], "image_md5"), (appfs["installer"], "installer_md5"),
                        (appfs["native_boot"], "native_boot_md5")):
        if md5(bundle / name) != appfs[field]:
            raise SystemExit(f"{name} checksum mismatch")
    if (bundle / appfs["image"]).stat().st_size != 3211264:
        raise SystemExit("appfs image must be exactly the 3211264-byte partition")


def ini_edit(text, settings, unset):
    """Set or remove key=value lines, keeping every other line as it is."""
    lines, seen = [], set()
    for line in text.splitlines():
        key = line.split("=", 1)[0].strip()
        if key in unset:
            continue
        if key in settings:
            line = f"{key}={settings[key]}"
            seen.add(key)
        lines.append(line)
    lines += [f"{k}={v}" for k, v in settings.items() if k not in seen]
    return "\n".join(lines) + "\n"


def write_remote(cam, path, content, mode="600"):
    """Atomically replace a camera file; content is str or bytes."""
    data = content.encode() if isinstance(content, str) else content
    cam.ssh(f"umask 077; cat > {shlex.quote(path + '.tmp')} && chmod {mode} {shlex.quote(path + '.tmp')} && "
            f"mv -f {shlex.quote(path + '.tmp')} {shlex.quote(path)} && sync", data=data)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("bundle", type=pathlib.Path)
    p.add_argument("--camera", required=True, help="camera address")
    p.add_argument("--yes", action="store_true", help="apply; without it only preflight runs")
    p.add_argument("--nvr-stop", help="host shell command run before a layer update (e.g. stop Frigate)")
    p.add_argument("--nvr-start", help="host shell command run after verification")
    p.add_argument("--yes-untested", action="store_true",
                   help="with a bundle from build-appfs.sh --first: go ahead, knowing it has not yet been run on a camera")
    args = p.parse_args()
    bundle = args.bundle.resolve()
    spec = json.loads((bundle / "bundle.json").read_text())
    if spec.get("schema") != 1 or spec.get("board") != "JA-A12":
        raise SystemExit("unsupported bundle (schema 1, board JA-A12 expected)")
    password = os.environ.get("CAMERA_PASSWORD", "")
    cam = Camera(args.camera, password, os.environ.get("MEDIA_PASSWORD", password))
    local, appfs, config = spec.get("jooan_local"), spec.get("jooanipc_open"), spec.get("config", {})
    if appfs and appfs.get("first"):
        return first_install(args, cam, bundle, spec)

    # ---- artifacts on this host
    if local:
        if sha256(bundle / local["package"]) != local["package_sha256"]:
            raise SystemExit("jooan_local package checksum mismatch")
    if appfs:
        check_appfs_artifacts(bundle, appfs)
    log(f"bundle {spec['name']}: artifacts verified")

    # ---- preflight on the camera
    facts = dict(line.split("=", 1) for line in cam.ssh(
        "echo appfs_size=$(cat /sys/class/mtd/mtd4/size); echo appfs_name=$(cat /sys/class/mtd/mtd4/name);"
        "echo appfs_md5=$(md5sum /dev/mtdblock4 | cut -c1-32);"
        "echo opt_free=$(df -k /opt | awk 'END {print $4}');"
        "echo trial=$([ -e /opt/jooanipc-open/boot-trial ] && echo yes || echo no);"
        f"echo native=$([ -e {NATIVE} ] && echo yes || echo no);"
        f"echo native_failed=$(tr '\\n' ' ' < {NATIVE}-failed 2>/dev/null);"
        f"echo wifi_json=$([ -s {WIFI_JSON} ] && echo yes || echo no);"
        "echo qualified=$(cat /tmp/jipc-handoff.log /tmp/jipc-native.log 2>/dev/null | "
        "grep -c -E 'boot handoff qualified|native boot qualified');"
        "echo libimp_sha256=$(sed -n 's/^JIPC_LIBIMP_SHA256=//p' " + ENV + ");"
        "echo release=$(sed -n 's/^JIPC_RELEASE=//p' " + ENV + ")").splitlines() if "=" in line)
    status, _, data = cam.platform("GET", "/api/v1/setup/status")
    platform = json.loads(data) if status == 200 else {}
    problems = preflight_problems(facts, platform, local, appfs, config)
    log(f"camera: platform {platform.get('release_version')} (sequence {platform.get('release_sequence')}), "
        f"media {facts.get('release', '?').rsplit('/', 1)[-1]}, appfs {facts.get('appfs_md5')}")
    plan = []
    if config:
        plan.append("stage config: " + ", ".join(
            [f"camera.ini set {k}" for k in config.get("camera_ini", {}).get("set", {})]
            + [f"camera.ini unset {k}" for k in config.get("camera_ini", {}).get("unset", [])]
            + [f"write {k}" for k, v in config.get("files", {}).items() if v is not None]
            + [f"delete {k}" for k, v in config.get("files", {}).items() if v is None]))
    if local:
        plan.append(f"platform -> {local['release_version']} (sequence {local['release_sequence']}), reboot")
    if appfs:
        plan.append(f"media -> {appfs['release']} (appfs {appfs['image_md5']}), reboot")
    for line in plan:
        log("plan: " + line)
    if problems:
        raise SystemExit("preflight failed:\n  " + "\n  ".join(problems))
    if not args.yes:
        log("preflight passed; rerun with --yes to apply")
        return 0

    # ---- back up everything that will change
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backups = bundle / "backups" / stamp
    backups.mkdir(parents=True, mode=0o700)
    touched = {INI, ENV, HANDOFF} | set(config.get("files", {}))
    saved = {}
    for path in sorted(touched):
        present = cam.ssh(f"[ -e {shlex.quote(path)} ] && echo yes || echo no").strip() == "yes"
        saved[path] = cam.ssh_bytes(f"cat {shlex.quote(path)}") if present else None
        if saved[path] is not None:
            target = backups / path.lstrip("/").replace("/", "__")
            target.write_bytes(saved[path]); target.chmod(0o600)
    log(f"backed up {sum(v is not None for v in saved.values())} files to {backups}")

    def restore():
        for path, content in saved.items():
            if content is None:
                cam.ssh(f"rm -f {shlex.quote(path)}", check=False)
            else:
                write_remote(cam, path, content, "700" if path == HANDOFF else "600")
        log("configuration restored from backup")

    rebooted = nvr_stopped = False
    mode = boot_mode(cam)
    try:
        # ---- config, staged before any reboot
        # New camera.ini keys must meet the binary that knows them: with a media
        # layer they go into the staged copy the appfs installer commits.
        ini_set = config.get("camera_ini", {}).get("set", {})
        ini_unset = set(config.get("camera_ini", {}).get("unset", []))
        if (ini_set or ini_unset) and not appfs:
            write_remote(cam, INI, ini_edit(saved[INI].decode(), ini_set, ini_unset))
        for path, content in config.get("files", {}).items():
            if content is None:
                cam.ssh(f"rm -f {shlex.quote(path)} && sync")
            elif isinstance(content, dict):
                write_remote(cam, path, (bundle / content["from"]).read_bytes(), content.get("mode", "600"))
            else:
                write_remote(cam, path, content)
        if config:
            log("config staged")
        if args.nvr_stop and (local or appfs):
            log("stopping the NVR")
            subprocess.run(args.nvr_stop, shell=True, check=True)
            nvr_stopped = True

        # ---- platform layer
        if local:
            session = cam.login()
            status, _, data = cam.platform("POST", "/api/v1/update", (bundle / local["package"]).read_bytes(),
                                           session, timeout=180)
            if status != 200:
                raise RuntimeError(f"platform rejected the package: {status} {data[:300]!r}")
            staged = json.loads(data)["id"]
            log(f"platform package verified on camera ({staged[:12]}); applying")
            before = cam.boot_id()
            status, _, data = cam.platform("PUT", "/api/v1/update", {"id": staged}, session, timeout=60)
            if status != 200:
                raise RuntimeError(f"platform apply failed: {status} {data[:300]!r}")
            rebooted = True
            mode = wait_reboot(cam, before, aborted=lambda: platform_stopped(cam, session))
            wait(lambda: json.loads(cam.platform("GET", "/api/v1/setup/status")[2]).get("release_sequence")
                 == local["release_sequence"], 300, "the new platform release")
            log(f"platform {local['release_version']} running")

        # ---- media layer
        if appfs:
            release = f"{RELEASES}/{appfs['release']}"
            cur_ini = cam.ssh(f"cat {INI}"); cur_env = cam.ssh(f"cat {ENV}")
            old = cur_env.split("JIPC_RELEASE=", 1)[1].split("\n", 1)[0]
            next_ini = ini_edit(cur_ini.replace(old + "/", release + "/"), ini_set, ini_unset)
            next_env = "".join(
                f"JIPC_RELEASE={release}\n" if line.startswith("JIPC_RELEASE=") else
                f"JIPC_APP_MD5={appfs['app_md5']}\n" if line.startswith("JIPC_APP_MD5=") else
                f"JIPC_IMP_MD5={appfs['imp_md5']}\n" if appfs.get("imp_md5") and line.startswith("JIPC_IMP_MD5=") else
                f"JIPC_LIBIMP_SHA256={appfs['imp_sha256']}\n"
                if appfs.get("imp_sha256") and line.startswith("JIPC_LIBIMP_SHA256=") else line + "\n"
                for line in cur_env.splitlines())
            write_remote(cam, INI + ".next", next_ini)
            write_remote(cam, ENV + ".next", next_env)
            cam.ssh(f"cp {HANDOFF} {HANDOFF}.next && chmod 700 {HANDOFF}.next")
            write_remote(cam, NATIVE_BOOT + ".next", (bundle / appfs["native_boot"]).read_bytes(), "700")
            cam.ssh("cat > /tmp/jipc-flash-candidate.squashfs", data=(bundle / appfs["image"]).read_bytes(), timeout=300)
            cam.ssh("cat > /tmp/upgrade-appfs.sh && chmod 700 /tmp/upgrade-appfs.sh",
                    data=(bundle / appfs["installer"]).read_bytes())
            current = cam.ssh("md5sum /dev/mtdblock4 | cut -c1-32").strip()
            log("flashing appfs (the installer verifies, keeps a RAM rollback copy and reboots)")
            before = cam.boot_id()
            cam.ssh(f"( trap '' HUP; exec sh /tmp/upgrade-appfs.sh {appfs['image_md5']} {current} ) "
                    "</dev/null >/tmp/jipc-upgrade.log 2>&1 &")
            rebooted = True
            # A precondition that fails changes nothing and reboots nothing: say so
            # now, not after waiting for a reboot with the NVR stopped.
            def aborted():
                try:
                    why = cam.ssh("grep ' ABORT: ' /tmp/jipc-upgrade.log || :", timeout=10).strip()
                    if why and not after_commit(why):  # the staged copies take up the small settings partition
                        cam.ssh(f"rm -f {INI}.next {ENV}.next {HANDOFF}.next {NATIVE_BOOT}.next; sync", timeout=20)
                    return why
                except Exception:
                    return ""
            mode = wait_reboot(cam, before, aborted)
            flashed = cam.ssh("md5sum /dev/mtdblock4 | cut -c1-32").strip()
            if flashed != appfs["image_md5"]:
                raise RuntimeError(f"appfs is {flashed} after reboot; the installer rolled back")
            log(f"media {appfs['release']} running")

        if config and not rebooted:
            log("rebooting to apply staged config")
            before = cam.boot_id()
            cam.ssh("sync; (sleep 1; reboot) </dev/null >/dev/null 2>&1 &", check=False)
            rebooted = True
            mode = wait_reboot(cam, before)
    except Exception:
        if not rebooted:
            restore()
        if nvr_stopped and args.nvr_start:
            log("starting the NVR again after the failure")
            subprocess.run(args.nvr_start, shell=True, check=False)
        raise

    # ---- verification
    session = cam.login()
    expected_ntp = config.get("verify_ntp")
    if expected_ntp:
        wait(lambda: json.loads(cam.platform("GET", "/api/v1/time", session=session)[2])["ntp"].get("synchronized"),
             120, "platform network time to synchronize")
    time_report = json.loads(cam.platform("GET", "/api/v1/time", session=session)[2])
    skew = time_report["epoch"] - time.time()
    status, media = cam.media_status()
    result = {
        "bundle": spec["name"], "installed_utc": stamp, "camera": args.camera,
        "platform": json.loads(cam.platform("GET", "/api/v1/setup/status")[2]),
        "platform_ntp": time_report["ntp"], "camera_clock_skew_seconds": round(skew, 1),
        "boot_mode": mode, "native_failed": cam.ssh("cat /opt/jooanipc-open/native-failed 2>/dev/null",
                                                     check=False).strip() or None,
        "media_ready": status == 200 and media.get("ready"), "media_clock": media.get("clock"),
        "homed": media.get("platform", {}).get("homed"),
        "appfs_md5": cam.ssh("md5sum /dev/mtdblock4 | cut -c1-32").strip(),
        "media_release": cam.ssh(f"sed -n 's/^JIPC_RELEASE=//p' {ENV}").strip(),
    }
    (bundle / f"deployed-{stamp}.json").write_text(json.dumps(result, indent=2) + "\n")
    for key, value in result.items():
        log(f"verified {key}: {value}")
    if args.nvr_start and (nvr_stopped or not args.nvr_stop):
        log("starting the NVR")
        subprocess.run(args.nvr_start, shell=True, check=True)
    if abs(skew) > 5 or not result["media_ready"]:
        raise SystemExit("verification failed: see the values above")
    if native_expected(facts, config) and mode != "native":
        raise SystemExit(f"installed, but this boot fell back to the {mode} path ({result['native_failed']}): "
                         "see 'A media change that stops native boot' in docs/RECOVERY.md")
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
