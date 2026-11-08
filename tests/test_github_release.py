# SPDX-License-Identifier: GPL-2.0-only
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
MARKER = b"-----BEGIN %sPRIVATE " + b"KEY-----"  # split, or the source-hygiene gate rightly objects


def load_tool():
    path = REPOSITORY / "tools" / "make-github-release.py"
    specification = importlib.util.spec_from_file_location("make_github_release", path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def archive_with(path: Path, name: str, data: bytes) -> None:
    with tarfile.open(path, "w:gz") as archive:
        member = tarfile.TarInfo(name)
        member.size = len(data)
        archive.addfile(member, io.BytesIO(data))


class PublicGateTests(unittest.TestCase):
    """What could leak: a camera's image, a vendor file or a key, also inside an archive."""

    def refused(self, name: str, data: bytes, why: str) -> None:
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            (out / name).write_bytes(data)
            with self.assertRaisesRegex(SystemExit, why):
                tool.check_public(out)
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            archive_with(out / "x.tar.gz", f"x/{name}", data)
            with self.assertRaisesRegex(SystemExit, why):
                tool.check_public(out)

    def test_a_partition_image_is_refused(self) -> None:
        self.refused("something", b"hsqs" + bytes(100), "partition image")
        self.refused("other", bytes(3211264), "partition image")

    def test_vendor_files_are_refused_by_name(self) -> None:
        self.refused("jooanipc", b"program", "vendor file")
        self.refused("tx-isp.ko", b"module", "vendor file")
        self.refused("cv2005-t23.bin", b"tuning", "vendor file")

    def test_a_private_key_is_refused(self) -> None:
        self.refused("key.pem", MARKER % b"EC " + b"\n" + b"A" * 64, "private key")

    def test_a_program_that_only_parses_keys_passes(self) -> None:
        # The daemon's TLS library holds this marker as a string, followed by no key.
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            (out / "jooanipc-open").write_bytes(b"\x7fELF" + MARKER % b"RSA " + b"\0-----END")
            tool.check_public(out)

    def test_directory_names_in_the_keep_list_are_not_vendor_files(self) -> None:
        names = load_tool().vendor_names()
        self.assertIn("jooanipc", names)
        self.assertNotIn("lib", names)
        self.assertNotIn("run", names)


class ContentsTests(unittest.TestCase):
    def test_a_platform_file_that_does_not_match_its_checksum_is_refused(self) -> None:
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            dist = Path(temporary)
            for name in tool.PLATFORM:
                (dist / name).write_bytes(b"x")
            (dist / "JOOAN_FW_PKG.sha256").write_text("0" * 64 + "  JOOAN_FW_PKG\n")
            with self.assertRaisesRegex(SystemExit, "does not match"):
                tool.platform_files(dist, REPOSITORY / "packaging/targets/ja-a12.json")

    def test_media_archive_lists_exactly_what_an_owner_needs(self) -> None:
        tool = load_tool()
        for path in tool.MEDIA_FILES:
            self.assertTrue((REPOSITORY / path).is_file(), path)
        files = {"jooanipc-open": b"app", "libatomic.so.1": b"atomic", "libimp.so": b"imp"}
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            name = tool.media_archive(out, "aaaa-bbbb", "c" * 40, files, "libimp-bbbb")
            with tarfile.open(out / name) as archive:
                names = set(archive.getnames())
                self.assertEqual(archive.getmember("jooanipc-open-aaaa-bbbb/bin/jooanipc-open").mode, 0o755)
                readme = archive.extractfile("jooanipc-open-aaaa-bbbb/README.txt").read().decode()
            tool.check_public(out)
        self.assertNotIn("jooanipc-open-aaaa-bbbb/bin/libimp.so", names)
        self.assertIn("jooanipc-open-aaaa-bbbb/media/scripts/first-appfs.sh", names)
        self.assertIn("jooanipc-open-aaaa-bbbb/packaging/targets/ja-a12.json", names)
        self.assertIn("HAS RUN ONCE", readme)
        self.assertIn("--first my-appfs.img", readme)

    def test_sums_cover_everything_else(self) -> None:
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            (out / "a").write_bytes(b"one")
            tool.write_sums(out)
            self.assertEqual((out / "SHA256SUMS").read_text(), hashlib.sha256(b"one").hexdigest() + "  a\n")


@unittest.skipUnless(shutil.which("mksquashfs") and shutil.which("unsquashfs"), "needs squashfs-tools")
class ReleaseFromImageTests(unittest.TestCase):
    def make_bundle(self, base: Path, release: str | None = None) -> Path:
        sha = lambda data: hashlib.sha256(data).hexdigest()
        real = f"{sha(b'app')[:8]}-{sha(b'imp')[:8]}"
        release = release or real
        tree = base / "tree"
        folder = tree / "jooanipc-open/releases" / release
        folder.mkdir(parents=True)
        for name, data in (("jooanipc-open", b"app"), ("libimp.so", b"imp"), ("libatomic.so.1", b"atomic")):
            (folder / name).write_bytes(data)
        (tree / "run").mkdir()
        (tree / "run/jooanipc").write_bytes(b"a vendor program")
        bundle = base / "bundle"
        bundle.mkdir()
        subprocess.run(["mksquashfs", str(tree), str(bundle / "appfs.img"), "-comp", "xz", "-noappend", "-quiet", "-all-root"],
                       check=True, stdout=subprocess.DEVNULL)
        (bundle / "bundle.json").write_text(json.dumps({"jooanipc_open": {"release": release, "image": "appfs.img"}}))
        return bundle

    def test_only_the_three_program_files_come_out(self) -> None:
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            release, files = tool.read_release(self.make_bundle(Path(temporary)))
        self.assertEqual(sorted(files), ["jooanipc-open", "libatomic.so.1", "libimp.so"])
        self.assertEqual(files["jooanipc-open"], b"app")
        self.assertIn("-", release)

    def test_a_release_id_that_does_not_match_is_refused(self) -> None:
        tool = load_tool()
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(SystemExit, "does not match"):
                tool.read_release(self.make_bundle(Path(temporary), "aaaaaaaa-bbbbbbbb"))


if __name__ == "__main__":
    unittest.main()
