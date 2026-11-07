# SPDX-License-Identifier: MIT
"""The admin UI's language packs: the script and the browser must agree on the keys,
and every pack must be current, complete and keep its placeholders."""
import importlib.util
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "media/tools/ui-i18n.py"
spec = importlib.util.spec_from_file_location("ui_i18n", TOOL)
i18n = importlib.util.module_from_spec(spec)
spec.loader.exec_module(i18n)


class Packs(unittest.TestCase):
    def test_keys_match_the_browser(self):
        # Computed by ikey() in app.js under Node: a change on one side alone would leave
        # every translated page silently in English.
        for text, key in {
            "Sign in": "w9nig3",
            "Update stopped: do not restart the camera or cut its power": "6k2kgc",
            "The camera’s clock is not confirmed": "14ein23",
            "简体 ✓": "1lvhe31",
            "😀 emoji": "vhhzna",
        }.items():
            self.assertEqual(i18n.ikey(text), key)
        app = (ROOT / "media/ui/app.js").read_text(encoding="utf-8")
        self.assertIn("for (let i = 0; i < t.length; i++) x = Math.imul(x ^ t.charCodeAt(i), 16777619);", app)
        self.assertIn("return (x >>> 0).toString(36);", app)

    def test_packs_are_current_and_complete(self):
        r = subprocess.run([sys.executable, str(TOOL), "--check"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_pack_holds_only_data(self):
        for code in i18n.LANGS:
            text = (ROOT / f"media/ui/lang-{code}.js").read_text(encoding="utf-8")
            self.assertTrue(text.startswith("self.JIPC_PACK={") and text.endswith("};\n"), code)

    def test_a_reworded_warning_loses_its_translation(self):
        self.assertNotEqual(i18n.ikey("Do not cut power."), i18n.ikey("Do not cut power"))


if __name__ == "__main__":
    unittest.main()
