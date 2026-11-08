#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The GUI's engine-facing logic, without opening a window.

The window itself is Tk and needs a display, but everything that matters for
correctness -- the Reporter subclass that turns engine callbacks into UI events,
the cross-thread confirmation, and that the engine runs to completion when driven
with the GUI's argument object -- is testable headless. If Tkinter is missing,
the whole module is skipped.
"""
import glob
import importlib.util
import pathlib
import queue
import sys
import tempfile
import types
import unittest

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "installer"))

try:
    import tkinter  # noqa: F401
    _HAVE_TK = True
except Exception:
    _HAVE_TK = False

# install.py is importable without a display; gui.py imports tkinter at module
# load, so only import it when Tk is present.
spec = importlib.util.spec_from_file_location("install_engine", REPO / "installer/install.py")
engine = importlib.util.module_from_spec(spec)
sys.modules["install_engine"] = engine
spec.loader.exec_module(engine)


def _media_release_dir():
    for d in glob.glob(str(pathlib.Path.home() /
                           "dev/cameras/releases/owner-test-20261107/jooanipc-open-*")):
        if pathlib.Path(d).is_dir():
            return d
    return None


@unittest.skipUnless(_HAVE_TK, "Tkinter not available")
class GuiLogic(unittest.TestCase):
    def setUp(self):
        import gui
        self.gui = gui

    def test_step_titles_cover_every_engine_step(self):
        self.assertEqual(set(self.gui.STEP_TITLES), set(self.gui.engine.STEPS))

    def test_reporter_emits_threadsafe_events(self):
        q = queue.Queue()

        class C:
            def ask(self, p):
                return True
        rep = self.gui.GuiReporter(q, C())
        rep.step("backup", "saving")
        rep.detail("mtd0")
        rep.warn("careful")
        self.assertEqual(q.get_nowait(), ("step", "backup", "saving"))
        self.assertEqual(q.get_nowait(), ("detail", "mtd0"))
        self.assertEqual(q.get_nowait(), ("warn", "careful"))

    def test_reporter_confirm_delegates_to_box(self):
        q = queue.Queue()

        class Yes:
            def ask(self, prompt):
                self.seen = prompt
                return True

        class No:
            def ask(self, prompt):
                return False
        self.assertTrue(self.gui.GuiReporter(q, Yes()).confirm("go?"))
        self.assertFalse(self.gui.GuiReporter(q, No()).confirm("go?"))

    def test_engine_runs_to_completion_with_gui_args(self):
        """The engine must accept the GUI's SimpleNamespace args and finish in dry-run."""
        rel = _media_release_dir()
        if not rel:
            self.skipTest("media release fixture not present")
        q = queue.Queue()

        class C:
            def ask(self, p):
                return True
        rep = self.gui.GuiReporter(q, C())
        work = tempfile.mkdtemp(prefix="gui-test-")
        self.addCleanup(lambda: __import__("shutil").rmtree(work, ignore_errors=True))
        args = types.SimpleNamespace(
            camera="203.0.113.1", work=work, platform_pkg="dist/JOOAN_FW_PKG",
            media_release=rel, camera_password="admin", stream_password="",
            ssh_key="", ssh_pubkey="", provision_helper="", only=None,
            force=False, assume_yes=False, dry_run=True)
        real_status = self.gui.engine._platform_status
        self.gui.engine._platform_status = lambda host: {"release_version": "0.2.39"}
        self.addCleanup(setattr, self.gui.engine, "_platform_status", real_status)
        state = self.gui.engine.RunState.load(pathlib.Path(work) / "run-state.json")
        state.camera_ip = "203.0.113.1"
        ctx = self.gui.engine.Context(
            args=args, state=state, state_path=pathlib.Path(work) / "run-state.json",
            report=rep, key=None)
        rc = self.gui.engine.run(ctx)
        self.assertEqual(rc, 0)
        self.assertEqual(state.completed, list(self.gui.engine.STEPS))


if __name__ == "__main__":
    unittest.main()
