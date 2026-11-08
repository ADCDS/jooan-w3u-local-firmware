#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The first-install platform gate: a fresh OTA platform must not be rejected.

A platform installed by OTA reports release_sequence 0 until a sequence is
published, but it is the correct version and can boot the media layer. The gate
must accept it on version when the sequence is still 0, while still rejecting a
genuinely old platform. Proven needed on hardware during the Phase F round trip.
"""
import importlib.util
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
MODULE = REPO / "media/scripts/install-camera.py"
spec = importlib.util.spec_from_file_location("install_camera", MODULE)
ic = importlib.util.module_from_spec(spec)
sys.modules["install_camera"] = ic
spec.loader.exec_module(ic)


class PlatformBootGate(unittest.TestCase):
    def test_fresh_ota_platform_is_accepted_on_version(self):
        """seq 0 but the contract version -> accepted (the first-install case)."""
        self.assertTrue(ic.platform_can_boot_media(
            {"release_sequence": 0, "release_version": "0.2.39"}))

    def test_newer_version_fresh_is_accepted(self):
        self.assertTrue(ic.platform_can_boot_media(
            {"release_sequence": 0, "release_version": "0.3.0"}))

    def test_old_platform_at_seq_zero_is_rejected(self):
        """seq 0 and an older version -> cannot boot the media layer."""
        self.assertFalse(ic.platform_can_boot_media(
            {"release_sequence": 0, "release_version": "0.2.10"}))

    def test_established_platform_passes_on_sequence(self):
        """A published sequence at or above the contract passes regardless of version."""
        self.assertTrue(ic.platform_can_boot_media(
            {"release_sequence": 73, "release_version": "0.1.0"}))
        self.assertTrue(ic.platform_can_boot_media(
            {"release_sequence": 200, "release_version": "9.9.9"}))

    def test_missing_fields_do_not_crash_and_reject(self):
        self.assertFalse(ic.platform_can_boot_media({}))
        self.assertFalse(ic.platform_can_boot_media({"release_sequence": None,
                                                     "release_version": None}))

    def test_version_tuple_parsing_is_robust(self):
        self.assertEqual(ic._version_tuple("0.2.39"), (0, 2, 39))
        self.assertEqual(ic._version_tuple("v0.2.39-rc1"), (0, 2, 39))
        self.assertEqual(ic._version_tuple(""), (0,))
        self.assertEqual(ic._version_tuple(None), (0,))

    def test_first_problems_no_longer_flags_fresh_platform(self):
        """The composed preflight must not list the sequence problem for a fresh 0.2.39."""
        facts = {
            "sensors": "cv2005 cv2005s1", "appfs_size": "3211264",
            "appfs_name": "appfs", "appfs_md5": "x", "native": "no",
            "enabled": "no", "daemon": "0", "wifi_json": "yes", "opt_free": "200",
            "runtime": "ready",
        }
        platform = {"release_sequence": 0, "release_version": "0.2.39", "state": "ready"}
        appfs = {"expected_current_appfs_md5": "x", "release": "r"}
        problems = ic.first_problems(facts, platform, appfs)
        self.assertFalse(any("cannot boot the media layer" in p for p in problems),
                         f"fresh platform wrongly flagged: {problems}")


if __name__ == "__main__":
    unittest.main()
