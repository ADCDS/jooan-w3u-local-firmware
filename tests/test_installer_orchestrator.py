#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The end-to-end installer engine: ordering, resume, and safety gates.

These tests exercise installer/install.py without a camera: steps are driven in
dry-run or with the network/subprocess seams stubbed, so they assert the
engine's decisions (order, idempotent resume, the flash-requires-backup gate,
build-input assembly) rather than any device behaviour.
"""
import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
MODULE = REPO / "installer/install.py"

spec = importlib.util.spec_from_file_location("jooan_installer", MODULE)
inst = importlib.util.module_from_spec(spec)
# Register before exec so dataclasses can resolve the module by name.
sys.modules["jooan_installer"] = inst
spec.loader.exec_module(inst)


class Silent(inst.Reporter):
    def __init__(self):
        self.lines = []
        self.confirmed = True

    def step(self, name, message):
        self.lines.append(f"{name}:{message}")

    def detail(self, message):
        self.lines.append(f"  {message}")

    def warn(self, message):
        self.lines.append(f"  !{message}")

    def confirm(self, prompt):
        return self.confirmed


def make_args(work, **over):
    base = dict(
        camera="172.16.1.30", work=str(work),
        platform_pkg=str(REPO / "dist/JOOAN_FW_PKG"),
        media_release="", camera_password="admin", stream_password="",
        ssh_key="", ssh_pubkey="", only=None, force=False,
        assume_yes=False, dry_run=True,
    )
    base.update(over)
    return argparse.Namespace(**base)


class OrchestratorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = pathlib.Path(self.tmp.name) / "work"
        self.release = pathlib.Path(self.tmp.name) / "release"
        (self.release / "bin").mkdir(parents=True)
        for name in ("jooanipc-open", "libatomic.so.1", "libimp.so"):
            (self.release / "bin" / name).write_text("x")
        # Avoid any real network: stub platform status to "already up".
        self._real_status = inst._platform_status
        inst._platform_status = lambda host: {"release_version": "0.2.39"}
        self.addCleanup(setattr, inst, "_platform_status", self._real_status)

    def _ctx(self, **over):
        args = make_args(self.work, media_release=str(self.release), **over)
        state = inst.RunState.load(self.work / "run-state.json")
        if args.camera:
            state.camera_ip = args.camera
        return inst.Context(args=args, state=state,
                            state_path=self.work / "run-state.json",
                            report=Silent(), key=None)

    def test_dry_run_completes_all_steps_in_order(self):
        ctx = self._ctx()
        rc = inst.run(ctx)
        self.assertEqual(rc, 0)
        self.assertEqual(ctx.state.completed, list(inst.STEPS))

    def test_resume_skips_completed_steps(self):
        ctx = self._ctx()
        inst.run(ctx)
        ctx2 = self._ctx()
        inst.run(ctx2)
        skips = [ln for ln in ctx2.report.lines if "already done" in ln]
        self.assertEqual(len(skips), len(inst.STEPS))

    def test_checkpoint_is_persisted_between_runs(self):
        ctx = self._ctx()
        inst.run(ctx)
        data = json.loads((self.work / "run-state.json").read_text())
        self.assertEqual(data["completed"], list(inst.STEPS))
        self.assertEqual(data["camera_ip"], "172.16.1.30")

    def test_flash_refuses_without_backup(self):
        """The irreversible step must refuse if backup is not marked done."""
        ctx = self._ctx(only="flash", dry_run=False, assume_yes=True)
        # backup not in completed -> flash must raise/stop.
        rc = inst.run(ctx, only="flash")
        self.assertEqual(rc, 1)
        self.assertTrue(any("without a completed backup" in ln for ln in ctx.report.lines))

    def test_flash_confirmation_can_decline(self):
        ctx = self._ctx(dry_run=False)
        ctx.state.mark("backup")
        ctx.state.backup_dir = str(self.work / "backup")
        ctx.report.confirmed = False
        # Run only flash; it must stop when confirmation is declined.
        rc = inst.run(ctx, only="flash")
        self.assertEqual(rc, 1)
        self.assertTrue(any("not confirmed" in ln for ln in ctx.report.lines))

    def test_build_requires_squashfs_tools_message(self):
        """Without mksquashfs the build step explains what is missing (non-dry-run)."""
        import shutil as _sh
        real_which = _sh.which
        inst_which = inst.shutil.which
        inst.shutil.which = lambda name: None if name in ("mksquashfs", "unsquashfs") else real_which(name)
        self.addCleanup(setattr, inst.shutil, "which", inst_which)
        ctx = self._ctx(only="build", dry_run=False)
        ctx.state.mark("backup")
        ctx.state.backup_dir = str(self.work / "backup")
        (self.work / "backup").mkdir(parents=True, exist_ok=True)
        (self.work / "backup" / "camera-mtd4.bin").write_bytes(b"x" * inst.APPFS_BYTES)
        rc = inst.run(ctx, only="build")
        self.assertEqual(rc, 1)
        self.assertTrue(any("squashfs-tools" in ln for ln in ctx.report.lines))

    def test_build_assembles_correct_inputs(self):
        """Dry-run build names the appfs, daemon, libimp and libatomic from the release."""
        ctx = self._ctx()
        ctx.state.mark("provision"); ctx.state.mark("platform")
        ctx.state.backup_dir = str(self.work / "backup")
        (self.work / "backup").mkdir(parents=True, exist_ok=True)
        inst.step_build(ctx)
        build_line = next(ln for ln in ctx.report.lines if "build-appfs.sh" in ln)
        self.assertIn("--first", build_line)
        self.assertIn("camera-mtd4.bin", build_line)
        self.assertIn("jooanipc-open", build_line)
        self.assertIn("libimp.so", build_line)
        self.assertIn("libatomic.so.1", build_line)

    def test_only_runs_single_step(self):
        ctx = self._ctx(only="provision")
        rc = inst.run(ctx, only="provision")
        self.assertEqual(rc, 0)
        self.assertEqual(ctx.state.completed, ["provision"])

    def test_steps_constant_matches_dispatch(self):
        self.assertEqual(set(inst.STEPS), set(inst.STEP_FUNCS))


if __name__ == "__main__":
    unittest.main()
