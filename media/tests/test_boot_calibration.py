#!/usr/bin/env python3
"""Boot qualification accepts complete fresh homing, never stale/failed reset."""
import subprocess
import sys

script = sys.argv[1]
marker = "JIPC_HANDOFF_BOOT=fixture-current"
episode = """[motor_step_reset:418]^^>>> start run ptz
[motor_step_reset:427]^^>>> end run left,start reset
[motor_step_reset:449]reset curMotorMsg.status:0
[motor_step_reset:459]^^>>> end run reset.use time:0 s,start run
[motor_step_reset:478]lastpot:[1379,397] reset_step:[2040,1200], max_step:[4080,1230] motor_cur[2040,1199]
[motor_step_reset:555]reset gst_curMotorMsg x=1379, y=397 status[0]
"""


def check(log, expected):
    result = subprocess.run(
        ["sh", script, "--check-calibration", marker],
        input=log.encode(), capture_output=True, timeout=3,
    )
    assert result.returncode == expected, (result.returncode, result.stderr, log)


check(marker + "\n" + episode, 0)
check(episode, 1)
check("JIPC_HANDOFF_BOOT=fixture-previous\n" + episode + marker + "\n", 1)
check(marker + "\n" + episode.splitlines()[-1] + "\n", 1)
check(marker + "\n" + episode.replace("status[0]", "status[1]"), 1)
for missing in (418, 427, 449, 459):
    incomplete = "\n".join(line for line in episode.splitlines()
                           if f"motor_step_reset:{missing}]" not in line)
    check(marker + "\n" + incomplete, 1)
for error in ("MOTOR_RESET is error", "wait go to last position, timeout 30s"):
    failed = episode.replace("[motor_step_reset:555]", error + "\n[motor_step_reset:555]")
    check(marker + "\n" + failed, 1)
    check(marker + "\n" + failed + episode, 0)
    check(marker + "\n" + episode + error + "\n", 1)
# Do not splice the first half of one reset with the second half of another.
half = "\n".join(episode.splitlines()[:3]) + "\n"
check(marker + "\n" + half + episode.splitlines()[0] + "\n"
      + "\n".join(episode.splitlines()[3:]), 1)
check(marker + "\n" + episode + marker + "\n", 1)
print("boot calibration: fresh, stale, incomplete, timeout and retry cases passed")
