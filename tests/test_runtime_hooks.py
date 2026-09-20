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

    def test_wifi_hook_preserves_spaces_as_single_wpa_arguments(self) -> None:
        hook = REPOSITORY / "runtime/slot/hooks/wifi-apply.sh"
        fixture = REPOSITORY / "tools/tests/wifi-space.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
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
                "  *add_network*) printf '0\\n' ;;\n"
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
            }
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


if __name__ == "__main__":
    unittest.main()
