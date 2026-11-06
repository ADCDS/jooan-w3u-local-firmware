#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Wave C helper operations, run for real against a temporary root:
several SSH keys, restart and its blockers, the restart schedule, the Wi-Fi
scan list, the Wi-Fi trial state, and band/gateway in network quality.
Never touches the host's network or reboots it (reboot is a fake)."""
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
HELPER = REPOSITORY / "runtime/slot/hooks/integration-helper.sh"
KEY = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI{0:032d} user{0}@host"


def executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(0o755)


class SystemOpsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        base = Path(self.temporary.name)
        self.root, self.run, self.bin = base / "root", base / "run", base / "bin"
        for directory in (self.root / "state", self.root / "config", self.run, self.bin):
            directory.mkdir(parents=True)
        self.rebooted = base / "rebooted"
        executable(self.bin / "reboot", f"#!/bin/sh\ntouch '{self.rebooted}'\n")
        executable(self.bin / "wpa_cli", "#!/bin/sh\n"
                   "[ \"$1\" = -iwlan0 ] || exit 2\n"
                   "case \"$2\" in\n"
                   "  status) printf '%s\\n' \"$FAKE_WPA_STATUS\" ;;\n"
                   "  scan) : > \"$FAKE_SCANNED\"; echo OK ;;\n"
                   # The table only changes once the scan has ended: until then, the old one.
                   "  scan_results) if [ -e \"$FAKE_SCANNED\" ]; then printf '%b' \"$FAKE_SCAN\"; else printf '%b' \"$FAKE_OLD_SCAN\"; fi ;;\n"
                   "  *) exit 2 ;;\n"
                   "esac\n")
        # The helper honours JOOAN_PATH; make sure its `reboot` is the fake before any test runs.
        self.assertEqual(shutil.which("reboot", path=f"{self.bin}:/bin:/usr/bin"), str(self.bin / "reboot"))
        self.env = os.environ | {"JOOAN_PATH": f"{self.bin}:/bin:/usr/bin", "JL_ROOT": str(self.root),
                                 "JL_RUN": str(self.run), "FAKE_WPA_STATUS": "wpa_state=COMPLETED\nssid=Home",
                                 "FAKE_SCAN": "", "FAKE_OLD_SCAN": "", "FAKE_SCANNED": str(base / "scanned"),
                                 "JL_NET_SYSFS": str(base / "sys"),
                                 "JL_NET_WIRELESS": str(base / "wireless")}

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def helper(self, op: str, path: str = "-", ident: str = "-"):
        return subprocess.run([str(HELPER), op, path, ident], env=self.env, text=True, capture_output=True)

    def keys(self) -> list[str]:
        path = self.root / "config/ssh/authorized_keys"
        return [line for line in path.read_text().splitlines() if line] if path.exists() else []

    def add_key(self, number: int):
        staged = Path(self.temporary.name) / f"key{number}"
        staged.write_text(KEY.format(number) + "\n")
        return self.helper("ssh-add", str(staged))

    def test_several_ssh_keys(self) -> None:
        self.assertEqual(self.add_key(1).returncode, 0)
        self.assertEqual(self.add_key(1).returncode, 0)            # already there: unchanged
        self.assertEqual(self.keys(), [KEY.format(1)])
        for number in range(2, 9):
            self.assertEqual(self.add_key(number).returncode, 0)
        self.assertEqual(len(self.keys()), 8)
        self.assertEqual(self.add_key(9).returncode, 1)            # at most 8
        self.assertEqual(self.helper("ssh-delete", ident="2").returncode, 0)
        self.assertEqual(self.keys()[:2], [KEY.format(1), KEY.format(3)])
        self.assertEqual(self.helper("ssh-delete", ident="x").returncode, 2)
        self.assertEqual(self.helper("ssh-delete", ident="all").returncode, 0)
        self.assertEqual(self.keys(), [])
        # Removing every key leaves an empty file, and adding must still work.
        self.assertEqual(self.add_key(1).returncode, 0)
        self.assertEqual(self.keys(), [KEY.format(1)])
        bad = Path(self.temporary.name) / "bad"
        bad.write_text("ssh-rsa AAAA user@host\n")
        self.assertEqual(self.helper("ssh-add", str(bad)).returncode, 1)
        self.assertEqual(oct((self.root / "config/ssh/authorized_keys").stat().st_mode & 0o777), "0o600")

    def test_restart_schedule(self) -> None:
        self.assertEqual(json.loads(self.helper("restart-schedule").stdout), {"enabled": False, "time": None})
        result = self.helper("restart-schedule-set", ident="04:30")
        self.assertEqual(json.loads(result.stdout), {"enabled": True, "time": "04:30"}, result.stderr)
        self.assertEqual(self.helper("restart-schedule-set", ident="24:00").returncode, 2)
        self.assertEqual(self.helper("restart-schedule-set", ident="4:30").returncode, 2)
        self.assertEqual(json.loads(self.helper("restart-schedule-set", ident="off").stdout)["enabled"], False)

    def test_restart_refuses_while_busy(self) -> None:
        def refused(reason: str) -> None:
            result = self.helper("restart")
            self.assertEqual(json.loads(result.stdout), {"ok": False, "reason": reason}, result.stderr)
        (self.run / "state.lock").mkdir()
        refused("update")
        (self.run / "state.lock").rmdir()
        (self.root / "state/wifi-trial").mkdir()
        refused("wifi-trial")
        (self.root / "state/wifi-trial").rmdir()
        (self.root / "state/selection").write_text("A B 1\n")
        refused("boot-trial")
        (self.root / "state/selection").write_text("A - 0\n")
        self.assertFalse(self.rebooted.exists())
        self.assertEqual(json.loads(self.helper("restart").stdout), {"ok": True})
        deadline = time.monotonic() + 6
        while not self.rebooted.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertTrue(self.rebooted.exists(), "the delayed reboot did not run")

    def test_wifi_scan(self) -> None:
        self.env["FAKE_OLD_SCAN"] = ("bssid / frequency / signal level / flags / ssid\\n"
                                     "aa:aa:aa:aa:aa:02\\t5180\\t-61\\t[WPA2-PSK-CCMP][ESS]\\tHome\\n")
        self.env["FAKE_SCAN"] = ("bssid / frequency / signal level / flags / ssid\\n"
                                 "aa:aa:aa:aa:aa:01\\t2437\\t-80\\t[WPA2-PSK-CCMP][ESS]\\tHome\\n"
                                 "aa:aa:aa:aa:aa:02\\t5180\\t-60\\t[WPA2-PSK-CCMP][ESS]\\tHome\\n"
                                 "aa:aa:aa:aa:aa:03\\t2412\\t-50\\t[ESS]\\tCafe \"Free\"\\n"
                                 "aa:aa:aa:aa:aa:04\\t2462\\t-90\\t[ESS]\\t\\n")
        result = self.helper("wifi-scan")
        self.assertEqual(result.returncode, 0, result.stderr)
        networks = json.loads(result.stdout)
        # Strongest first, no hidden, and the fresh table rather than the old one.
        self.assertEqual([n["ssid"] for n in networks], ["Cafe ?Free?", "Home"])
        home = networks[1]
        self.assertEqual((home["signal_dbm"], home["frequency_mhz"], home["secure"], home["in_use"]),
                         (-60, 5180, True, True))
        self.assertEqual((networks[0]["secure"], networks[0]["in_use"]), (False, False))

    def test_wifi_trial_state(self) -> None:
        self.assertEqual(json.loads(self.helper("wifi-state").stdout), {"pending": False, "last": None})
        trial = self.root / "state/wifi-trial"
        trial.mkdir()
        uptime = int(float(Path("/proc/uptime").read_text().split()[0]))
        (trial / "deadline").write_text(f"{uptime + 60}\n")
        (trial / "candidate.json").write_text('{"ssid":"New Net","psk":"secret-never-shown"}')
        self.env["FAKE_WPA_STATUS"] = "wpa_state=COMPLETED\nssid=New Net\nip_address=192.168.20.40"
        result = self.helper("wifi-state")
        state = json.loads(result.stdout)
        self.assertTrue(state["pending"] and state["joined"], result.stdout)
        self.assertTrue(55 <= state["seconds_left"] <= 60, state)
        self.assertEqual((state["ssid"], state["address"]), ("New Net", "192.168.20.40"))
        self.assertNotIn("secret", result.stdout)
        trial.joinpath("deadline").unlink(); trial.joinpath("candidate.json").unlink(); trial.rmdir()
        (self.root / "state/wifi-committed").write_text("x\n")
        self.assertEqual(json.loads(self.helper("wifi-state").stdout)["last"], "committed")

    def test_network_quality_band_and_gateway(self) -> None:
        sysfs = Path(self.temporary.name) / "sys"
        for interface in ("wlan0", "eth0"):
            (sysfs / interface / "statistics").mkdir(parents=True)
        self.env["FAKE_WPA_STATUS"] = "wpa_state=COMPLETED\nssid=Home\nfreq=5180"
        (self.run / "default-gateway").write_text("192.168.20.1 wlan0\n")
        result = self.helper("network-quality")
        wifi = json.loads(result.stdout)["wifi"]
        self.assertEqual((wifi["frequency_mhz"], wifi["band"], wifi["gateway"]), (5180, "5 GHz", "192.168.20.1"),
                         result.stderr)
        self.env["FAKE_WPA_STATUS"] = "wpa_state=COMPLETED\nssid=Home\nfreq=2437"
        self.assertEqual(json.loads(self.helper("network-quality").stdout)["wifi"]["band"], "2.4 GHz")

    def test_reset_to_defaults(self) -> None:
        """The helper only marks and restarts; the boot erases exactly the list."""
        media = Path(self.temporary.name) / "media"
        erased = [self.root / "config/auth.db", self.root / "config/sessions.db", self.root / "config/restart-schedule",
                  self.root / "config/ssh/passwd", self.root / "config/ssh/authorized_keys",
                  media / "state/controls.json", media / "state/presets.v2.json"]
        kept = [self.root / "config/wifi.json", self.root / "config/hostname", self.root / "config/routes.list",
                self.root / "config/ntp-server", self.root / "config/tls-key.pem",
                media / "state/position", media / "native", media / "enabled"]
        (self.root / "config/ssh").mkdir()
        (media / "state").mkdir(parents=True)
        for path in erased + kept:
            path.write_text("old\n")
        (media / "camera.ini").write_text("bind=192.0.2.2\nusername=admin\npassword=changed\naudio=1\n")
        mark = self.root / "state/reset-pending"
        (self.root / "state/wifi-trial").mkdir()
        result = self.helper("factory-reset")
        self.assertEqual(json.loads(result.stdout), {"ok": False, "reason": "wifi-trial"}, result.stderr)
        self.assertFalse(mark.exists())
        (self.root / "state/wifi-trial").rmdir()
        self.assertEqual(json.loads(self.helper("factory-reset").stdout), {"ok": True})
        self.assertTrue(mark.exists())
        self.assertTrue(all(path.exists() for path in erased), "the helper must not erase anything itself")
        deadline = time.monotonic() + 6
        while not self.rebooted.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertTrue(self.rebooted.exists(), "the delayed reboot did not run")
        result = self.common("jl_apply_reset", JL_MEDIA_ROOT=str(media))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([str(path) for path in erased if path.exists()], [])
        self.assertEqual([str(path) for path in kept if not path.exists()], [])
        self.assertEqual((media / "camera.ini").read_text(),
                         "bind=192.0.2.2\nusername=admin\npassword=admin\naudio=1\n")
        self.assertEqual(oct((media / "camera.ini").stat().st_mode & 0o777), "0o600")
        self.assertFalse(mark.exists())
        # With no mark it does nothing at all.
        (media / "state/controls.json").write_text("new\n")
        self.common("jl_apply_reset", JL_MEDIA_ROOT=str(media))
        self.assertTrue((media / "state/controls.json").exists())

    def common(self, script: str, uptime: int = 7200, **env):
        """Run a snippet with the real runtime/boot/common.sh sourced.

        common.sh RESETS PATH, so a fake in a PATH directory is ignored and a
        bare `reboot` is the machine's real one: this very test once rebooted
        the workstation that way. Fakes are shell functions defined AFTER the
        source, which always win over PATH, and the snippet refuses to run
        (exit 99) unless `reboot` really is one of them."""
        (self.run / "uptime").write_text(f"{uptime}.00 1.00\n")
        extra = {"JL_PROC_UPTIME": str(self.run / "uptime"), "JL_OEM_CONFIG": str(self.root / "oem.json"),
                 "FAKE_REBOOTED": str(self.rebooted)} | env
        fakes = ('reboot() { : > "$FAKE_REBOOTED"; }; sync() { :; }; '
                 'dmesg() { printf "boot line\\nCPU0 RESET ERROR PC:80194240\\nskw DUMPDONE\\n"; }; '
                 'wpa_cli() { echo wpa_state=COMPLETED; }; '
                 'case "$(command -V reboot 2>&1)" in *function*) ;; *) exit 99 ;; esac; ')
        result = subprocess.run(["sh", "-c", f'. "{REPOSITORY}/runtime/boot/common.sh"; {fakes}{script}'],
                                env=self.env | extra, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 99, "reboot is not faked: refusing to run")
        return result

    def schedule_now(self, offset_minutes: int = -180) -> None:
        minute = (int(time.time()) // 60 + offset_minutes) % 1440
        (self.root / "config/restart-schedule").write_text(f"{minute // 60:02d}:{minute % 60:02d}\n")
        (self.root / "oem.json").write_text('{"IpcParam":{"TimeZomeCfg":{\n  "GmtTz":\t"GMT-03:00"\n}}}\n')

    def test_scheduled_restart(self) -> None:
        # Not due: another minute of the day.
        minute = (int(time.time()) // 60 - 180 + 300) % 1440
        (self.root / "config/restart-schedule").write_text(f"{minute // 60:02d}:{minute % 60:02d}\n")
        (self.root / "oem.json").write_text('{"GmtTz": "GMT-03:00"}\n')
        self.assertEqual(self.common("jl_scheduled_restart").returncode, 0)
        self.assertFalse(self.rebooted.exists())
        # Due, in the camera's time zone, but in the first hour after a boot.
        self.schedule_now()
        self.common("jl_scheduled_restart", uptime=1200)
        self.assertFalse(self.rebooted.exists())
        # Due, but an update is running, or a Wi-Fi or boot trial is pending.
        (self.run / "state.lock").mkdir()
        self.common("jl_scheduled_restart")
        self.assertFalse(self.rebooted.exists())
        (self.run / "state.lock").rmdir()
        (self.root / "state/wifi-trial").mkdir()
        self.common("jl_scheduled_restart")
        self.assertFalse(self.rebooted.exists())
        (self.root / "state/wifi-trial").rmdir()
        (self.root / "state/selection").write_text("A B 1\n")
        self.common("jl_scheduled_restart")
        self.assertFalse(self.rebooted.exists())
        (self.root / "state/selection").write_text("A - 0\n")
        result = self.common("jl_scheduled_restart")
        self.assertTrue(self.rebooted.exists(), result.stderr)

    def test_flight_recorder(self) -> None:
        history = self.root / "state/reset-history"
        for _ in range(23):
            self.assertEqual(self.common("jl_note_boot").returncode, 0)
        lines = history.read_text().splitlines()
        self.assertEqual(len(lines), 20, "the boot trail stays bounded")
        self.assertRegex(lines[-1], r"^\d+ pc=80194240 netdog=0$")
        self.assertEqual(oct(history.stat().st_mode & 0o777), "0o600")
        log = self.root / "state/net-loss.log"
        self.assertEqual(self.common("jl_note_net_loss").returncode, 0)
        record = log.read_text()
        self.assertIn("DUMPDONE", record)
        self.assertIn("uptime 7200", record)
        self.assertEqual(oct(log.stat().st_mode & 0o777), "0o600")
        # Written again only after the link returned, and at most three times a boot.
        result = self.common('jl_note_net_loss; rm -f "$JL_STATE/net-loss.log"; jl_note_net_loss; '
                             '[ ! -e "$JL_STATE/net-loss.log" ] && echo held')
        self.assertIn("held", result.stdout, result.stderr)
        result = self.common('for i in 1 2 3 4 5; do jl_net_loss_armed=1; rm -f "$JL_STATE/net-loss.log"; jl_note_net_loss; '
                             '[ -e "$JL_STATE/net-loss.log" ] && echo wrote; done')
        self.assertEqual(result.stdout.split().count("wrote"), 3, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
