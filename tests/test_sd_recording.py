#!/usr/bin/env python3
"""Integration-helper tests for the microSD recording curator.

These exercise the real hook logic that the daemon's /api/v1/storage and
/api/v1/recordings routes delegate to: enumerating the OEM recorder's on-card
files and deleting them safely. The C routes are thin pass-throughs over these
ops.
"""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
HELPER = ROOT / "runtime/slot/hooks/integration-helper.sh"


def run(op, path="-", ident="-", env=None):
    result = subprocess.run(
        ["sh", str(HELPER), op, path, ident],
        capture_output=True, text=True,
        env={**os.environ, **(env or {})},
    )
    return result.returncode, result.stdout


class SdRecordingHelper(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.sd = pathlib.Path(self.tmp.name) / "sd"
        self.base = self.sd / "JOOAN_RECORD"
        (self.base / "20260921").mkdir(parents=True)
        (self.base / "20260920").mkdir(parents=True)
        (self.base / "notaday").mkdir(parents=True)  # must be ignored
        (self.base / "20260921" / "1758400000").write_bytes(b"clipone")
        (self.base / "20260921" / "1758401000").write_bytes(b"cliptwolonger")
        (self.base / "20260920" / "1758300000").write_bytes(b"d2")
        # OEM sidecars that share the day folder but are NOT clips.
        (self.base / "20260921" / "1758400000.index").write_bytes(b"index-bytes")
        (self.base / "20260921" / "alarm_index.log").write_bytes(b"")
        self.env = {"JL_SD_DIR": str(self.sd)}

    def test_days_lists_only_valid_day_folders(self):
        code, out = run("recordings-days", env=self.env)
        self.assertEqual(code, 0)
        days = {d["day"]: d for d in json.loads(out)["days"]}
        self.assertEqual(set(days), {"20260920", "20260921"})
        self.assertEqual(days["20260921"]["count"], 2)

    def test_list_reports_clip_sizes(self):
        code, out = run("recordings-list", ident="20260921", env=self.env)
        self.assertEqual(code, 0)
        clips = {c["name"]: c["size"] for c in json.loads(out)["clips"]}
        self.assertEqual(clips, {"1758400000": 7, "1758401000": 13})

    def test_list_rejects_non_day(self):
        self.assertEqual(run("recordings-list", ident="2026", env=self.env)[0], 2)
        self.assertEqual(run("recordings-list", ident="../etc", env=self.env)[0], 2)

    def test_delete_single_clip(self):
        code, out = run("recordings-delete", ident="20260921/1758400000", env=self.env)
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(out)["ok"])
        self.assertFalse((self.base / "20260921" / "1758400000").exists())
        # its OEM index sidecar goes with it
        self.assertFalse((self.base / "20260921" / "1758400000.index").exists())
        self.assertTrue((self.base / "20260921" / "1758401000").exists())

    def test_delete_whole_day(self):
        self.assertEqual(run("recordings-delete", ident="20260920", env=self.env)[0], 0)
        self.assertFalse((self.base / "20260920").exists())

    def test_delete_rejects_traversal(self):
        victim = pathlib.Path(self.tmp.name) / "outside"
        victim.write_text("keep")
        for bad in ("../../outside", "/etc/passwd", "20260921/../../outside", "20260921/"):
            self.assertEqual(run("recordings-delete", ident=bad, env=self.env)[0], 2, bad)
        self.assertTrue(victim.exists())


if __name__ == "__main__":
    unittest.main()
