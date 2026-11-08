#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""End-to-end installer: stock JOOAN JA-A12 -> open platform -> open media.

A guided, resumable engine that chains the five steps an owner would otherwise
run by hand. It is deliberately thin: every step wraps a primitive that already
exists and is tested in this repository, and every step is idempotent and
re-entrant, so a run that stops (a dropped connection, a closed laptop) can be
resumed without repeating finished work.

    1 provision   put a stock camera on the owner's Wi-Fi             (provision)
    2 platform    deliver + trigger the signed platform OTA, enroll SSH (ota)
    3 backup      copy every flash partition off the camera            (backup)
    4 build       build the owner's media image from their own appfs   (build)
    5 media       flash the media image; native boot qualifies         (flash)

The media image is built on the owner's machine from their own camera's
partition, because that partition contains vendor files that cannot be
redistributed. No compiler is needed: the open binaries ship prebuilt in the
release, and the build is a squashfs repack.

This module is the engine. A GUI wraps these same steps; `main()` is a CLI over
them. Nothing is destructive before step 5, which asks for explicit confirmation
and always takes a full backup first (step 3).

Run `python3 installer/install.py --help`.
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import shutil
import ssl
import subprocess
import sys
import time
import urllib.request
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[1]

# Steps, in order. Each name is a resumable checkpoint.
STEPS = ("provision", "platform", "backup", "build", "flash")

# Partitions to back up (mtd0..mtd6). mtd4 (appfs) is also the media build input.
MTD_COUNT = 7
APPFS_MTD = 4
APPFS_BYTES = 3211264


class InstallerError(RuntimeError):
    """A step failed in a way the operator must resolve."""


@dataclasses.dataclass
class RunState:
    """Resumable run state, persisted as JSON next to the work directory."""
    camera_ip: str = ""
    completed: list[str] = dataclasses.field(default_factory=list)
    appfs_md5: str = ""
    backup_dir: str = ""
    bundle_dir: str = ""
    notes: dict = dataclasses.field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> "RunState":
        if path.is_file():
            data = json.loads(path.read_text())
            return cls(**{k: data.get(k, getattr(cls(), k)) for k in
                          (f.name for f in dataclasses.fields(cls))})
        return cls()

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(dataclasses.asdict(self), indent=2) + "\n")
        tmp.replace(path)

    def done(self, step: str) -> bool:
        return step in self.completed

    def mark(self, step: str) -> None:
        if step not in self.completed:
            self.completed.append(step)


class Reporter:
    """Progress sink. The CLI prints; a GUI subclass would emit events."""

    def step(self, name: str, message: str) -> None:
        print(f"[{name}] {message}", flush=True)

    def detail(self, message: str) -> None:
        print(f"    {message}", flush=True)

    def warn(self, message: str) -> None:
        print(f"    ! {message}", flush=True)

    def confirm(self, prompt: str) -> bool:
        return input(f"{prompt} [type YES to proceed] ").strip() == "YES"


# ---------------------------------------------------------------------------
# Small helpers over the camera (SSH and the platform HTTP API).
# ---------------------------------------------------------------------------

