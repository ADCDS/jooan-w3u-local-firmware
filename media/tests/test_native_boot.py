#!/usr/bin/env python3
"""Static invariants of the native boot script; no device access."""
import re
import subprocess
import sys
from pathlib import Path

script, main_c = Path(sys.argv[1]), Path(sys.argv[2])
text = script.read_text()
subprocess.run(["sh", "-n", str(script)], check=True)
# The readiness line it waits for is exactly what the daemon logs.
logged = re.search(r'jipc_log\("info", "ready: backend=%s, %u video streams"', main_c.read_text())
assert logged and "'ready: backend=imp, 4 video streams'" in text
# The trial marker is persisted before anything that can fail or hang.
assert text.index("stage start") < text.index('. "$root/deployment.env"')
assert text.index("stage launch") < text.index('kill -TERM "$f"')
# TERM hands the feeder over cleanly; only a failure leaves the watchdog armed.
assert 'kill -TERM "$f"' in text and 'kill -KILL "$f"' in text
assert "grep -qx 'motor_home=1'" in text and "motor homing complete" in text
assert text.rstrip().endswith("log 'native boot qualified; OEM application never started'")
assert text.index('rm -f "$root/native-trial"') > text.index("[ -n \"$homed\" ]")
# Lock contention is recorded, never a silent exit 0 that leaves the feeder
# timer to reset the camera back into the same native boot.
assert 'mkdir "$run/boot.lock" 2>/dev/null || exit 0' not in text
assert text.index("stage 'failed: boot lock held") < text.index("stage start")
print("native boot script: PASS")
