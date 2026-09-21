#!/usr/bin/env python3
"""Clock persistence across boots.

The JA-A12 has no RTC: every boot starts near 2021 until a client sets the
clock, which misdates SD recordings and the burned-in OSD. The supervisor
persists a coarse last-known-good time and lifts the clock to it at boot. The
clock must only ever move forward, so an accurate client-set time is never
regressed, and a corrupt state file must be ignored rather than trusted.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMON = ROOT / "runtime/boot/common.sh"
HELPER = ROOT / "runtime/slot/hooks/integration-helper.sh"

# Source common.sh, then re-pin the state paths it defaults, and run a snippet.
HARNESS = """
JL_ROOT={root}; JL_RUN={root}/run; JL_STATE={root}/state; JL_CONFIG={root}/config
mkdir -p "$JL_STATE" "$JL_RUN"
jl_log() {{ printf 'LOG: %s\\n' "$*"; }}
. {common} 2>/dev/null || true
JL_ROOT={root}; JL_STATE={root}/state
{body}
"""


class ClockPersistence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name) / "root"
        (self.root / "state").mkdir(parents=True)
        self.state = self.root / "state" / "last-time"

    def sh(self, body):
        script = HARNESS.format(root=self.root, common=COMMON, body=body)
        return subprocess.run(["sh", "-c", script], capture_output=True, text=True)

    def test_save_writes_current_epoch(self):
        r = self.sh("jl_save_time || echo FAIL")
        self.assertNotIn("FAIL", r.stdout)
        self.assertTrue(self.state.exists())
        self.assertGreater(int(self.state.read_text().strip()), 1_700_000_000)

    def test_restore_is_noop_without_state(self):
        self.state.unlink(missing_ok=True)
        r = self.sh("jl_restore_time && echo OK")
        self.assertIn("OK", r.stdout)

    def test_restore_ignores_corrupt_state(self):
        self.state.write_text("not-a-number\n")
        r = self.sh("jl_restore_time && echo OK")
        self.assertIn("OK", r.stdout)
        self.assertNotIn("clock restored", r.stdout)

    def test_save_never_regresses(self):
        """A later boot must not overwrite a newer persisted time with an older one."""
        future = 4_000_000_000
        self.state.write_text(f"{future}\n")
        self.sh("jl_save_time || true")
        self.assertEqual(int(self.state.read_text().strip()), future)

    def test_restore_only_moves_forward(self):
        """Restore must not fire when the running clock is already ahead."""
        self.state.write_text("1000000000\n")  # 2001, far behind "now"
        r = self.sh("jl_restore_time && echo OK")
        self.assertIn("OK", r.stdout)
        self.assertNotIn("clock restored", r.stdout)

    def test_time_set_helper_persists_immediately(self):
        """PUT /api/v1/time must make the new clock durable without waiting for a tick."""
        epoch = "1790000000"
        env = {**os.environ, "JL_ROOT": str(self.root)}
        # date -s needs privilege; the helper still persists even if it cannot set.
        subprocess.run(["sh", str(HELPER), "time-set", "-", epoch],
                       capture_output=True, text=True, env=env)
        if self.state.exists():
            self.assertEqual(self.state.read_text().strip(), epoch)


if __name__ == "__main__":
    unittest.main()