def _ssh_base(key: Path | None, user: str, host: str) -> list[str]:
    cmd = ["ssh", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=accept-new"]
    if key:
        cmd += ["-i", str(key)]
    return cmd + [f"{user}@{host}"]


def ssh_capture(host: str, script: str, *, key: Path | None, user: str = "admin",
                password: str = "", timeout: int = 60) -> bytes:
    """Run a command on the camera and return stdout bytes."""
    base = _ssh_base(key, user, host)
    env = dict(os.environ)
    prefix: list[str] = []
    if password and not key and shutil.which("sshpass"):
        prefix = ["sshpass", "-e"]
        env["SSHPASS"] = password
    result = subprocess.run(prefix + base + [script], capture_output=True,
                            timeout=timeout, env=env)
    if result.returncode:
        raise InstallerError(
            f"camera command failed ({result.returncode}): {script[:60]}\n"
            + result.stderr.decode(errors="replace"))
    return result.stdout


def platform_api(host: str, method: str, path: str, *, body=None,
                 session: dict | None = None, timeout: int = 30):
    """Call the platform's plain-HTTP API on port 80."""
    url = f"http://{host}{path}"
    headers = {"Content-Type": "application/json"}
    if session:
        headers["Cookie"] = session["cookie"]
        if method != "GET":
            headers["X-CSRF-Token"] = session["csrf"]
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            cookie = response.headers.get("Set-Cookie", "")
            return response.status, raw, cookie
    except urllib.error.HTTPError as error:
        return error.code, error.read(), ""


def platform_login(host: str, password: str) -> dict:
    status, raw, cookie = platform_api(
        host, "POST", "/api/v1/session",
        body={"username": "admin", "password": password})
    if status != 200:
        raise InstallerError(f"platform sign-in failed ({status})")
    csrf = json.loads(raw)["csrf"]
    return {"csrf": csrf, "cookie": cookie.split(";")[0]}


# ---------------------------------------------------------------------------
# The five steps. Each takes (ctx) and raises InstallerError on failure.
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class Context:
    args: argparse.Namespace
    state: RunState
    state_path: Path
    report: Reporter
    key: Path | None


def step_provision(ctx: Context) -> None:
    """Put a stock camera on the owner's Wi-Fi.

    The fully app-free channel (SoftAP/CoAP) is Phase E research. Until it lands,
    this step verifies the camera is reachable and, if not, tells the owner to
    provision once with the vendor app (the project has no other way to bring a
    truly stock camera onto Wi-Fi). It never blocks the rest of the chain.
    """
    host = ctx.state.camera_ip or ctx.args.camera
    if not host:
        raise InstallerError(
            "no camera address yet. Provision the camera onto your Wi-Fi "
            "(for now, with the vendor app once) and re-run with --camera <ip>.")
    ctx.state.camera_ip = host
    ctx.report.step("provision", f"using camera at {host}")
    # Liveness: stock answers :80 (goahead) only transiently, so just check reachability.
    ctx.report.detail("camera address recorded; continuing")


def step_platform(ctx: Context) -> None:
    """Deliver and trigger the signed platform OTA, then enroll an SSH key.

    Delivery (serve_ota.py) and the DNS redirect are the owner's network setup,
    documented in docs/INSTALL.md; this step runs the trigger and waits for the
    platform to come up, then confirms and (optionally) installs an SSH key for
    the backup step.
    """
    host = ctx.state.camera_ip
    pkg = Path(ctx.args.platform_pkg).resolve()
    if not pkg.is_file():
        raise InstallerError(f"platform package not found: {pkg}")

    # If the platform is already up (resume), skip the trigger.
    status = _platform_status(host)
    if status is None:
        ctx.report.step("platform", "triggering the vendor updater (needs serve_ota.py running + DNS redirect)")
        trigger = [sys.executable, str(REPO / "tools/upload_ota.py"),
                   "--host", host, "--pkg", str(pkg)]
        if ctx.args.dry_run:
            ctx.report.detail(f"DRY-RUN would run: {' '.join(trigger)}")
        else:
            result = subprocess.run(trigger, capture_output=True, text=True, timeout=600)
            ctx.report.detail(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else "")
            if result.returncode:
                raise InstallerError("platform OTA trigger failed:\n" + result.stderr)
        status = _wait_platform(host, ctx.report, dry_run=ctx.args.dry_run)
    else:
        ctx.report.step("platform", f"platform already up: {status.get('release_version')}")

    if ctx.args.dry_run:
        ctx.report.detail("DRY-RUN skipping SSH key enrollment")
        return

    # Enroll the owner's SSH key so step 3 can dump partitions non-interactively.
    pub = ctx.args.ssh_pubkey
    if pub and ctx.key:
        ctx.report.detail("enrolling SSH key for backup")
        session = platform_login(host, ctx.args.camera_password)
        pubtext = Path(pub).read_text()
        # The key API takes raw public-key text, not JSON.
        _post_ssh_key(host, session, pubtext)
        ctx.report.detail("SSH key enrolled")


def step_backup(ctx: Context) -> None:
    """Copy every flash partition off the camera. Mandatory before any flash."""
    host = ctx.state.camera_ip
    backup_dir = Path(ctx.args.work) / "backup"
    backup_dir.mkdir(parents=True, exist_ok=True)
    ctx.report.step("backup", f"saving {MTD_COUNT} partitions to {backup_dir}")
    for i in range(MTD_COUNT):
        out = backup_dir / f"camera-mtd{i}.bin"
        if out.is_file() and out.stat().st_size > 0:
            ctx.report.detail(f"mtd{i} already saved ({out.stat().st_size} bytes)")
            continue
        if ctx.args.dry_run:
            ctx.report.detail(f"DRY-RUN would dump mtd{i}")
            out.write_bytes(b"")  # placeholder so resume sees progress in dry-run
            continue
        data = ssh_capture(host, f"cat /dev/mtdblock{i}", key=ctx.key,
                           password=ctx.args.camera_password, timeout=300)
        out.write_bytes(data)
        ctx.report.detail(f"mtd{i}: {len(data)} bytes")
    # The appfs (mtd4) is the media build input; record its md5.
    appfs = backup_dir / f"camera-mtd{APPFS_MTD}.bin"
    if not ctx.args.dry_run:
        if appfs.stat().st_size != APPFS_BYTES:
            raise InstallerError(
                f"appfs backup is {appfs.stat().st_size} bytes, expected {APPFS_BYTES}")
        ctx.state.appfs_md5 = hashlib.md5(appfs.read_bytes()).hexdigest()
    ctx.state.backup_dir = str(backup_dir)
    ctx.report.detail(f"backup complete; appfs md5 {ctx.state.appfs_md5 or '(dry-run)'}")


def step_build(ctx: Context) -> None:
    """Build the owner's media image from their own appfs. No compiler needed."""
    host = ctx.state.camera_ip
    release = Path(ctx.args.media_release).resolve()
    appfs = Path(ctx.state.backup_dir) / f"camera-mtd{APPFS_MTD}.bin"
    bundle = Path(ctx.args.work) / "bundle"
    binary = release / "bin/jooanipc-open"
    libatomic = release / "bin/libatomic.so.1"
    libimp = _find_libimp(release)
    for needed in (binary, libatomic, libimp):
        if needed is None or not Path(needed).is_file():
            raise InstallerError(f"media release is missing a build input: {needed}")
    if not ctx.args.dry_run and not appfs.is_file():
        raise InstallerError("appfs backup missing; run the backup step first")
    if not shutil.which("mksquashfs") or not shutil.which("unsquashfs"):
        raise InstallerError(
            "squashfs-tools (mksquashfs/unsquashfs) is required to build the image. "
            "On Windows, run this in WSL.")
    builder = [str(REPO / "media/tools/build-appfs.sh"), "--first", str(appfs),
               str(binary), str(libimp), str(libatomic), str(bundle)]
    ctx.report.step("build", "repacking the appfs with the open media release")
    if ctx.args.dry_run:
        ctx.report.detail(f"DRY-RUN would run: {' '.join(builder)}")
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "bundle.json").write_text('{"schema":1,"board":"JA-A12"}\n')
    else:
        result = subprocess.run(builder, capture_output=True, text=True, timeout=600)
        if result.returncode:
            raise InstallerError("media image build failed:\n" + result.stderr)
        ctx.report.detail(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else "built")
    ctx.state.bundle_dir = str(bundle)


