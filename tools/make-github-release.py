#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Put together the files of one GitHub release, from a clean, merged checkout.

    make-github-release.py BUNDLE_DIR --openimp CHECKOUT --out DIR

BUNDLE_DIR is the bundle flashed on a camera: only the daemon and its two libraries are
read out of its image, so what gets published is what has been running. The rest of that
image is vendor files and stays private. Publishing is a separate step, so that the output
can be looked at first:

    gh release create vX.Y.Z DIR/* --title ... --notes-file ...
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY / "tools"))

PARTITION_BYTES = 3211264
PLATFORM = ("JOOAN_FW_PKG", "JOOAN_FW_PKG.sha256", "JOOAN_UNINSTALL", "JOOAN_UNINSTALL.sha256",
            "manifest.json", "manifest.json.sig")
# Everything an owner needs from this tree to build and install their own image.
MEDIA_FILES = (
    "media/scripts/install-camera.py",
    "media/scripts/first-appfs.sh",
    "media/scripts/upgrade-appfs.sh",
    "media/scripts/oem-boot-handoff.sh",
    "media/scripts/native-boot.sh",
    "media/tools/build-appfs.sh",
    "media/config/ja-a12.example.ini",
    "media/config/ja-a12-appfs-keep.txt",
    "packaging/targets/ja-a12.json",
    "LICENSE",
    "media/LICENSE",
    "media/vendor/licenses/mbedtls-LICENSE",
    "media/vendor/cjson/LICENSE",
    "media/docs/third-party.md",
    "docs/THIRD_PARTY.md",
    "docs/INSTALL.md",
)
EXECUTABLE = {"media/scripts/install-camera.py", "media/tools/build-appfs.sh"}
# What the T23 library is built from: the corresponding source of libimp.so, without the
# fork's reverse-engineering notes and tools, which are not build inputs.
OPENIMP_SOURCE = ("src", "include", "build-t23.sh", "tools/openimp-tuningd.c", "tools/t23_helix_selftest.c",
                  "tools/t23_ivdc_lifecycle_probe.c", "tools/tuning_startup.h", "NOTICE", "README.md", "BUILD.md", "Makefile")
OPENIMP_NOTICES = ("NOTICE", "src/audio/webrtc/LICENSE", "src/audio/webrtc/PATENTS", "src/audio/webrtc/LICENSE_THIRD_PARTY")
LIBATOMIC_NOTICE = """\
libatomic.so.1 is GNU libatomic from the GCC 5.4 runtime of the Ingenic MIPS toolchain, unchanged.
License: GNU General Public License version 3 or later, with the GCC Runtime Library Exception 3.1.
  https://www.gnu.org/licenses/gpl-3.0.txt
  https://www.gnu.org/licenses/gcc-exception-3.1.html
Source: GCC 5.4, https://gcc.gnu.org/ (libatomic/). The toolchain is fetched by tools/fetch-ingenic-toolchain.sh
in this project's repository, whose docs/BUILDING.md describes it.
"""
PRIVATE_KEY = re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----\r?\n[A-Za-z0-9+/]{32}")


def run(*command: str, cwd: Path | None = None) -> str:
    return subprocess.run(command, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def add_bytes(archive: tarfile.TarFile, name: str, data: bytes, executable: bool = False) -> None:
    member = tarfile.TarInfo(name)
    member.size, member.mode = len(data), 0o755 if executable else 0o644
    archive.addfile(member, io.BytesIO(data))


def platform_files(dist: Path, target: Path) -> tuple[str, int]:
    """Check the signed platform release; returns its version and sequence."""
    import upload_ota

    for name in PLATFORM:
        if not (dist / name).is_file():
            raise SystemExit(f"{dist / name} is missing: build the platform release first")
    for name in ("JOOAN_FW_PKG", "JOOAN_UNINSTALL"):
        said = (dist / f"{name}.sha256").read_text().split()[0]
        if said != digest((dist / name).read_bytes()):
            raise SystemExit(f"{name} does not match its checksum file")
    return upload_ota.verify_release_index(dist / "JOOAN_FW_PKG", dist / "manifest.json", dist / "manifest.json.sig", target)


def read_release(bundle: Path) -> tuple[str, dict[str, bytes]]:
    """The release ID and its three program files, and nothing else, from a bundle's image."""
    spec = json.loads((bundle / "bundle.json").read_text())["jooanipc_open"]
    release = spec["release"]
    names = ("jooanipc-open", "libimp.so", "libatomic.so.1")
    with tempfile.TemporaryDirectory(prefix="github-release-") as temporary:
        tree = Path(temporary) / "x"
        run("unsquashfs", "-q", "-d", str(tree), str(bundle / spec["image"]),
            *(f"jooanipc-open/releases/{release}/{name}" for name in names))
        files = {name: (tree / "jooanipc-open/releases" / release / name).read_bytes() for name in names}
    if release != f"{digest(files['jooanipc-open'])[:8]}-{digest(files['libimp.so'])[:8]}":
        raise SystemExit(f"release {release} does not match the files in its image")
    return release, files


def readme(release: str, commit: str, library: str) -> str:
    return f"""\
jooanipc-open media release {release}, from {commit}.
For the JOOAN W3-U (JA-A12) only, with platform 0.2.34 or later already installed.

A FIRST INSTALL HAS NOT YET BEEN RUN ON A REAL CAMERA. Read "The media layer" in docs/INSTALL.md
first. Power lost while it writes can only be recovered with a flash programmer and a backup.

You build the image yourself, from your own camera, because it contains vendor files.
Keep that image private. It needs Linux or WSL with squashfs-tools, Python 3, OpenSSH and OpenSSL.

  ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
  tar xf ../{library}.tar.gz -C ..
  sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../{library}/libimp.so bin/libatomic.so.1 my-bundle
  CAMERA_PASSWORD=... python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP

That last command only checks the camera. It tells you what it needs to go ahead.
"""


def media_archive(out: Path, release: str, commit: str, files: dict[str, bytes], library: str) -> str:
    name = f"jooanipc-open-{release}"
    with tarfile.open(out / f"{name}.tar.gz", "w:gz") as archive:
        add_bytes(archive, f"{name}/README.txt", readme(release, commit[:12], library).encode())
        add_bytes(archive, f"{name}/bin/jooanipc-open", files["jooanipc-open"], True)
        add_bytes(archive, f"{name}/bin/libatomic.so.1", files["libatomic.so.1"], True)
        add_bytes(archive, f"{name}/bin/libatomic.NOTICE.txt", LIBATOMIC_NOTICE.encode())
        for path in MEDIA_FILES:
            add_bytes(archive, f"{name}/{path}", (REPOSITORY / path).read_bytes(), path in EXECUTABLE)
    return f"{name}.tar.gz"


def library_archives(out: Path, files: dict[str, bytes], openimp: Path) -> tuple[str, str, str]:
    commit = run("git", "rev-parse", "HEAD", cwd=openimp)
    short = commit[:7]
    library = f"libimp-{digest(files['libimp.so'])[:8]}"
    source = f"openimp-{short}-source"
    note = f"""\
libimp.so built for the Ingenic T23 from OpenIMP commit {commit}.
The exact source is in {source}.tar.gz beside this file, and rebuilds these same bytes with:
  TOOLCHAIN_PREFIX=.../bin/mips-linux-gnu T23_OUTPUT_DIR=out \\
  T23_EXTRA_CFLAGS='-muclibc -D_GNU_SOURCE=' T23_EXTRA_LDFLAGS=-muclibc sh ./build-t23.sh
Licensing is per file; see NOTICE. x264-derived code built into this library is under
GPL-2.0-or-later. NOTICE also lists files that carry no explicit licence grant.
"""
    with tarfile.open(out / f"{library}.tar.gz", "w:gz") as archive:
        add_bytes(archive, f"{library}/libimp.so", files["libimp.so"], True)
        add_bytes(archive, f"{library}/README.txt", note.encode())
        for path in OPENIMP_NOTICES:
            add_bytes(archive, f"{library}/{path.replace('src/audio/webrtc/', 'webrtc-')}", (openimp / path).read_bytes())
    tree = subprocess.run(["git", "archive", f"--prefix={source}/", "--format=tar.gz", "HEAD", "--", *OPENIMP_SOURCE],
                          cwd=openimp, check=True, capture_output=True).stdout
    (out / f"{source}.tar.gz").write_bytes(tree)
    return library, f"{library}.tar.gz", f"{source}.tar.gz"


def problem_with(name: str, data: bytes, vendor: set[str]) -> str | None:
    if data[:4] == b"hsqs" or len(data) == PARTITION_BYTES:
        return f"{name} looks like a partition image"
    if name.rsplit("/", 1)[-1] in vendor:
        return f"{name} has the name of a vendor file"
    if PRIVATE_KEY.search(data):
        return f"{name} holds a private key"
    return None


def vendor_names() -> set[str]:
    """The files a first image keeps from the stock partition, without the directories."""
    entries = [line.strip() for line in (REPOSITORY / "media/config/ja-a12-appfs-keep.txt").read_text().splitlines()
               if line.strip() and not line.startswith("#")]
    return {entry.rsplit("/", 1)[-1] for entry in entries if not any(other.startswith(entry + "/") for other in entries)}


def check_public(out: Path) -> None:
    """The last gate: what is in this directory, and in its archives, may be published."""
    vendor = vendor_names()
    for path in sorted(out.iterdir()):
        found = problem_with(path.name, path.read_bytes(), vendor)
        if not found and path.name.endswith(".tar.gz"):
            with tarfile.open(path) as archive:
                for member in archive:
                    if not member.isfile():
                        continue
                    found = problem_with(f"{path.name}:{member.name}", archive.extractfile(member).read(), vendor)
                    if found:
                        break
        if found:
            raise SystemExit(f"refusing to publish: {found}")


def write_sums(out: Path) -> None:
    lines = [f"{digest(path.read_bytes())}  {path.name}\n" for path in sorted(out.iterdir()) if path.name != "SHA256SUMS"]
    (out / "SHA256SUMS").write_text("".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--openimp", type=Path, required=True, help="OpenIMP checkout at the commit libimp.so was built from")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if run("git", "status", "--porcelain", cwd=REPOSITORY):
        raise SystemExit("the checkout has uncommitted changes")
    commit = run("git", "rev-parse", "HEAD", cwd=REPOSITORY)
    if subprocess.run(["git", "merge-base", "--is-ancestor", commit, "origin/main"], cwd=REPOSITORY).returncode:
        raise SystemExit(f"{commit[:12]} is not on origin/main: merge and push first")
    if run("git", "status", "--porcelain", cwd=args.openimp) or not run("git", "branch", "-r", "--contains", "HEAD", cwd=args.openimp):
        raise SystemExit("the OpenIMP checkout has changes, or a commit that is not public")
    version, sequence = platform_files(REPOSITORY / "dist", REPOSITORY / "packaging/targets/ja-a12.json")
    release, files = read_release(args.bundle)
    args.out.mkdir(parents=True, exist_ok=True)
    if any(args.out.iterdir()):
        raise SystemExit(f"{args.out} is not empty")
    for name in PLATFORM:
        shutil.copyfile(REPOSITORY / "dist" / name, args.out / name)
    library, library_file, source_file = library_archives(args.out, files, args.openimp)
    media = media_archive(args.out, release, commit, files, library)
    check_public(args.out)
    write_sums(args.out)
    print(f"platform {version} sequence {sequence}; media {release}; from {commit[:12]}")
    for path in sorted(args.out.iterdir()):
        print(f"  {path.stat().st_size:>9}  {path.name}")
    print(f"Check {media}, {library_file} and {source_file}, then: gh release create v{version} {args.out}/* ...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
