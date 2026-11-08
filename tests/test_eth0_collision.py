#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Carrier-less eth0 must not collide with wlan0's subnet.

Stock startapp puts 172.16.1.18/24 on a down, carrier-less eth0 before this
platform's boot hook runs. If wlan0 is on 172.16.1.0/24 the kernel then holds
two identical routes and reverse-path-drops every inbound packet sourced from
the gateway, so the camera is unreachable. jl_drop_dead_eth0 (called from
jl_local_network_policy, which runs every second in every boot mode) removes the
IPv4 address from a no-carrier eth0 and leaves a real wired link untouched.
"""
import os
import pathlib
import stat
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMON = ROOT / "runtime/boot/common.sh"

# Source common.sh, point JL_NET_SYSFS at a fake sysfs, and shim ip/ifconfig to
# a recorder so the test observes exactly what the policy would run.
HARNESS = """
JL_ROOT={root}; JL_RUN={root}/run; JL_STATE={root}/state; JL_CONFIG={root}/config
JL_NET_SYSFS={sysfs}
JL_IP={bin}/ip
JL_IFCONFIG={bin}/ifconfig
mkdir -p "$JL_STATE" "$JL_RUN"
jl_log() {{ printf 'LOG: %s\\n' "$*" >&2; }}
PATH={bin}:$PATH
. {common} 2>/dev/null || true
JL_ROOT={root}; JL_STATE={root}/state; JL_NET_SYSFS={sysfs}; JL_IP={bin}/ip; JL_IFCONFIG={bin}/ifconfig
JL_IFCONFIG={bin}/ifconfig
jl_drop_dead_eth0
"""


class Eth0Collision(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name) / "root"
        (self.root / "state").mkdir(parents=True)
        self.sysfs = pathlib.Path(self.tmp.name) / "sysfs"
        self.bin = pathlib.Path(self.tmp.name) / "bin"
        self.bin.mkdir(parents=True)
        self.calls = self.bin / "calls.log"

    def _iface(self, name, carrier=None, has_ipv4=True):
        d = self.sysfs / name
        d.mkdir(parents=True, exist_ok=True)
        if carrier is not None:
            (d / "carrier").write_text(f"{carrier}\n")
        # ifconfig shim reports an inet addr only when has_ipv4.
        self._shim("ifconfig", inet=has_ipv4)

    def _shim(self, name, inet=True):
        script = self.bin / name
        inet_line = (
            '          inet addr:172.16.1.18  Bcast:172.16.1.255  Mask:255.255.255.0\n'
            if inet else ''
        )
        script.write_text(
            "#!/bin/sh\n"
            f'printf "%s %s\\n" "$(basename "$0")" "$*" >> "{self.calls}"\n'
            # ifconfig with no further filtering prints the inet line; flush/0.0.0.0 clears it.
            'case "$*" in\n'
            '  *0.0.0.0*) exit 0 ;;\n'
            'esac\n'
            f'cat <<EOF\n'
            'eth0      Link encap:Ethernet  HWaddr 96:0C:2C:88:F8:83\n'
            f'{inet_line}'
            'EOF\n'
        )
        script.chmod(script.stat().st_mode | stat.S_IEXEC)

    def _ip_shim(self):
        script = self.bin / "ip"
        script.write_text(
            "#!/bin/sh\n"
            f'printf "ip %s\\n" "$*" >> "{self.calls}"\n'
            "exit 0\n"
        )
        script.chmod(script.stat().st_mode | stat.S_IEXEC)

    def sh(self):
        script = HARNESS.format(
            root=self.root, sysfs=self.sysfs, bin=self.bin, common=COMMON
        )
        return subprocess.run(["sh", "-c", script], capture_output=True, text=True)

    def calls_text(self):
        return self.calls.read_text() if self.calls.exists() else ""

    def test_flushes_carrierless_eth0_with_address(self):
        self._ip_shim()
        self._iface("eth0", carrier=0, has_ipv4=True)
        r = self.sh()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("addr flush dev eth0", self.calls_text())

    def test_leaves_live_eth0_untouched(self):
        """carrier==1 (real wired link) must never be flushed."""
        self._ip_shim()
        self._iface("eth0", carrier=1, has_ipv4=True)
        r = self.sh()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("flush", self.calls_text())

    def test_noop_when_eth0_has_no_address(self):
        """Nothing to do if the down interface carries no IPv4."""
        self._ip_shim()
        self._iface("eth0", carrier=0, has_ipv4=False)
        r = self.sh()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("flush", self.calls_text())

    def test_noop_when_no_eth0(self):
        """A board without eth0 must not error."""
        self._ip_shim()
        self._shim("ifconfig", inet=False)
        r = self.sh()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("flush", self.calls_text())

    def test_falls_back_to_ifconfig_when_ip_flush_unavailable(self):
        """If `ip addr flush` fails, ifconfig eth0 0.0.0.0 is the fallback."""
        # ip shim that fails, forcing the ifconfig fallback branch.
        (self.bin / "ip").write_text(
            "#!/bin/sh\n"
            f'printf "ip %s\\n" "$*" >> "{self.calls}"\n'
            "exit 1\n"
        )
        (self.bin / "ip").chmod((self.bin / "ip").stat().st_mode | stat.S_IEXEC)
        self._iface("eth0", carrier=0, has_ipv4=True)
        r = self.sh()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("eth0 0.0.0.0", self.calls_text())

    def test_policy_calls_the_flush(self):
        """Static guard: jl_local_network_policy invokes jl_drop_dead_eth0 first."""
        src = COMMON.read_text()
        policy = src.index("jl_local_network_policy() {")
        body = src[policy:policy + 200]
        self.assertIn("jl_drop_dead_eth0", body)


if __name__ == "__main__":
    unittest.main()
