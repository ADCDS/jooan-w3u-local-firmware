from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]


def executable(path: Path, contents: str) -> None:
    path.write_text(contents, encoding="utf-8")
    path.chmod(0o755)


class RuntimeHookTests(unittest.TestCase):
    def _network_quality(self, root: Path, status: str, radio: str) -> dict:
        sysfs = root / "sys" / "class" / "net"
        for interface in ("wlan0", "eth0"):
            statistics = sysfs / interface / "statistics"
            statistics.mkdir(parents=True, exist_ok=True)
            for field, value in (("rx_bytes", "123456"), ("tx_bytes", "12"),
                                 ("rx_packets", "45"), ("tx_packets", "67")):
                counter = statistics / field
                if not counter.exists():
                    counter.write_text(value + "\n", encoding="utf-8")
        for field, value in (("carrier", "1"), ("speed", "100")):
            path = sysfs / "eth0" / field
            if not path.exists():
                path.write_text(value + "\n", encoding="utf-8")
        fake_bin = root / "bin"
        fake_bin.mkdir(exist_ok=True)
        executable(fake_bin / "wpa_cli", "#!/bin/sh\n"
                   "[ \"$1\" = -iwlan0 ] && [ \"$2\" = status ] || exit 2\n"
                   "printf '%s\\n' \"$FAKE_WPA_STATUS\"\n")
        executable(fake_bin / "iwconfig", "#!/bin/sh\n"
                   "[ \"$1\" = wlan0 ] || exit 2\n"
                   "printf '%s\\n' \"$FAKE_RADIO\"\n")
        env = os.environ | {"JOOAN_PATH": f"{fake_bin}:/bin:/usr/bin",
                            "JL_NET_SYSFS": str(sysfs),
                            "JL_NET_WIRELESS": str(root / "wireless"),
                            "FAKE_WPA_STATUS": status, "FAKE_RADIO": radio}
        result = subprocess.run(
            [str(REPOSITORY / "runtime/slot/hooks/integration-helper.sh"), "network-quality"],
            env=env, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def test_network_quality_reports_sanitized_association_and_counters(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            data = self._network_quality(
                Path(temporary),
                'wpa_state=COMPLETED\nssid=Guest "Network" ; $(touch /tmp/nope)\n'
                'bssid=aa:bb:cc:dd:ee:ff\npsk=never-expose-this',
                'wlan0 IEEE 802.11 Bit Rate=72.2 Mb/s  Access Point: AA:BB:CC\n'
                ' Link Quality=55/70 Signal level=-48 dBm',
            )
            self.assertEqual(data["wifi"]["associated"], True)
            self.assertEqual(data["wifi"]["ssid"], "Guest ?Network? ? ??touch ?tmp?nope?")
            self.assertEqual(data["wifi"]["signal_dbm"], -48)
            self.assertEqual(data["wifi"]["rate_mbps"], 72.2)
            self.assertEqual(data["wifi"]["rx_bytes"], "123456")
            self.assertEqual(data["ethernet"]["link"], True)
            self.assertEqual(data["ethernet"]["speed_mbps"], 100)
            self.assertEqual(data["ethernet"]["tx_packets"], "67")
            self.assertNotIn("never-expose-this", json.dumps(data))
            self.assertNotIn("aa:bb:cc:dd:ee:ff", json.dumps(data))

    def test_network_quality_unknown_and_unassociated_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = self._network_quality(root, "wpa_state=SCANNING\nssid=old", "")
            self.assertFalse(data["wifi"]["associated"])
            self.assertIsNone(data["wifi"]["ssid"])
            self.assertIsNone(data["wifi"]["signal_dbm"])
            self.assertIsNone(data["wifi"]["rate_mbps"])
            (root / "sys/class/net/eth0/carrier").write_text("0\n", encoding="utf-8")
            (root / "sys/class/net/wlan0/statistics/rx_bytes").write_text(
                "oops\n", encoding="utf-8"
            )
            data = self._network_quality(root, "", "")
            self.assertIsNone(data["wifi"]["associated"])
            self.assertIsNone(data["wifi"]["rx_bytes"])
            self.assertFalse(data["ethernet"]["link"])
            self.assertIsNone(data["ethernet"]["speed_mbps"])

    def test_speaker_is_muted_before_oem_media_can_start(self) -> None:
        hook = (REPOSITORY / "runtime/boot/local.rc").read_text(encoding="utf-8")
        mute = hook.index("JL_SPEAKER_GPIO=63")
        media_wrapper = hook.index("jooanipc()")
        self.assertLess(mute, media_wrapper)
        self.assertIn('echo 1 > "$JL_SPEAKER_PATH/value"', hook[mute:media_wrapper])
        self.assertIn('[ "$jl_speaker_tick" -lt 3000 ]; do', hook)
        self.assertIn("sleep 0.02", hook)
        self.assertIn('JL_SPEAKER_RUN=/run/jooan-local', hook)
        self.assertIn('JL_SPEAKER_HOOK_RELEASED=$JL_SPEAKER_RUN/speaker-hook-released', hook)
        self.assertIn('[ ! -f "$JL_SPEAKER_GUARD_READY" ] || break', hook)
        self.assertIn('printf \'%s\\n\' released > "$JL_SPEAKER_HOOK_RELEASED.new"', hook)
        self.assertNotIn('echo 0 > "$JL_SPEAKER_PATH/value"', hook[mute:media_wrapper])

    def test_onvif_50ms_is_not_encoded_as_500ms(self) -> None:
        hook = REPOSITORY / "runtime/slot/hooks/onvif-ptz.sh"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            capture = root / "request"
            executable(
                fake_bin / "nc",
                "#!/bin/sh\n"
                "[ \"$1\" = -w2 ] || exit 3\n"
                "cat > \"$CAPTURE\"\n"
                "printf 'HTTP/1.0 200 OK\\r\\n\\r\\n'\n"
                "printf '<tptz:ContinuousMoveResponse/>'\n",
            )
            environment = os.environ | {
                "PATH": f"{fake_bin}:/bin:/usr/bin",
                "CAPTURE": str(capture),
                "JL_RUN": str(root),
            }
            result = subprocess.run(
                [str(hook), "move", "up", "3", "50"],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            request = capture.read_text(encoding="utf-8")
            self.assertIn("<tptz:Timeout>PT0.050S</tptz:Timeout>", request)
            self.assertNotIn("PT0.50S", request)

    def _wifi_apply_environment(self, root: Path) -> tuple[dict[str, str], Path]:
        """Build a fake PATH for wifi-apply.sh: wpa_cli hands out incrementing
        add_network ids (like a freshly booted wpa_supplicant), and every call
        is logged so tests can assert on the exact argv wpa_cli received."""
        fake_bin = root / "bin"
        fake_bin.mkdir()
        log = root / "wpa.log"
        json_tool = root / "json_debug"
        executable(
            json_tool,
            "#!/bin/sh\n"
            "case \"$4\" in\n"
            "  /ssid) printf '1 /ssid Family Room WiFi\\n' ;;\n"
            "  /password) printf '1 /password safe passphrase!\\n' ;;\n"
            "  *) exit 1 ;;\n"
            "esac\n",
        )
        executable(
            fake_bin / "wpa_cli",
            "#!/bin/sh\n"
            "printf '%s' \"$#\" >> \"$WPA_LOG\"\n"
            "for value in \"$@\"; do printf '|%s' \"$value\" >> \"$WPA_LOG\"; done\n"
            "printf '\\n' >> \"$WPA_LOG\"\n"
            "case \"$*\" in\n"
            "  *ping*)\n"
            "    t=0; [ -f \"$WPA_PINGS\" ] && t=$(cat \"$WPA_PINGS\")\n"
            "    echo $((t + 1)) > \"$WPA_PINGS\"\n"
            "    # Stay unreachable for PING_FAILS calls, like a boot-time\n"
            "    # wpa_supplicant whose control socket does not exist yet.\n"
            "    if [ \"$t\" -lt \"${PING_FAILS:-0}\" ]; then\n"
            "      echo 'Failed to connect to non-global ctrl_ifname: wlan0' >&2\n"
            "      exit 255\n"
            "    fi\n"
            "    printf 'PONG\\n'\n"
            "    ;;\n"
            "  *add_network*)\n"
            "    n=0; [ -f \"$WPA_COUNTER\" ] && n=$(cat \"$WPA_COUNTER\")\n"
            "    printf '%s\\n' \"$n\"; echo $((n + 1)) > \"$WPA_COUNTER\"\n"
            "    ;;\n"
            "  *status*) printf 'wpa_state=COMPLETED\\n' ;;\n"
            "  *) printf 'OK\\n' ;;\n"
            "esac\n",
        )
        executable(fake_bin / "killall", "#!/bin/sh\nexit 0\n")
        executable(fake_bin / "udhcpc", "#!/bin/sh\nexit 0\n")
        executable(
            fake_bin / "ifconfig",
            "#!/bin/sh\nprintf 'inet addr:192.168.1.2\\n'\n",
        )
        environment = os.environ | {
            "PATH": f"{fake_bin}:/bin:/usr/bin",
            "JOAN_JSON_TOOL": str(json_tool),
            "WPA_LOG": str(log),
            "WPA_COUNTER": str(root / "wpa.counter"),
            "WPA_PINGS": str(root / "wpa.pings"),
        }
        return environment, log

    def _write_iwlist_fake(self, fake_bin: Path, cells: str) -> None:
        executable(
            fake_bin / "iwlist",
            "#!/bin/sh\n"
            "[ \"$1\" = wlan0 ] && [ \"$2\" = scan ] || exit 1\n"
            "cat <<'SCAN'\n" + cells + "SCAN\n",
        )

    def test_wifi_hook_preserves_spaces_as_single_wpa_arguments(self) -> None:
        hook = REPOSITORY / "runtime/slot/hooks/wifi-apply.sh"
        fixture = REPOSITORY / "tools/tests/wifi-space.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment, log = self._wifi_apply_environment(root)
            # No iwlist on PATH: the hook must treat a scan it cannot run as
            # "no usable 5 GHz" and still join, on 2.4 GHz, rather than fail.
            result = subprocess.run(
                [str(hook), str(fixture)],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8")
            self.assertIn('5|-iwlan0|set_network|0|ssid|"Family Room WiFi"', calls)
            self.assertIn('5|-iwlan0|set_network|0|psk|"safe passphrase!"', calls)

    def test_slot_start_does_not_apply_wifi(self) -> None:
        """start.sh is bounded to 15s and runs long before wpa_supplicant
        exists, so it must not try to apply the committed network: doing so
        either configured nothing or ran past the bound and killed the slot
        before the daemon started. The supervisor owns this instead."""
        start = (REPOSITORY / "runtime/slot/start.sh").read_text(encoding="utf-8")
        self.assertNotIn('"$JL_SLOT_DIR/hooks/wifi-apply.sh"', start)
        boot = (REPOSITORY / "runtime/boot/boot.sh").read_text(encoding="utf-8")
        self.assertIn("jl_ensure_wifi", boot)
        common = (REPOSITORY / "runtime/boot/common.sh").read_text(encoding="utf-8")
        # It only acts once wpa_supplicant answers, and not forever.
        self.assertIn("wpa_cli -iwlan0 ping", common)
        self.assertIn("wifi-attempts", common)

    def test_wifi_hook_waits_for_wpa_supplicant_control_socket(self) -> None:
        """At boot the hook can run before wpa_supplicant's control socket
        exists. It must wait for it rather than exiting, or the committed
        network is silently never applied and the camera keeps the OEM one."""
        hook = REPOSITORY / "runtime/slot/hooks/wifi-apply.sh"
        fixture = REPOSITORY / "tools/tests/wifi-space.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment, log = self._wifi_apply_environment(root)
            environment["PING_FAILS"] = "3"
            result = subprocess.run(
                [str(hook), str(fixture)],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8")
            # It kept probing past the failures and still configured the network.
            self.assertGreaterEqual(
                int((root / "wpa.pings").read_text(encoding="utf-8").strip()), 4
            )
            self.assertIn('set_network|0|ssid|"Family Room WiFi"', calls)

    def test_wifi_hook_prefers_5ghz_when_scan_shows_it_usable(self) -> None:
        hook = REPOSITORY / "runtime/slot/hooks/wifi-apply.sh"
        fixture = REPOSITORY / "tools/tests/wifi-space.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment, log = self._wifi_apply_environment(root)
            self._write_iwlist_fake(
                root / "bin",
                "          Cell 01 - Address: AA:BB:CC:DD:EE:01\n"
                "                    Channel:1\n"
                "                    Frequency:2.412 GHz (Channel 1)\n"
                "                    Quality=70/70  Signal level=-40 dBm\n"
                "                    Encryption key:on\n"
                "                    ESSID:\"Family Room WiFi\"\n"
                "          Cell 02 - Address: AA:BB:CC:DD:EE:02\n"
                "                    Channel:149\n"
                "                    Frequency:5.745 GHz (Channel 149)\n"
                "                    Quality=60/70  Signal level=-50 dBm\n"
                "                    Encryption key:on\n"
                "                    ESSID:\"Family Room WiFi\"\n",
            )
            result = subprocess.run(
                [str(hook), str(fixture)],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8")
            # Network 0 is added first: 5 GHz, higher priority.
            self.assertIn("|-iwlan0|set_network|0|freq_list|5180 ", calls)
            self.assertIn("5|-iwlan0|set_network|0|priority|2", calls)
            self.assertIn("|-iwlan0|set_network|1|freq_list|2412 ", calls)
            self.assertIn("5|-iwlan0|set_network|1|priority|1", calls)

    def test_wifi_hook_falls_back_to_24ghz_when_5ghz_is_weak(self) -> None:
        hook = REPOSITORY / "runtime/slot/hooks/wifi-apply.sh"
        fixture = REPOSITORY / "tools/tests/wifi-space.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            environment, log = self._wifi_apply_environment(root)
            self._write_iwlist_fake(
                root / "bin",
                "          Cell 01 - Address: AA:BB:CC:DD:EE:02\n"
                "                    Channel:149\n"
                "                    Frequency:5.745 GHz (Channel 149)\n"
                "                    Quality=10/70  Signal level=-85 dBm\n"
                "                    Encryption key:on\n"
                "                    ESSID:\"Family Room WiFi\"\n",
            )
            result = subprocess.run(
                [str(hook), str(fixture)],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8")
            # A 5 GHz BSS exists but is too weak to trust: 2.4 GHz goes first.
            self.assertIn("|-iwlan0|set_network|0|freq_list|2412 ", calls)
            self.assertIn("5|-iwlan0|set_network|0|priority|2", calls)
            self.assertIn("|-iwlan0|set_network|1|freq_list|5180 ", calls)
            self.assertIn("5|-iwlan0|set_network|1|priority|1", calls)


if __name__ == "__main__":
    unittest.main()
