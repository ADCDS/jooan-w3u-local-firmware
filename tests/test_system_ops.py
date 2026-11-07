#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Wave C helper operations, run for real against a temporary root:
several SSH keys, restart and its blockers, the restart schedule, the Wi-Fi
scan list, the Wi-Fi trial state, and band/gateway in network quality.
Never touches the host's network or reboots it (reboot is a fake)."""
import json
import hashlib
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
        # No key file yet (a fresh install, or after a reset): an empty list, not a failure.
        self.assertEqual(self.helper("ssh-list").returncode, 0)
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
        # While this boot is proving itself, a restart would latch the fallback.
        media = Path(self.temporary.name) / "media"
        media.mkdir(exist_ok=True)
        self.env["JL_MEDIA_ROOT"] = str(media)
        uptime = Path(self.temporary.name) / "uptime"
        self.env["JL_PROC_UPTIME"] = str(uptime)
        uptime.write_text("120.00 1.00\n")
        for marker in ("native-trial", "boot-trial"):
            (media / marker).write_text("stage=qualify\n")
            refused("starting")
            self.assertEqual(json.loads(self.helper("restart-check").stdout), {"ok": False, "reason": "starting"})
            (media / marker).unlink()
        # A marker a failed boot left for good must not refuse restarts for ever.
        (media / "boot-trial").write_text("x\n")
        uptime.write_text("4000.00 1.00\n")
        self.assertEqual(json.loads(self.helper("restart-check").stdout), {"ok": True})
        self.assertFalse(self.rebooted.exists())
        self.assertEqual(json.loads(self.helper("restart").stdout), {"ok": True})
        deadline = time.monotonic() + 6
        while not self.rebooted.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertTrue(self.rebooted.exists(), "the delayed reboot did not run")
        # It told the next boot, and kept the clock's time.
        self.assertEqual((media / "reboot-requested").read_text().strip(),
                         Path("/proc/sys/kernel/random/boot_id").read_text().strip())
        self.assertTrue((self.root / "state/last-time").exists())

    def test_reset_during_a_trial_is_allowed_and_latches_nothing(self) -> None:
        media = Path(self.temporary.name) / "media"
        media.mkdir(exist_ok=True)
        self.env["JL_MEDIA_ROOT"] = str(media)
        (media / "native-trial").write_text("stage=qualify\n")
        self.assertEqual(json.loads(self.helper("factory-reset").stdout), {"ok": True})
        deadline = time.monotonic() + 6
        while not self.rebooted.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        self.assertTrue(self.rebooted.exists())
        self.assertTrue((media / "reboot-requested").exists())

    def test_last_platform_install_can_be_asked_about(self) -> None:
        self.assertEqual(json.loads(self.helper("firmware-status").stdout)["state"], "idle")
        (self.run / "firmware.abc.status").write_text("state=failed\nreason=upgrade\nexit=1\n")
        self.assertEqual(json.loads(self.helper("firmware-status").stdout),
                         {"state": "failed", "reason": "upgrade", "restart": ""})
        (self.run / "firmware.abc.status").write_text("state=complete\nrestart=needed\n")
        self.assertEqual(json.loads(self.helper("firmware-status").stdout)["restart"], "needed")

    def test_update_is_refused_before_anything_is_deleted(self) -> None:
        """The whole update against the room it will have, with the real function."""
        script = (REPOSITORY / "packaging/payload/install-upgrade.sh").read_text()
        # Asked before the first step that can delete the runtime.
        first = script.index("publish_controller_core || die", script.index("transaction_fits ||"))
        self.assertLess(script.index("transaction_fits || die"), first)
        self.assertEqual(script.count("transaction_fits || die"), 1)
        start = script.index("tree_bytes() {")
        end = script.index("enter_runtime_recovery() {")
        functions = script[start:end]
        base = Path(self.temporary.name) / "fit"
        stage, tree, fake = base / "stage", base / "tree", base / "bin"
        for folder in (stage, tree / "slots/A", tree / "recovery", fake):
            folder.mkdir(parents=True)
        (fake / "df").write_text('#!/bin/sh\nprintf "x\\nd 384 288 $FAKE_FREE 75%% /opt\\n"\n')
        (fake / "df").chmod(0o755)
        def fits(free: int, new_kb: int, old_kb: int = 90) -> bool:
            (stage / "runtime.tar.gz").write_bytes(b"x" * (new_kb * 1024))
            (tree / "slots/A/runtime.tar.gz").write_bytes(b"x" * (old_kb * 1024))
            result = subprocess.run(["sh", "-c", functions + "\ntransaction_fits"],
                                    env=os.environ | {"PATH": f"{fake}:/bin:/usr/bin", "FAKE_FREE": str(free),
                                                      "self": str(stage), "root": str(tree)},
                                    capture_output=True, text=True)
            return result.returncode == 0
        self.assertTrue(fits(free=96, new_kb=90))           # camera1's figures: replacing like for like
        self.assertTrue(fits(free=64, new_kb=90))           # just enough, once the old one is gone
        self.assertFalse(fits(free=60, new_kb=90))          # a little short: nothing is touched
        self.assertFalse(fits(free=96, new_kb=130))         # a release that grew too much
        self.assertFalse(fits(free=500, new_kb=300, old_kb=90))  # room on flash, but over the logical budget

    def _install_runtime(self, selection: str, old_bytes: int, new_bytes: int, free_kb: list[int]):
        """The real install-runtime.sh and common.sh. df, sleep and sync are shell functions added
        to a copy of common.sh, as common.sh resets PATH; the script has no reboot to fake."""
        base = Path(self.temporary.name) / "install"
        shutil.rmtree(base, ignore_errors=True)
        control = base / "control"
        (control / "boot").mkdir(parents=True)
        (control / "admin").mkdir()
        readings = base / "readings"
        readings.write_text("".join(f"{value}\n" for value in free_kb))
        (control / "boot/common.sh").write_text(
            (REPOSITORY / "runtime/boot/common.sh").read_text() +
            f"""
df() {{
    value=$(head -n 1 "{readings}")
    [ "$(wc -l < "{readings}")" -le 1 ] || sed -i 1d "{readings}"
    printf 'Filesystem 1K-blocks Used Available Use%% Mounted\\nflash 384 0 %s 0%% /opt\\n' "$value"
}}
sleep() {{ :; }}
sync() {{ :; }}
""")
        persistent = base / "persistent"
        (persistent / "state").mkdir(parents=True)
        (persistent / "state/selection").write_text(selection + "\n")
        (persistent / "slots").mkdir()
        if old_bytes:
            (persistent / "slots/A").mkdir()
            (persistent / "slots/A/runtime.tar.gz").write_bytes(b"o" * old_bytes)
        stage = base / "stage"
        stage.mkdir()
        archive = os.urandom(new_bytes)
        (stage / "runtime.tar.gz").write_bytes(archive)
        (stage / "runtime.md5").write_text(hashlib.md5(archive).hexdigest())
        run = base / "run"
        run.mkdir()
        result = subprocess.run(["sh", str(REPOSITORY / "runtime/admin/install-runtime.sh"), str(stage)],
                                env=os.environ | {"JL_ROOT": str(persistent), "JL_RUN": str(run), "JL_CONTROL": str(control)},
                                text=True, capture_output=True, timeout=60)
        return result, persistent

    def test_runtime_install_waits_for_space_just_freed(self) -> None:
        """What happened on camera1: an earlier step had just deleted the old runtime, the flash
        had not shown the space yet, and a single reading refused an update that fit."""
        result, persistent = self._install_runtime("- - 0", 0, 50 * 1024, [60, 60, 90, 144])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        selection = (persistent / "state/selection").read_text().split()
        self.assertEqual(selection[0], "-")
        self.assertTrue((persistent / "slots" / selection[1] / "runtime.tar.gz").exists())

    def test_runtime_install_refuses_with_the_old_one_untouched(self) -> None:
        result, persistent = self._install_runtime("A - 0", 20 * 1024, 90 * 1024, [60])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nothing was changed", result.stdout + result.stderr)
        self.assertEqual((persistent / "state/selection").read_text().strip(), "A - 0")
        self.assertEqual((persistent / "slots/A/runtime.tar.gz").stat().st_size, 20 * 1024)

    def test_clock_confirmed(self) -> None:
        self.assertFalse((self.run / "clock-set").exists())
        self.assertEqual(self.helper("clock-known").returncode, 0)
        self.assertTrue((self.run / "clock-set").exists())

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

    def address_fakes(self) -> dict:
        """A PATH holding only fakes for what the address path can touch, each logged.
        The hook keeps the caller's PATH, so nothing real may be reachable first."""
        base = Path(self.temporary.name)
        log = base / "net.log"
        for tool in ("ifconfig", "route", "killall", "udhcpc", "arping"):
            executable(self.bin / tool, "#!/bin/sh\n"
                       f"printf '%s %s\\n' {tool} \"$*\" >> \"$NET_LOG\"\n"
                       + ("[ \"$FAKE_ARPING\" != \"$1\" ] || exit 1\n" if tool == "arping" else "")
                       + ("printf '          inet addr:192.168.20.38  Bcast:192.168.20.255  Mask:255.255.255.0\\n'\n"
                          if tool == "ifconfig" else ""))
        executable(base / "json_tool", r"""#!/bin/sh
key=${4#/}
sed -n 's/.*"'"$key"'":"\([^"]*\)".*/1 x \1/p' "$5"
""")
        path = f"{self.bin}:/bin:/usr/bin"
        for tool in ("ifconfig", "route", "killall", "udhcpc", "arping", "wpa_cli", "reboot"):
            self.assertEqual(shutil.which(tool, path=path), str(self.bin / tool), "not faked: refusing to run")
        return self.env | {"PATH": path, "JOOAN_PATH": path, "NET_LOG": str(log), "FAKE_ARPING": "",
                           "JOAN_JSON_TOOL": str(base / "json_tool")}

    def test_static_address_hook(self) -> None:
        env, base = self.address_fakes(), Path(self.temporary.name)
        hook = REPOSITORY / "runtime/slot/hooks/wifi-apply.sh"
        log = Path(env["NET_LOG"])
        def run(fields: str):
            log.write_text("")
            (base / "c.json").write_text('{"ssid":"Home","password":"secret-pass"' + fields + "}\n")
            result = subprocess.run([str(hook), str(base / "c.json")], env=env | {"JOAN_WIFI_KEEP_LINK": "1"},
                                    text=True, capture_output=True)
            return result, log.read_text()
        result, seen = run(',"address":"192.168.20.50","prefix":"24","gateway":"192.168.20.1"')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ifconfig wlan0 192.168.20.50 netmask 255.255.255.0 up", seen)
        self.assertIn("route add default gw 192.168.20.1 dev wlan0", seen)
        self.assertIn("killall udhcpc", seen)
        self.assertNotIn("udhcpc -a", seen)
        result, seen = run(',"address":"10.1.2.3","prefix":"20","gateway":"10.1.0.1"')
        self.assertIn("netmask 255.255.240.0", seen, result.stderr)
        # Automatic: DHCP again, and no address of its own.
        result, seen = run("")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("udhcpc -a", seen)
        self.assertNotIn("route add", seen)
        # All three or none, and nothing but numbers.
        self.assertEqual(run(',"address":"192.168.20.50"')[0].returncode, 1)
        self.assertEqual(run(',"address":"1;reboot","prefix":"24","gateway":"192.168.20.1"')[0].returncode, 1)

    def test_address_stage(self) -> None:
        env, base = self.address_fakes(), Path(self.temporary.name)
        self.env = env
        admin = base / "control/admin"
        admin.mkdir(parents=True)
        env["JL_CONTROL"] = str(base / "control")
        executable(admin / "wifi-transaction.sh", '#!/bin/sh\nprintf "%s %s\\n" "$JOAN_WIFI_KEEP_LINK" "$1" > "$NET_LOG.tx"\n'
                   'cp "$2" "$NET_LOG.candidate"\n')
        def stage(line: str):
            (base / "line").write_text(line + "\n")
            result = subprocess.run([str(HELPER), "ip-stage", str(base / "line"), "x"], env=env, text=True, capture_output=True)
            return result
        wanted = "static 192.168.20.50 24 192.168.20.1"
        self.assertEqual(json.loads(stage(wanted).stdout), {"ok": False, "reason": "no-network"})
        (self.root / "config/wifi.json").write_text('{"ssid":"Home","password":"secret-pass"}\n')
        env["FAKE_ARPING"] = "-D"
        self.assertEqual(json.loads(stage(wanted).stdout), {"ok": False, "reason": "address-in-use"})
        env["FAKE_ARPING"] = "-f"
        self.assertEqual(json.loads(stage(wanted).stdout), {"ok": False, "reason": "gateway-silent"})
        self.assertFalse(Path(env["NET_LOG"] + ".candidate").exists(), "a refusal must not start a trial")
        env["FAKE_ARPING"] = ""
        (self.root / "state/wifi-trial").mkdir()
        self.assertEqual(json.loads(stage(wanted).stdout), {"ok": False, "reason": "wifi-trial"})
        (self.root / "state/wifi-trial").rmdir()
        result = stage(wanted)
        self.assertEqual(json.loads(result.stdout), {"ok": True}, result.stderr)
        self.assertEqual(json.loads(Path(env["NET_LOG"] + ".candidate").read_text()),
                         {"ssid": "Home", "password": "secret-pass", "address": "192.168.20.50",
                          "prefix": "24", "gateway": "192.168.20.1"})
        self.assertTrue(Path(env["NET_LOG"] + ".tx").read_text().startswith("1 "), "the link must stay up")
        self.assertFalse((self.run / "ip-candidate.json").exists(), "the password must not stay behind")
        # The address it already has is not a conflict, and going back to automatic needs no checks.
        log = Path(env["NET_LOG"]); log.write_text("")
        env["FAKE_ARPING"] = "-D"
        self.assertEqual(json.loads(stage("static 192.168.20.38 24 192.168.20.1").stdout), {"ok": True})
        log.write_text("")
        self.assertEqual(json.loads(stage("dhcp").stdout), {"ok": True})
        self.assertNotIn("arping", log.read_text())
        self.assertEqual(json.loads(Path(env["NET_LOG"] + ".candidate").read_text()), {"ssid": "Home", "password": "secret-pass"})

    def test_address_readout(self) -> None:
        self.env = self.address_fakes()
        (self.root / "config/wifi.json").write_text('{"ssid":"Home","password":"secret-pass"}\n')
        (self.run / "default-gateway").write_text("192.168.20.1 wlan0\n")
        result = self.helper("ip-get")
        self.assertEqual(json.loads(result.stdout), {"mode": "dhcp", "address": "192.168.20.38", "prefix": 24,
                                                     "gateway": "192.168.20.1"}, result.stderr)
        (self.root / "config/wifi.json").write_text('{"ssid":"Home","password":"x","address":"192.168.20.38","prefix":"24","gateway":"192.168.20.1"}\n')
        self.assertEqual(json.loads(self.helper("ip-get").stdout)["mode"], "static")
        # A trial says which address it proposes; the password stays out of it.
        trial = self.root / "state/wifi-trial"
        trial.mkdir()
        (trial / "deadline").write_text("999999999\n")
        (trial / "candidate.json").write_text('{"ssid":"Home","password":"secret-pass","address":"192.168.20.50","prefix":"24","gateway":"192.168.20.1"}')
        result = self.helper("wifi-state")
        self.assertEqual(json.loads(result.stdout)["static"], "192.168.20.50", result.stderr)
        self.assertNotIn("secret", result.stdout)

    def test_bounded_hook_leaves_nothing_behind(self) -> None:
        # capture_output reads to the end of the pipe, so a sleep left holding it shows as time.
        started = time.monotonic()
        result = self.common('jl_bounded_hook 30 /bin/true; echo rc=$?')
        self.assertIn("rc=0", result.stdout, result.stderr)
        self.assertLess(time.monotonic() - started, 5, "the timer's sleep outlived the hook")
        result = self.common('jl_bounded_hook 30 /bin/false; echo rc=$?')
        self.assertIn("rc=1", result.stdout, result.stderr)
        # And the limit still holds.
        started = time.monotonic()
        result = self.common('jl_bounded_hook 1 /bin/sleep 30; echo rc=$?')
        self.assertNotIn("rc=0", result.stdout, result.stderr)
        self.assertLess(time.monotonic() - started, 6, "the limit did not stop the hook")

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
        (self.run / "clock-set").write_text("")
        due = "jl_netdog_ok=1; jl_scheduled_restart"
        self.assertEqual(self.common(due).returncode, 0)
        self.assertFalse(self.rebooted.exists())
        # Due, in the camera's time zone, but in the first hour after a boot.
        self.schedule_now()
        self.common(due, uptime=1200)
        self.assertFalse(self.rebooted.exists())
        # Due, but the gateway is not answering: not into an outage.
        self.common("jl_scheduled_restart")
        self.assertFalse(self.rebooted.exists())
        self.common("jl_netdog_ok=1; jl_netdog_down=7000; jl_scheduled_restart")
        self.assertFalse(self.rebooted.exists())
        # Due, but an update is running, or a Wi-Fi or boot trial is pending.
        (self.run / "state.lock").mkdir()
        self.common(due)
        self.assertFalse(self.rebooted.exists())
        (self.run / "state.lock").rmdir()
        (self.root / "state/wifi-trial").mkdir()
        self.common(due)
        self.assertFalse(self.rebooted.exists())
        (self.root / "state/wifi-trial").rmdir()
        (self.root / "state/selection").write_text("A B 1\n")
        self.common(due)
        self.assertFalse(self.rebooted.exists())
        (self.root / "state/selection").write_text("A - 0\n")
        result = self.common(due)
        self.assertTrue(self.rebooted.exists(), result.stderr)
        self.rebooted.unlink()
        # With no clock anything has confirmed: once a day counted from the start.
        (self.run / "clock-set").unlink()
        self.common(due, uptime=7200)
        self.assertFalse(self.rebooted.exists())
        self.common(due, uptime=86400)
        self.assertTrue(self.rebooted.exists())

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
