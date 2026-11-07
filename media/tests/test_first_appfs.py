#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static invariants of scripts/first-appfs.sh, and build-appfs.sh --first on a made-up stock
image. first-appfs.sh is only parsed here, never run: it would stop processes and reboot."""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

first, builder = Path(sys.argv[1]), Path(sys.argv[2])
text = first.read_text()
subprocess.run(["sh", "-n", str(first)], check=True)
commit = text.index("# Commit point")
before, after = text[:commit], text[commit:]
stop = text.index('stop_owner "$watch"')
# The platform's lock is taken before anything stops, and never released.
assert text.index('mkdir "$platform_lock"') < stop < commit
assert 'rm -rf "$platform_lock"' not in text and "rmdir" not in text
# Before the commit point, once something has stopped, every failure goes back to the
# untouched image: a plain exit there would leave a camera with no application running.
assert "die " not in before[stop:]
# The watchdog is handed over first, and checked, before the application stops.
assert stop < text.index("/sbin/watchdog -F") < text.index("holds_watchdog \"$feeder\"") < text.index('stop_owner "$oem"')
# Nothing is flashed until the image has been checked, the partition is unused and a
# rollback copy is verified in RAM.
flash = text.index('/sbin/flashcp -v "$image"')
for step in ('[ "$(hash "$image")" = "$1" ]', '[ "$(hash /dev/mtdblock4)" = "$2" ]', 'cat /dev/mtdblock4 > "$backup"',
             '[ "$(hash "$backup")" = "$2" ]', "appfs mapping remains", "appfs descriptor/path remains", "umount /mnt/mtd"):
    assert text.index(step) < flash, step
# The boot flags only exist once the flash is about to happen, and go if it is undone.
assert ': > "$root/native"' in after and ': > "$root/native"' not in before
assert 'rm -f "$root/boot-handoff.sh" "$root/camera.ini" "$root/deployment.env" "$root/enabled" "$root/native"' in text
# A failed flash is rolled back with retries; a reboot only follows a verified image or rollback.
assert 'until /sbin/flashcp -v "$backup" /dev/mtd4 && [ "$(hash /dev/mtd4)" = "$2" ]' in after
assert after.index("restore_config") < after.index("/bin/busybox reboot -f")
# It refuses a camera that already has a media layer.
assert 'die \'this camera already has a media layer\'' in before and "a media daemon is already running" in before
# Past the first stop an exit of any kind must end in a reboot, never in a camera with
# nothing running; the commit point undoes the configuration too.
assert "trap on_exit EXIT" in text and text.index("trap on_exit EXIT") < stop
assert 'phase=stopped' in before and 'phase=commit' in after and 'phase=flash' in after
assert after.index('phase=commit') < after.index('mv "$root/$name.next"') < after.index('phase=flash') < after.index('/sbin/flashcp -v "$image"')
# A platform update or a Wi-Fi trial cannot be under way.
assert "firmware-apply.lock" in before and "wifi-trial" in before
print("first-appfs script: PASS")

if not shutil.which("mksquashfs") or not shutil.which("unsquashfs"):
    print("build-appfs --first: SKIPPED (no squashfs-tools)")
    sys.exit(0)

with tempfile.TemporaryDirectory(prefix="appfs-first-") as temporary:
    base = Path(temporary)
    tree = base / "stock"
    for path, data in {"run/jooanipc": b"vendor program\n", "startapp": b"#!/bin/sh\n", "lib/keep.so": b"library\n",
                       "VoiceGuide/EN/hello.mp3": b"not needed\n", "lib/drop.so": b"not needed\n"}.items():
        (tree / path).parent.mkdir(parents=True, exist_ok=True)
        (tree / path).write_bytes(data)
    stock = base / "stock.img"
    subprocess.run(["mksquashfs", str(tree), str(stock), "-comp", "xz", "-noappend", "-quiet", "-all-root"], check=True, stdout=subprocess.DEVNULL)
    pad = 3211264 - stock.stat().st_size
    stock.write_bytes(stock.read_bytes() + b"\xff" * pad)
    keep = base / "keep.txt"
    keep.write_text("# what stays\nrun\nrun/jooanipc\nstartapp\nlib\nlib/keep.so\n")
    sha = lambda data: hashlib.sha256(data).hexdigest()
    pins = base / "target.json"
    def write_pins(program=b"vendor program\n"):
        pins.write_text(json.dumps({"p": {"/mnt/mtd/run/jooanipc": sha(program), "/mnt/mtd/startapp": sha(b"#!/bin/sh\n")}}, indent=2))
    write_pins()
    for name, data in (("bin", b"app\n"), ("libimp", b"imp\n"), ("atomic", b"atomic\n")):
        (base / name).write_bytes(data)
    def build(out):
        environment = {"JIPC_APPFS_KEEP": str(keep), "JIPC_TARGET_JSON": str(pins), "PATH": "/usr/sbin:/usr/bin:/sbin:/bin"}
        return subprocess.run(["sh", str(builder), "--first", str(stock), str(base / "bin"), str(base / "libimp"), str(base / "atomic"), str(out)],
                              env=environment, capture_output=True, text=True, timeout=120)
    out = base / "out"
    result = build(out)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (out / "appfs.img").stat().st_size == 3211264
    spec = json.loads((out / "bundle.json").read_text())["jooanipc_open"]
    assert spec["expected_current_appfs_md5"] == hashlib.md5(stock.read_bytes()).hexdigest()
    assert spec["image_md5"] == hashlib.md5((out / "appfs.img").read_bytes()).hexdigest()
    assert spec["first"] is True and spec["installer"] == "first-appfs.sh"
    assert spec["imp_sha256"] == sha(b"imp\n") and spec["atomic_md5"] == hashlib.md5(b"atomic\n").hexdigest()
    assert (out / "first-appfs.sh").exists() and (out / "boot-handoff.sh").exists() and (out / "native-boot.sh").exists()
    listing = subprocess.run(["unsquashfs", "-l", str(out / "appfs.img")], capture_output=True, text=True).stdout
    release = spec["release"]
    assert f"jooanipc-open/releases/{release}/libimp.so" in listing and "run/jooanipc" in listing and "lib/keep.so" in listing
    assert "VoiceGuide" not in listing and "drop.so" not in listing
    # A stock image it does not recognise is refused, with nothing to install.
    write_pins(b"another program\n")
    result = build(base / "out2")
    assert result.returncode != 0 and "pinned" in result.stderr, result.stderr
    write_pins()
    keep.write_text(keep.read_text() + "lib/missing.so\n")
    result = build(base / "out3")
    assert result.returncode != 0 and "lacks" in result.stderr, result.stderr
print("build-appfs --first: PASS")