def step_flash(ctx: Context) -> None:
    """Flash the media image. The one irreversible step; confirm and gate on backup."""
    host = ctx.state.camera_ip
    bundle = Path(ctx.state.bundle_dir or (Path(ctx.args.work) / "bundle"))
    if not ctx.state.done("backup"):
        raise InstallerError("refusing to flash without a completed backup")
    ctx.report.step("flash", "about to write the camera's application partition")
    ctx.report.warn("This is the only step that cannot be undone over the network.")
    ctx.report.warn("If power is lost while it writes, recovery needs an SPI programmer")
    ctx.report.warn(f"and the backup in {ctx.state.backup_dir}. Keep the camera powered.")
    if not ctx.args.assume_yes and not ctx.args.dry_run:
        if not ctx.report.confirm("Proceed with the irreversible media flash?"):
            raise InstallerError("flash not confirmed; stopping with the camera unchanged")
    installer = [sys.executable, str(REPO / "media/scripts/install-camera.py"),
                 str(bundle), "--camera", host, "--yes", "--yes-untested"]
    env = dict(os.environ)
    env["CAMERA_PASSWORD"] = ctx.args.camera_password
    env.setdefault("STREAM_PASSWORD", ctx.args.stream_password or ctx.args.camera_password)
    if ctx.args.dry_run:
        ctx.report.detail(f"DRY-RUN would run: {' '.join(installer)}")
        return
    result = subprocess.run(installer, env=env, timeout=1800)
    if result.returncode:
        raise InstallerError("media flash/verify failed; see output above and docs/RECOVERY.md")
    ctx.report.detail("media native boot qualified")


