#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make the package that the admin UI's System page installs, from a camera release bundle.

Host tool. The image in a bundle is built from that camera's own app partition and holds
vendor files, so a package fits that camera only and must stay private. The manifest is
signed with the platform's release key, which is why the camera can check it with the
verifier the platform already has. It needs squashfs-tools to read the image.

    make-media-update.py BUNDLE_DIR [--out FILE] [--key PEM]
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MEDIA = HERE.parent
REPOSITORY = MEDIA.parent
sys.path.insert(0, str(REPOSITORY / "src"))
from ironman.signing import require_matching_key, secure_default_key_path, sign_bytes, verify_bytes  # noqa: E402

MAGIC = "JOOAN-SIGNED-RELEASE-V1"
HEADER = 96
IMAGE_BYTES = 3211264


def digest(data, name="sha256"):
    return hashlib.new(name, data).hexdigest()


def release_files(image, release):
    """The sums the camera's boot checks, read from the image itself."""
    with tempfile.TemporaryDirectory(prefix="media-update-") as temporary:
        root = Path(temporary) / "image"
        where = f"jooanipc-open/releases/{release}"
        subprocess.run(["unsquashfs", "-q", "-n", "-d", str(root), str(image), where], check=True, stdout=subprocess.DEVNULL)
        folder = root / where
        return {name: (folder / name).read_bytes() for name in ("jooanipc-open", "libimp.so", "libatomic.so.1")}


def manifest_for(target, spec, image, upgrade, apply_script, files):
    authenticity, appfs = target["release_authenticity"], spec["jooanipc_open"]
    lines = [
        MAGIC,
        f"algorithm={authenticity['algorithm']}",
        f"signature_format={authenticity['signature_format']}",
        f"key_id={authenticity['key_id']}",
        f"target_id={target['target_id']}",
        f"device_model={target['device']['model']}",
        f"model_token={target['ironman']['model_token']}",
        f"release_version={appfs['release']}",
        "artifact_kind=media",
        f"previous_appfs_md5={appfs['expected_current_appfs_md5']}",
        f"appfs_md5={digest(image, 'md5')}",
        f"app_md5={digest(files['jooanipc-open'], 'md5')}",
        f"imp_md5={digest(files['libimp.so'], 'md5')}",
        f"imp_sha256={digest(files['libimp.so'])}",
        f"atomic_md5={digest(files['libatomic.so.1'], 'md5')}",
        "files-begin",
        f"{digest(image)}  appfs.img",
        f"{digest(upgrade)}  upgrade-appfs.sh",
        f"{digest(apply_script)}  apply-media-update.sh",
        "files-end",
    ]
    return ("\n".join(lines) + "\n").encode()


def package(manifest, signature, upgrade, apply_script, image):
    header = f"JIPCMEDIA1 {len(manifest)} {len(signature)} {len(upgrade)} {len(apply_script)} {len(image)}".encode()
    return header.ljust(HEADER - 1) + b"\n" + manifest + signature + upgrade + apply_script + image


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bundle", type=Path, help="a camera release bundle (bundle.json and appfs.img)")
    parser.add_argument("--out", type=Path, help="default: BUNDLE_DIR/media-<release>.pkg")
    parser.add_argument("--key", type=Path, default=secure_default_key_path(), help="the release signing key")
    args = parser.parse_args()

    spec = json.loads((args.bundle / "bundle.json").read_text())
    appfs = spec["jooanipc_open"]
    image = (args.bundle / appfs["image"]).read_bytes()
    if len(image) != IMAGE_BYTES:
        sys.exit(f"the image must be the {IMAGE_BYTES}-byte app partition")
    if digest(image, "md5") != appfs["image_md5"]:
        sys.exit("the image does not match the bundle's checksum")
    target = json.loads((REPOSITORY / "packaging/targets/ja-a12.json").read_text())
    public_key = target["release_authenticity"]["public_key_sec1"]
    require_matching_key(args.key, public_key)
    upgrade = (MEDIA / "scripts/upgrade-appfs.sh").read_bytes()
    apply_script = (MEDIA / "scripts/apply-media-update.sh").read_bytes()
    files = release_files(args.bundle / appfs["image"], appfs["release"])
    manifest = manifest_for(target, spec, image, upgrade, apply_script, files)
    signature = sign_bytes(manifest, args.key)
    verify_bytes(manifest, signature, public_key)
    out = args.out or args.bundle / f"media-{appfs['release']}.pkg"
    out.write_bytes(package(manifest, signature, upgrade, apply_script, image))
    out.chmod(0o600)
    print(f"{out}: release {appfs['release']}, for an app partition with md5 {appfs['expected_current_appfs_md5']}")


if __name__ == "__main__":
    main()
