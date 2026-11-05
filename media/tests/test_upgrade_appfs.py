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
print("appfs upgrade script: PASS")
