#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static invariants of scripts/upgrade-appfs.sh; no device access."""
import subprocess
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
subprocess.run(["sh", "-n", sys.argv[1]], check=True)
commit = text.index("# Commit point")
# The platform maintenance lock is taken before media stops and never
# released: it keeps the network watchdog from rebooting mid-flash, and after
# a failed rollback it keeps the camera up instead of booting a damaged appfs.
lock = text.index('mkdir "$platform_lock"')
assert lock < text.index('stop_owner "$native"') < commit
assert 'rm -rf "$platform_lock"' not in text and "rmdir" not in text
# Before the commit point every failure reboots into the untouched appfs.
assert "die " not in text[text.index('stop_owner "$native"'):commit]
# After it, a failed flash is rolled back with retries, and the script only
# reboots after a verified image or a verified rollback.
after = text[commit:]
assert "until /sbin/flashcp -v \"$backup\" /dev/mtd4 && [ \"$(hash /dev/mtd4)\" = \"$2\" ]" in after
assert after.index("restore_config") < after.index("/bin/busybox reboot -f")
assert "OEM->native installer" not in text
# The boot script goes with its release, staged, checked, committed and restored like the rest:
# an update that left it behind kept every fix to it from reaching the camera.
assert 'die \'no platform runtime on flash' in text[:text.index('stop_owner "$native"')]
names = "for name in boot-handoff.sh native-boot.sh camera.ini deployment.env; do"
assert text.count(names) == 3 and "for name in boot-handoff.sh camera.ini" not in text
assert 'sh -n "$root/native-boot.sh.next"' in text[:commit]
assert text.count('chmod 700 "$root/boot-handoff.sh" "$root/native-boot.sh"') == 2
print("appfs upgrade script: PASS")
