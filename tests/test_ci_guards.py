from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GuardTests(unittest.TestCase):
    def run_script(self, script: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(ROOT / "ci" / script), *arguments],
            text=True, capture_output=True,
        )

    def test_hygiene_accepts_source_and_rejects_private_key(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "safe.c").write_text("int main(void) { return 0; }\n")
            self.assertEqual(self.run_script("source_hygiene.py", str(root)).returncode, 0)
            marker = "-----BEGIN " + "PRIVATE KEY-----\nnot-public\n"
            (root / "leak.txt").write_text(marker)
            result = self.run_script("source_hygiene.py", str(root))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("private key", result.stderr)

    def test_hygiene_rejects_elf_and_vendor_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "payload").write_bytes(b"\x7fELF" + b"\0" * 32)
            (root / "driver.ko").write_bytes(b"opaque")
            result = self.run_script("source_hygiene.py", str(root))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ELF", result.stderr)
            self.assertIn(".ko", result.stderr)

    def test_hygiene_rejects_embedded_default_credential(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            credential = "change" + "-me-now"
            (root / "auth.c").write_text(
                'const char *bootstrap = "' + credential + '";\n'
            )
            result = self.run_script("source_hygiene.py", str(root))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("embedded default credential", result.stderr)

    def test_compatibility_hash_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "abi.json"
            data.write_text("{}\n")
            expected = hashlib.sha256(data.read_bytes()).hexdigest()
            manifest = root / "pins.sha256"
            manifest.write_text(f"{expected}  abi.json\n")
            result = self.run_script(
                "check_compatibility.py", str(manifest), "--root", str(root)
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            data.write_text("changed\n")
            result = self.run_script(
                "check_compatibility.py", str(manifest), "--root", str(root)
            )
            self.assertNotEqual(result.returncode, 0)

    def test_persistent_size_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "exact").write_bytes(b"x" * (180 * 1024))
            result = self.run_script("check_persistent_size.py", str(root))
            self.assertEqual(result.returncode, 0, result.stderr)
            (root / "over").write_bytes(b"x")
            result = self.run_script("check_persistent_size.py", str(root))
            self.assertNotEqual(result.returncode, 0)

    def test_reproducible_gate_accepts_and_rejects(self) -> None:
        stable = 'mkdir -p "$BUILD_OUT"; printf stable > "$BUILD_OUT/fw.pkg"'
        result = self.run_script(
            "check_reproducible.py", "--root", str(ROOT),
            "--command", stable, "--artifact", "fw.pkg",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        unstable = ('mkdir -p "$BUILD_OUT"; '
                    'python3 -c "import time; print(time.time_ns())" > "$BUILD_OUT/fw.pkg"')
        result = self.run_script(
            "check_reproducible.py", "--root", str(ROOT),
            "--command", unstable, "--artifact", "fw.pkg",
        )
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
