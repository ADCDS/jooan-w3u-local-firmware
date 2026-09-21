from __future__ import annotations

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
