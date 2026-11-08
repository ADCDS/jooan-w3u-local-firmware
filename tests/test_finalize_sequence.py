#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Boot-time completion of an install whose finalize was interrupted.

The signed installer publishes release-sequence as the last step. A reboot in
the window between flipping the journal to legacy-retired and writing the
sequence used to leave a working but unjournaled install (release_sequence
reads 0, so no downgrade/replay protection). The installer now persists a
durable state/release-sequence.pending BEFORE the flip, and jl_finalize_sequence
in common.sh promotes it at the next boot without needing the signed package.
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
COMMON = ROOT / "runtime/boot/common.sh"

HARNESS = """
JL_ROOT={root}; JL_RUN={root}/run; JL_STATE={root}/state; JL_CONFIG={root}/config
mkdir -p "$JL_STATE" "$JL_RUN"
jl_log() {{ printf 'LOG: %s\\n' "$*"; }}
. {common} 2>/dev/null || true
JL_ROOT={root}; JL_STATE={root}/state
{body}
"""


class FinalizeSequence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name) / "root"
        self.state = self.root / "state"
        self.state.mkdir(parents=True)
        self.seq = self.state / "release-sequence"
        self.pending = self.state / "release-sequence.pending"
        self.migration = self.state / "migration-state"

    def sh(self, body="jl_finalize_sequence || echo RC_NONZERO"):
        script = HARNESS.format(root=self.root, common=COMMON, body=body)
        return subprocess.run(["sh", "-c", script], capture_output=True, text=True)

    def test_promotes_pending_when_interrupted(self):
        """legacy-retired + pending present + sequence missing -> published."""
        self.migration.write_text("legacy-retired\n")
        self.pending.write_text("73\n")
        r = self.sh()
        self.assertTrue(self.seq.exists(), r.stderr)
        self.assertEqual(self.seq.read_text().strip(), "73")
        self.assertFalse(self.pending.exists(), "pending marker must be cleared")
        self.assertIn("completing interrupted install", r.stderr)

    def test_noop_without_pending(self):
        """A clean install (no pending marker) must do nothing."""
        self.migration.write_text("legacy-retired\n")
        self.seq.write_text("73\n")
        r = self.sh()
        self.assertEqual(self.seq.read_text().strip(), "73")
        self.assertNotIn("completing interrupted install", r.stderr)

    def test_noop_when_journal_not_legacy_retired(self):
        """A pending marker left mid-migration must not publish early."""
        self.migration.write_text("runtime-staged\n")
        self.pending.write_text("73\n")
        r = self.sh()
        self.assertFalse(self.seq.exists(), "must not publish before legacy-retired")
        self.assertTrue(self.pending.exists(), "pending must remain until finalize")

    def test_drops_stale_lower_or_equal_pending(self):
        """A pending <= the published sequence is stale and must never downgrade."""
        self.migration.write_text("legacy-retired\n")
        self.seq.write_text("73\n")
        self.pending.write_text("73\n")  # equal: already published, stale
        r = self.sh()
        self.assertEqual(self.seq.read_text().strip(), "73")
        self.assertFalse(self.pending.exists(), "stale pending must be cleared")
        self.assertNotIn("completing interrupted install", r.stderr)

        self.seq.write_text("73\n")
        self.pending.write_text("50\n")  # lower: a downgrade, must be refused
        r = self.sh()
        self.assertEqual(self.seq.read_text().strip(), "73", "must never downgrade")
        self.assertFalse(self.pending.exists())
        self.assertNotIn("completing interrupted install", r.stderr)

    def test_completes_interrupted_upgrade(self):
        """An interrupted UPGRADE (pending > published) must finish to pending."""
        self.migration.write_text("legacy-retired\n")
        self.seq.write_text("73\n")
        self.pending.write_text("74\n")  # activation done, publish interrupted
        r = self.sh()
        self.assertEqual(self.seq.read_text().strip(), "74")
        self.assertFalse(self.pending.exists())
        self.assertIn("completing interrupted install", r.stderr)

    def test_ignores_corrupt_pending(self):
        """A non-numeric pending marker is ignored, not published."""
        self.migration.write_text("legacy-retired\n")
        self.pending.write_text("not-a-number\n")
        r = self.sh()
        self.assertFalse(self.seq.exists())

    def test_installer_writes_pending_before_flip(self):
        """Static guard: the installer persists pending before legacy-retired."""
        src = (ROOT / "packaging/payload/install-upgrade.sh").read_text()
        pend = src.index("release-sequence.pending")
        flip = src.index("write_state legacy-retired")
        publish = src.index('release-sequence.new"')
        self.assertLess(pend, flip, "pending marker must be written before the journal flip")
        self.assertLess(flip, publish, "sequence is published after the flip")


if __name__ == "__main__":
    unittest.main()