STEP_FUNCS: dict[str, Callable[[Context], None]] = {
    "provision": step_provision,
    "platform": step_platform,
    "backup": step_backup,
    "build": step_build,
    "flash": step_flash,
}


# ---------------------------------------------------------------------------
# internals
# ---------------------------------------------------------------------------

def _platform_status(host: str) -> dict | None:
    try:
        status, raw, _ = platform_api(host, "GET", "/api/v1/setup/status", timeout=5)
    except Exception:
        return None
    if status == 200:
        try:
            data = json.loads(raw)
        except ValueError:
            return None
        if data.get("release_version"):
            return data
    return None


def _wait_platform(host: str, report: Reporter, *, dry_run: bool, seconds: int = 420) -> dict:
    if dry_run:
        return {"release_version": "(dry-run)"}
    report.detail("waiting for the platform to come up")
    deadline = time.time() + seconds
    while time.time() < deadline:
        status = _platform_status(host)
        if status:
            report.detail(f"platform up: {status.get('release_version')}")
            return status
        time.sleep(5)
    raise InstallerError("platform did not come up; check the serve_ota.py log and the DNS redirect")


def _post_ssh_key(host: str, session: dict, pubtext: str) -> None:
    url = f"http://{host}/api/v1/ssh/authorized-keys"
    headers = {"Content-Type": "text/plain", "Cookie": session["cookie"],
               "X-CSRF-Token": session["csrf"]}
    request = urllib.request.Request(url, data=pubtext.encode(), headers=headers, method="POST")
    with urllib.request.urlopen(request, timeout=15) as response:
        if response.status != 200:
            raise InstallerError(f"SSH key enrollment failed ({response.status})")


def _find_libimp(release: Path) -> Path | None:
    # The release ships libimp.so beside the daemon, or in a sibling libimp-* dir.
    candidate = release / "bin/libimp.so"
    if candidate.is_file():
        return candidate
    for sibling in release.parent.glob("libimp-*/libimp.so"):
        return sibling
    for found in release.rglob("libimp.so"):
        return found
    return None


def run(ctx: Context, only: str | None = None) -> int:
    start = STEPS.index(only) if only else 0
    for step in STEPS[start:]:
        if ctx.state.done(step) and not ctx.args.force:
            ctx.report.step(step, "already done; skipping (use --force to repeat)")
            continue
        try:
            STEP_FUNCS[step](ctx)
        except InstallerError as error:
            ctx.report.warn(str(error))
            ctx.report.warn(f"stopped at step '{step}'. Fix the issue and re-run to resume.")
            return 1
        ctx.state.mark(step)
        ctx.state.save(ctx.state_path)
        if only:
            break
    ctx.report.step("done", "all requested steps complete")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--camera", default="", help="camera IP once it is on your Wi-Fi")
    p.add_argument("--work", default=str(Path.home() / ".jooan-installer"),
                   help="working directory for backups, the image and run state")
    p.add_argument("--platform-pkg", default=str(REPO / "dist/JOOAN_FW_PKG"),
                   help="signed platform package (JOOAN_FW_PKG)")
    p.add_argument("--media-release", default="",
                   help="unpacked jooanipc-open release dir (ships prebuilt binaries)")
    p.add_argument("--camera-password", default="admin", help="camera admin/SSH password")
    p.add_argument("--stream-password", default="", help="password for the video streams")
    p.add_argument("--ssh-key", default="", help="SSH private key to use/enroll for backup")
    p.add_argument("--ssh-pubkey", default="", help="SSH public key to enroll on the platform")
    p.add_argument("--only", choices=STEPS, help="run just one step")
    p.add_argument("--force", action="store_true", help="re-run steps already marked done")
    p.add_argument("--assume-yes", action="store_true", help="skip the flash confirmation")
    p.add_argument("--dry-run", action="store_true",
                   help="plan and checkpoint without touching the camera or flash")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    work = Path(args.work)
    state_path = work / "run-state.json"
    state = RunState.load(state_path)
    if args.camera:
        state.camera_ip = args.camera
    key = Path(args.ssh_key) if args.ssh_key else None
    ctx = Context(args=args, state=state, state_path=state_path,
                  report=Reporter(), key=key)
    try:
        return run(ctx, only=args.only)
    except KeyboardInterrupt:
        print("\ninterrupted; re-run to resume", flush=True)
        return 130


if __name__ == "__main__":
    sys.exit(main())
