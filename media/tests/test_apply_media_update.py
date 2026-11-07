#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""apply-media-update.sh in a sandbox. It contains no reboot and no flash of its own: those
are in upgrade-appfs.sh, which here is a fixture that only records its arguments."""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

script = Path(sys.argv[1]).read_bytes()
md5 = lambda data: hashlib.md5(data).hexdigest()
OLD = "/mnt/mtd/jooanipc-open/releases/aaaaaaaa-bbbbbbbb"
NEW_RELEASE = "cccccccc-dddddddd"
NEW = f"/mnt/mtd/jooanipc-open/releases/{NEW_RELEASE}"
ENV = (f"JIPC_RELEASE={OLD}\nJIPC_LIBIMP_SHA256={'0' * 64}\nJIPC_APP_MD5={'1' * 32}\nJIPC_IMP_MD5={'2' * 32}\n"
       f"JIPC_ATOMIC_MD5={'3' * 32}\nJIPC_WIFI_BSSID=\n")
INI = f"backend=imp\nlibimp={OLD}/libimp.so\naudio=1\nbind=0.0.0.0\n"
CURRENT, IMAGE = b"the app partition now\n", b"the new image\n"


def world(base, upgrade_ok=True, **fields):
    root, package = base / "opt", base / "package"
    root.mkdir(); package.mkdir()
    (root / "deployment.env").write_text(ENV)
    (root / "camera.ini").write_text(INI)
    (root / "boot-handoff.sh").write_text("#!/bin/sh\necho handoff\n")
    (base / "appfs").write_bytes(CURRENT)
    (package / "apply-media-update.sh").write_bytes(script)
    (package / "appfs.img").write_bytes(IMAGE)
    # A fixture: it records how it was called and what was staged when it ran.
    (package / "upgrade-appfs.sh").write_text(
        f'#!/bin/sh\necho "$1 $2" > "{base}/upgrade-args"\n'
        f'ls "{root}" > "{base}/staged-then"\n'
        f'[ -e "{base}/candidate" ] && echo yes > "{base}/candidate-then"\n'
        f'exit {0 if upgrade_ok else 3}\n')
    values = dict(release_version=NEW_RELEASE, previous_appfs_md5=md5(CURRENT), appfs_md5=md5(IMAGE), app_md5="4" * 32,
                  imp_md5="5" * 32, imp_sha256="6" * 64, atomic_md5="7" * 32)
    values.update(fields)
    (package / "release.manifest").write_text(
        "JOOAN-SIGNED-RELEASE-V1\n" + "".join(f"{k}={v}\n" for k, v in values.items()) +
        "files-begin\nabc  release_version=decoy\nfiles-end\n")
    return root, package


def run(base, action):
    environment = {"JIPC_ROOT": str(base / "opt"), "JIPC_APPFS": str(base / "appfs"), "JIPC_CANDIDATE": str(base / "candidate"),
                   "JIPC_RUN": str(base / "run"), "JIPC_PLATFORM": str(base / "platform"), "JIPC_PLATFORM_RUN": str(base / "platform-run"),
                   "PATH": os.environ["PATH"]}
    (base / "run").mkdir(exist_ok=True)
    (base / "platform/state").mkdir(parents=True, exist_ok=True)
    (base / "platform-run").mkdir(exist_ok=True)
    marker = base / "run/qualified"
    if not marker.exists() and not (base / "not-qualified").exists():
        marker.write_text("boot pid\n")
    return subprocess.run(["sh", str(base / "package/apply-media-update.sh"), action], env=environment, capture_output=True, text=True, timeout=30)


def case(**kwargs):
    base = Path(tempfile.mkdtemp(prefix="apply-media-"))
    return base, world(base, **kwargs)


def refused(why, action="check", prepare=None, **fields):
    base, (root, _) = case(**fields)
    try:
        if prepare:
            prepare(base, root)
        before = sorted(p.name for p in root.iterdir())
        result = run(base, action)
        assert result.returncode != 0 and why in result.stdout, (why, result.stdout, result.stderr)
        assert sorted(p.name for p in root.iterdir()) == before, "a refusal changes nothing"
        assert not (base / "upgrade-args").exists() and (base / "package/appfs.img").exists()
    finally:
        shutil.rmtree(base)


# check passes on a good package, and changes nothing.
base, (root, package) = case()
result = run(base, "check")
assert result.returncode == 0, result.stdout + result.stderr
assert sorted(p.name for p in root.iterdir()) == ["boot-handoff.sh", "camera.ini", "deployment.env"]
assert not (base / "upgrade-args").exists() and (package / "appfs.img").exists()

# apply stages what the boot checks against, moves the image, and hands over.
result = run(base, "apply")
assert result.returncode == 0, result.stdout + result.stderr
assert (base / "upgrade-args").read_text().split() == [md5(IMAGE), md5(CURRENT)]
env = (root / "deployment.env.next").read_text()
assert f"JIPC_RELEASE={NEW}\n" in env and f"JIPC_APP_MD5={'4' * 32}\n" in env and f"JIPC_IMP_MD5={'5' * 32}\n" in env
assert f"JIPC_LIBIMP_SHA256={'6' * 64}\n" in env and f"JIPC_ATOMIC_MD5={'7' * 32}\n" in env and "JIPC_WIFI_BSSID=\n" in env
ini = (root / "camera.ini.next").read_text()
assert f"libimp={NEW}/libimp.so\n" in ini and "audio=1\n" in ini and OLD not in ini
assert (root / "boot-handoff.sh.next").read_text() == (root / "boot-handoff.sh").read_text()
assert (base / "candidate").read_bytes() == IMAGE and (base / "candidate-then").exists()
assert "deployment.env.next" in (base / "staged-then").read_text()
assert (root / "deployment.env").read_text() == ENV, "the live files are upgrade-appfs.sh's to commit"
shutil.rmtree(base)

# If the flash script refuses, everything staged goes, and the package can be tried again.
base, (root, package) = case(upgrade_ok=False)
result = run(base, "apply")
assert result.returncode == 3, (result.returncode, result.stdout, result.stderr)
assert sorted(p.name for p in root.iterdir()) == ["boot-handoff.sh", "camera.ini", "deployment.env"]
assert (package / "appfs.img").read_bytes() == IMAGE and not (base / "candidate").exists()
shutil.rmtree(base)

# What it refuses, for check and apply alike.
for action in ("check", "apply"):
    refused("different copy", action, previous_appfs_md5="9" * 32)
    refused("does not match", action, appfs_md5="9" * 32)
    refused("no valid release", action, release_version="../../etc")
    refused("no valid release", action, release_version="CCCCCCCC-DDDDDDDD")
    refused("missing a checksum", action, app_md5="short")
    refused("missing a checksum", action, imp_sha256="short")
    refused("already installed", action, release_version="aaaaaaaa-bbbbbbbb")
    refused("failed start", action, prepare=lambda b, r: (r / "boot-trial").write_text(""))
    refused("failed start", action, prepare=lambda b, r: (r / "native-failed").write_text("x"))
    refused("no media release", action, prepare=lambda b, r: (r / "deployment.env").write_text("JIPC_WIFI_BSSID=\n"))
# Nothing starts while this boot is proving itself, a Wi-Fi trial is on or a platform update runs.
def refused_now(why, prepare):
    base, _ = case()
    prepare(base)
    result = run(base, "check")
    assert result.returncode != 0 and why in result.stdout + result.stderr, (why, result.stdout, result.stderr)
refused_now("still checking its last start", lambda base: (base / "not-qualified").write_text(""))
refused_now("being tested", lambda base: (base / "platform/state").mkdir(parents=True) or (base / "platform/state/wifi-trial").mkdir())
refused_now("platform update", lambda base: (base / "platform-run/firmware-apply.lock").mkdir(parents=True))
print("apply-media-update: PASS")
