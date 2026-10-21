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

Credentials come from the environment: CAMERA_PASSWORD (SSH and the platform
API) and MEDIA_PASSWORD (jooanipc-open's API; defaults to CAMERA_PASSWORD).
SSH uses sshpass when it is installed and CAMERA_PASSWORD is set.
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
import time

INI = "/opt/jooanipc-open/camera.ini"
ENV = "/opt/jooanipc-open/deployment.env"
HANDOFF = "/opt/jooanipc-open/boot-handoff.sh"
RELEASES = "/mnt/mtd/jooanipc-open/releases"


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


class Camera:
    def __init__(self, host, password, media_password):
        self.host, self.password, self.media_password = host, password, media_password
        self.prefix = (["sshpass", "-e"] if password and shutil.which("sshpass") else [])

    def ssh_bytes(self, script, data=None, timeout=120, check=True):
        env = os.environ | ({"SSHPASS": self.password} if self.prefix else {})
        command = self.prefix + ["ssh", "-o", "ConnectTimeout=8", "-o", "ServerAliveInterval=5",
                                 "-o", "ServerAliveCountMax=3", f"admin@{self.host}", script]
        result = subprocess.run(command, input=data, capture_output=True, timeout=timeout, env=env)
        if check and result.returncode:
            raise RuntimeError(f"camera command failed ({result.returncode}): {script[:80]}\n"
                               + result.stderr.decode(errors="replace"))
        return result.stdout

    def ssh(self, script, data=None, timeout=120, check=True):
        return self.ssh_bytes(script, data, timeout, check).decode(errors="replace")

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

    def media_status(self):
        context = ssl.create_default_context()
        context.check_hostname, context.verify_mode = False, ssl.CERT_NONE
        c = http.client.HTTPSConnection(self.host, 18443, timeout=15, context=context)
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


def wait_reboot(cam, seconds=600):
    # A platform apply runs its whole maintenance transaction before rebooting.
    wait(lambda: not cam.up(), 600, "the camera to go down", step=2)
    log("camera rebooting")
    wait(cam.up, seconds, "SSH after reboot")
    log("SSH is back; waiting for the boot handoff")
    wait(lambda: "boot handoff qualified" in cam.ssh("cat /tmp/jipc-handoff.log 2>/dev/null", check=False),
         seconds, "the jooanipc-open handoff to qualify", step=5)
    log("handoff qualified")


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
    args = p.parse_args()
    bundle = args.bundle.resolve()
    spec = json.loads((bundle / "bundle.json").read_text())
    if spec.get("schema") != 1 or spec.get("board") != "JA-A12":
        raise SystemExit("unsupported bundle (schema 1, board JA-A12 expected)")
    password = os.environ.get("CAMERA_PASSWORD", "")
    cam = Camera(args.camera, password, os.environ.get("MEDIA_PASSWORD", password))
    local, appfs, config = spec.get("jooan_local"), spec.get("jooanipc_open"), spec.get("config", {})

    # ---- artifacts on this host
    if local:
        if sha256(bundle / local["package"]) != local["package_sha256"]:
            raise SystemExit("jooan_local package checksum mismatch")
    if appfs:
        for name, field in ((appfs["image"], "image_md5"), (appfs["installer"], "installer_md5")):
            if md5(bundle / name) != appfs[field]:
                raise SystemExit(f"{name} checksum mismatch")
        if (bundle / appfs["image"]).stat().st_size != 3211264:
            raise SystemExit("appfs image must be exactly the 3211264-byte partition")
    log(f"bundle {spec['name']}: artifacts verified")

    # ---- preflight on the camera
    facts = dict(line.split("=", 1) for line in cam.ssh(
        "echo appfs_size=$(cat /sys/class/mtd/mtd4/size); echo appfs_name=$(cat /sys/class/mtd/mtd4/name);"
        "echo appfs_md5=$(md5sum /dev/mtdblock4 | cut -c1-32);"
        "echo opt_free=$(df -k /opt | awk 'END {print $4}');"
        "echo trial=$([ -e /opt/jooanipc-open/boot-trial ] && echo yes || echo no);"
        "echo qualified=$(grep -c 'boot handoff qualified' /tmp/jipc-handoff.log 2>/dev/null);"
        "echo release=$(sed -n 's/^JIPC_RELEASE=//p' " + ENV + ")").splitlines() if "=" in line)
    status, _, data = cam.platform("GET", "/api/v1/setup/status")
    platform = json.loads(data) if status == 200 else {}
    problems = []
    if facts.get("appfs_size") != "3211264" or facts.get("appfs_name") != "appfs":
        problems.append("not the qualified JA-A12 flash layout")
    if int(facts.get("opt_free", 0)) < 64:
        problems.append("/opt has less than 64 KiB free")
    if facts.get("trial") != "no":
        problems.append("a failed jooanipc-open boot trial is latched")
    if facts.get("qualified", "0") == "0":
        problems.append("jooanipc-open did not qualify on this boot")
    if local and platform.get("release_sequence", 0) >= local["release_sequence"]:
        problems.append(f"platform already at sequence {platform.get('release_sequence')}")
    if appfs and facts.get("appfs_md5") != appfs["expected_current_appfs_md5"]:
        problems.append(f"appfs is {facts.get('appfs_md5')}, bundle expects {appfs['expected_current_appfs_md5']}")
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
    try:
        # ---- config, staged before any reboot
        if config.get("camera_ini"):
            ini = saved[INI].decode()
            write_remote(cam, INI, ini_edit(ini, config["camera_ini"].get("set", {}),
                                            set(config["camera_ini"].get("unset", []))))
        for path, content in config.get("files", {}).items():
            if content is None:
                cam.ssh(f"rm -f {shlex.quote(path)} && sync")
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
            status, _, data = cam.platform("PUT", "/api/v1/update", {"id": staged}, session, timeout=60)
            if status != 200:
                raise RuntimeError(f"platform apply failed: {status} {data[:300]!r}")
            rebooted = True
            wait_reboot(cam)
            wait(lambda: json.loads(cam.platform("GET", "/api/v1/setup/status")[2]).get("release_sequence")
                 == local["release_sequence"], 300, "the new platform release")
            log(f"platform {local['release_version']} running")

        # ---- media layer
        if appfs:
            release = f"{RELEASES}/{appfs['release']}"
            cur_ini = cam.ssh(f"cat {INI}"); cur_env = cam.ssh(f"cat {ENV}")
            old = cur_env.split("JIPC_RELEASE=", 1)[1].split("\n", 1)[0]
            next_ini = cur_ini.replace(old + "/", release + "/")
            next_env = "".join(
                f"JIPC_RELEASE={release}\n" if line.startswith("JIPC_RELEASE=") else
                f"JIPC_APP_MD5={appfs['app_md5']}\n" if line.startswith("JIPC_APP_MD5=") else line + "\n"
                for line in cur_env.splitlines())
            write_remote(cam, INI + ".next", next_ini)
            write_remote(cam, ENV + ".next", next_env)
            cam.ssh(f"cp {HANDOFF} {HANDOFF}.next && chmod 700 {HANDOFF}.next")
            cam.ssh("cat > /tmp/jipc-flash-candidate.squashfs", data=(bundle / appfs["image"]).read_bytes(), timeout=300)
            cam.ssh("cat > /tmp/upgrade-appfs.sh && chmod 700 /tmp/upgrade-appfs.sh",
                    data=(bundle / appfs["installer"]).read_bytes())
            current = cam.ssh("md5sum /dev/mtdblock4 | cut -c1-32").strip()
            log("flashing appfs (the installer verifies, keeps a RAM rollback copy and reboots)")
            cam.ssh(f"( trap '' HUP; exec sh /tmp/upgrade-appfs.sh {appfs['image_md5']} {current} ) "
                    "</dev/null >/tmp/jipc-upgrade.log 2>&1 &")
            rebooted = True
            wait_reboot(cam)
            flashed = cam.ssh("md5sum /dev/mtdblock4 | cut -c1-32").strip()
            if flashed != appfs["image_md5"]:
                raise RuntimeError(f"appfs is {flashed} after reboot; the installer rolled back")
            log(f"media {appfs['release']} running")

        if config and not rebooted:
            log("rebooting to apply staged config")
            cam.ssh("sync; (sleep 1; reboot) >/dev/null 2>&1 &", check=False)
            rebooted = True
            wait_reboot(cam)
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
        "media_ready": status == 200 and media.get("ready"), "media_clock": media.get("clock"),
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
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
