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
# Waiting for the network leaves no marker, so an outage cannot latch the fallback,
# and the daemon's own launch is recorded again before it can go wrong.
waiting = text.index('rm -f "$root/native-trial" "$root/native-trial.new"')
assert text.index("stage start") < waiting < text.index("until network_ready") < text.index("stage launch")
assert "stage network" not in text
# ...but it ends in a failure, and so the OEM's network, if nothing can come of it: no committed
# network, no runtime trying it, or a camera that has never qualified here (a first install).
worth = text[text.index("worth_waiting() {"):]
worth = worth[:worth.index("}")]
for needed in ('"$wifi_committed"', '"$wifi_attempts"', '"$root/ever-qualified"'):
    assert needed in worth, needed
assert 'if [ "$i" -ge 300 ] && ! worth_waiting; then' in text
# The feeder limit waits with it, and a failure clears that first.
assert text.index('> "$network_wait"') < text.index("until network_ready")
assert text.index('rm -f "$network_wait"') < text.index("fail 'no gateway")
# The wait has an end: the file goes after a bound that grows with each boot spent waiting, and
# sooner if the radio crashed, while the script goes on waiting. The count clears on qualifying.
give_up = text[text.index("give_up_waiting() {"):text.index("i=0\nuntil network_ready")]
assert 'rm -f "$network_wait"' in give_up and '> "$root/net-wait-boots"' in give_up
limit = text.index('[ "$i" -lt "$wait_limit" ] || give_up_waiting')
assert text.index("until network_ready") < limit < text.index("log 'network ready'") < text.index('rm -f "$root/net-wait-boots"')
assert "DUMPDONE" in text[limit:text.index("log 'network ready'")]
assert "$((900 << waits))" in text and '[ "$waits" -le 4 ] || waits=4' in text
# Only a qualified boot earns the wait; a lost network counts on three boots, not one.
assert text.rindex('"$root/ever-qualified"') > text.index("[ -n \"$homed\" ]")
assert '"$lost" -ge 3' in text and "three boots in a row" in text
# TERM hands the feeder over cleanly; only a failure leaves the watchdog armed.
assert 'kill -TERM "$f"' in text and 'kill -KILL "$f"' in text
assert "grep -qx 'motor_home=1'" in text and "motor homing complete" in text
assert text.rstrip().endswith("log 'native boot qualified; OEM application never started'")
assert text.rindex('rm -f "$root/native-trial"') > text.index("[ -n \"$homed\" ]")
# Lock contention is recorded, never a silent exit 0 that leaves the feeder
# timer to reset the camera back into the same native boot.
assert 'mkdir "$run/boot.lock" 2>/dev/null || exit 0' not in text
assert text.index("stage 'failed: boot lock held") < text.index("stage start")
print("native boot script: PASS")
