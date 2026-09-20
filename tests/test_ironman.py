from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
import sys

sys.path.insert(0, str(REPOSITORY / "src"))

from ironman.package import build_package, validate_stage  # noqa: E402
from ironman.signing import (  # noqa: E402
    public_key_bytes,
    public_key_id,
    sign_bytes,
    verify_bytes,
)
from ironman.trailer import (  # noqa: E402
    HEADER_LEN,
    PAYLOAD_LIMIT,
    PackageValidationError,
    UnencodableTrailer,
    assemble_package,
    build_trailer,
    inspect_package,
    pad_payload,
    qa_decode,
    qa_encode,
)


def legacy_md5(data: bytes) -> bytes:
    return hashlib.md5(data, usedforsecurity=False).hexdigest().encode("ascii")


def load_stage_signer():
    path = REPOSITORY / "packaging" / "sign-stage.py"
    specification = importlib.util.spec_from_file_location("test_sign_stage", path)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def temporary_release_key(root: Path) -> tuple[Path, str]:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    private = ec.generate_private_key(ec.SECP256R1())
    path = root / "release-key.pem"
    path.write_bytes(private.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    ))
    os.chmod(path, 0o600)
    return path, public_key_bytes(private.public_key()).hex()


def temporary_target(root: Path, public_key: str) -> Path:
    target = json.loads(
        (REPOSITORY / "packaging" / "targets" / "ja-a12.json").read_text()
    )
    target["release_authenticity"]["public_key_sec1"] = public_key
    target["release_authenticity"]["key_id"] = public_key_id(public_key)
    path = root / "target.json"
    path.write_text(json.dumps(target, sort_keys=True) + "\n", encoding="utf-8")
    return path


class TrailerCodecTests(unittest.TestCase):
    def test_encoded_trailer_round_trips(self) -> None:
        for size in range(128):
            payload = bytes([size]) * size
            padded, _ = pad_payload(payload)
            digest = legacy_md5(padded)
            encoded = build_trailer(len(padded), digest)
            decoded = qa_decode(encoded)
            self.assertEqual(decoded[0:5], b"toolv")
            self.assertEqual(int(decoded[8:16].split(b"\x00", 1)[0]), len(padded))
            self.assertEqual(decoded[64:96], digest)

    def test_known_public_trailer_vector(self) -> None:
        payload = b"public-vector"
        encoded = build_trailer(len(payload), legacy_md5(payload))
        self.assertEqual(
            hashlib.sha256(encoded).hexdigest(),
            "0922c49331e117d7a302b994daca8d11c7d00bb0a3575ada5ef82b061f5712aa",
        )

    def test_encoder_refuses_a_nonrepresentable_trailer(self) -> None:
        found = None
        for counter in range(4096):
            payload = f"unencodable-{counter}".encode("ascii")
            decoded = bytearray(0x60)
            decoded[0:8] = b"toolv\x00\x00\x00"
            decoded[8:16] = str(len(payload)).encode().ljust(8, b"\x00")
            decoded[64:96] = legacy_md5(payload)
            try:
                qa_encode(bytes(decoded))
            except UnencodableTrailer:
                found = bytes(decoded)
                break
        self.assertIsNotNone(found)
        with self.assertRaises(UnencodableTrailer):
            qa_encode(found)

    def test_invalid_codec_inputs_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            qa_decode(b"short")
        with self.assertRaises(ValueError):
            qa_encode(bytes(95))
        with self.assertRaises(ValueError):
            build_trailer(1, b"A" * 32)
        with self.assertRaises(ValueError):
            build_trailer(PAYLOAD_LIMIT, b"0" * 32)


class PackageCodecTests(unittest.TestCase):
    def test_package_is_deterministic_and_self_validating(self) -> None:
        payload = b"hsqs" + bytes(range(256)) * 17
        first, first_padding = assemble_package(payload)
        second, second_padding = assemble_package(payload)
        self.assertEqual(first, second)
        self.assertEqual(first_padding, second_padding)

        info = inspect_package(first, expected_model="A12")
        self.assertEqual(info.model_token, "A12")
        self.assertEqual(info.firmware_version, "05.02.31.115")
        self.assertEqual(info.package_size, len(first))
        self.assertEqual(info.payload_size + 0xC0, len(first))

    def test_package_padding_preserves_truthful_digest(self) -> None:
        selected = None
        for counter in range(4096):
            payload = f"needs-padding-{counter}".encode("ascii")
            try:
                build_trailer(len(payload), legacy_md5(payload))
            except UnencodableTrailer:
                selected = payload
                break
        self.assertIsNotNone(selected)

        package, padding_size = assemble_package(selected)
        self.assertGreater(padding_size, 0)
        info = inspect_package(package, expected_model="A12")
        payload = package[HEADER_LEN:HEADER_LEN + info.payload_size]
        self.assertEqual(info.payload_md5, legacy_md5(payload).decode("ascii"))
        self.assertTrue(payload.startswith(selected))

    def test_tampering_and_wrong_model_are_rejected(self) -> None:
        package, _ = assemble_package(b"payload")
        with self.assertRaises(PackageValidationError):
            inspect_package(package, expected_model="not-A12")

        tampered = bytearray(package)
        tampered[HEADER_LEN] ^= 0x01
        with self.assertRaises(PackageValidationError):
            inspect_package(bytes(tampered))

        bad_size = bytearray(package)
        bad_size[8:16] = b"1\x00\x00\x00\x00\x00\x00\x00"
        with self.assertRaises(PackageValidationError):
            inspect_package(bytes(bad_size))

        bad_trailer = bytearray(package)
        bad_trailer[-1] ^= 0x04
        with self.assertRaises(PackageValidationError):
            inspect_package(bytes(bad_trailer))


class SignedReleaseTests(unittest.TestCase):
    def test_rfc6979_signature_is_deterministic_and_tamper_evident(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            key, public = temporary_release_key(root)
            data = b"signed release manifest\n"
            first = sign_bytes(data, key)
            second = sign_bytes(data, key)
            self.assertEqual(first, second)
            verify_bytes(data, first, public)
            with self.assertRaises(ValueError):
                verify_bytes(data + b"tamper", first, public)

    def test_signed_stage_inventory_rejects_tampering(self) -> None:
        signer = load_stage_signer()
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            key, public = temporary_release_key(root)
            target = temporary_target(root, public)
            stage = root / "stage"
            stage.mkdir()
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o755)
            (stage / "RELEASE").write_text("test-1\n", encoding="utf-8")
            signer.sign_stage(
                stage,
                target,
                kind="install",
                release_version="test-1",
                release_sequence=1,
                signing_key=key,
            )
            metadata = signer.verify_stage(stage, target, "install")
            self.assertEqual(metadata["release_sequence"], "1")
            upgrade.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "inventory"):
                signer.verify_stage(stage, target, "install")

    def test_direct_update_helper_verifies_applies_and_cleans_mounts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            persistent = root / "persistent"
            run = root / "run"
            control = run / "controller"
            shared = control / "shared"
            boot = control / "boot"
            staging = run / "staging" / "firmware"
            fixture = root / "mounted"
            fake_bin = root / "bin"
            slot = run / "slot-A"
            for directory in (
                persistent / "state", shared, boot, staging, fixture, fake_bin,
                slot / "hooks",
            ):
                directory.mkdir(parents=True, exist_ok=True)
            (boot / "common.sh").write_text(
                'jl_local_network_policy() { echo routes-applied >> "$FAKE_MOUNT_LOG"; }\n',
                encoding="utf-8",
            )

            image = b"H" * 96 + b"SQUASHFS-PAYLOAD" + b"T" * 96
            package_id = hashlib.sha256(image).hexdigest()
            package = staging / f"{package_id}.bin"
            package.write_bytes(image)
            manifest = fixture / "release.manifest"
            manifest.write_text(
                "\n".join((
                    "JOOAN-SIGNED-RELEASE-V1",
                    "target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316",
                    "device_model=JA-A12",
                    "model_token=A12",
                    "release_version=0.2.0",
                    "release_sequence=2",
                    "minimum_sequence=1",
                    "artifact_kind=install",
                    "files-begin",
                    "0" * 64 + "  upgrade.sh",
                    "files-end",
                    "",
                )),
                encoding="utf-8",
            )
            (fixture / "release.manifest.sig").write_bytes(b"signature")
            (fixture / "RELEASE").write_text("0.2.0\n", encoding="utf-8")
            upgrade = fixture / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o755)

            inspector = shared / "jooan-ironman-inspect"
            inspector.write_text(
                "#!/bin/sh\n"
                "size=$(wc -c < \"$1\")\n"
                "sha=$(sha256sum \"$1\" | awk '{print $1}')\n"
                "echo payload_offset=96\n"
                "echo payload_length=$((size-192))\n"
                "echo package_length=$size\n"
                "echo model_token=A12\n"
                "echo package_sha256=$sha\n",
                encoding="utf-8",
            )
            authenticator = shared / "jooan-auth-verify"
            authenticator.write_text("#!/bin/sh\nexit ${FAKE_AUTH_RC:-0}\n", encoding="utf-8")
            os.chmod(inspector, 0o755)
            os.chmod(authenticator, 0o755)

            log = root / "mount.log"
            (fake_bin / "mount").write_text(
                "#!/bin/sh\n"
                "echo mount >> \"$FAKE_MOUNT_LOG\"\n"
                "cp -R \"$FAKE_MOUNT_SOURCE/.\" \"$6/\"\n",
                encoding="utf-8",
            )
            (fake_bin / "umount").write_text(
                "#!/bin/sh\n"
                "echo umount >> \"$FAKE_MOUNT_LOG\"\n"
                "rm -rf \"$1\"/*\n"
                "exit 0\n",
                encoding="utf-8",
            )
            (fake_bin / "reboot").write_text(
                "#!/bin/sh\necho reboot >> \"$FAKE_MOUNT_LOG\"\nexit 0\n",
                encoding="utf-8",
            )
            for command in ("mount", "umount", "reboot"):
                os.chmod(fake_bin / command, 0o755)
            (slot / "hooks/onvif-ptz.sh").write_text(
                '#!/bin/sh\nprintf "%s\\n" "$*" >> "$FAKE_MOUNT_LOG"\n',
                encoding="utf-8",
            )
            os.chmod(slot / "hooks/onvif-ptz.sh", 0o755)

            helper = REPOSITORY / "runtime/slot/hooks/integration-helper.sh"
            environment = os.environ | {
                "JL_ROOT": str(persistent),
                "JL_RUN": str(run),
                "JL_CONTROL": str(control),
                "JL_SLOT_DIR": str(slot),
                "JOOAN_PATH": f"{fake_bin}:/bin:/usr/bin:/sbin:/usr/sbin",
                "FAKE_MOUNT_SOURCE": str(fixture),
                "FAKE_MOUNT_LOG": str(log),
            }
            routes = root / "routes.list"
            routes.write_text("10.42.0.0/24\nfd00::/64\n", encoding="utf-8")
            routes_set = subprocess.run(
                [str(helper), "routes-set", str(routes), "route-id"],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(routes_set.returncode, 0, routes_set.stderr)
            self.assertEqual(
                json.loads(routes_set.stdout),
                {"cidrs": ["10.42.0.0/24", "fd00::/64"]},
            )
            routes.write_text("0.0.0.0/0\n", encoding="utf-8")
            self.assertNotEqual(subprocess.run(
                [str(helper), "routes-set", str(routes), "route-id"],
                env=environment,
                capture_output=True,
            ).returncode, 0)
            ptz = root / "ptz.json"
            ptz.write_text(
                '{"command":"left","duration_ms":250,"speed":3}\n',
                encoding="utf-8",
            )
            jog = subprocess.run(
                [str(helper), "ptz-jog", str(ptz), "ptz-id"],
                env=environment,
                capture_output=True,
            )
            self.assertEqual(jog.returncode, 0, jog.stderr)
            self.assertIn("move left 3 250", log.read_text())
            ptz.write_text(
                '{"command":"left;reboot","duration_ms":250,"speed":3}\n',
                encoding="utf-8",
            )
            self.assertNotEqual(subprocess.run(
                [str(helper), "ptz-jog", str(ptz), "ptz-id"],
                env=environment,
                capture_output=True,
            ).returncode, 0)
            verify = subprocess.run(
                [str(helper), "firmware-verify", str(package), package_id],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(verify.returncode, 0, verify.stderr)
            self.assertTrue((run / f"firmware.{package_id}.verified").is_file())
            self.assertIn("umount", log.read_text())

            apply = subprocess.run(
                [str(helper), "firmware-apply", "-", package_id],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertEqual(apply.returncode, 0, apply.stderr)
            for _ in range(30):
                status = run / f"firmware.{package_id}.status"
                if status.is_file() and "state=complete" in status.read_text():
                    break
                import time
                time.sleep(0.05)
            else:
                self.fail("direct apply did not record completion")
            self.assertIn("reboot", log.read_text())

            replay_state = persistent / "state/release-sequence"
            replay_state.write_text("2\n", encoding="utf-8")
            replay = subprocess.run(
                [str(helper), "firmware-verify", str(package), package_id],
                env=environment,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(replay.returncode, 0)

            failed_environment = environment | {"FAKE_AUTH_RC": "1"}
            failed = subprocess.run(
                [str(helper), "firmware-verify", str(package), package_id],
                env=failed_environment,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(failed.returncode, 0)
            self.assertGreaterEqual(log.read_text().count("umount"), 2)


class HostBuilderTests(unittest.TestCase):
    def test_target_manifest_is_exact_ja_a12(self) -> None:
        target = json.loads(
            (REPOSITORY / "packaging" / "targets" / "ja-a12.json").read_text()
        )
        self.assertEqual(target["device"]["model"], "JA-A12")
        self.assertEqual(target["ironman"]["model_token"], "A12")
        self.assertEqual(
            [sensor["driver"] for sensor in target["device"]["sensors"]],
            ["cv2005", "cv2005s1"],
        )
        self.assertEqual(target["device"]["radio"]["driver_family"], "skw6316")
        self.assertEqual(
            target["persistent_contract"]["logical_regular_file_cap_bytes"],
            176 * 1024,
        )
        self.assertEqual(
            target["persistent_contract"]["final_free_reserve_bytes"],
            80 * 1024,
        )
        self.assertEqual(
            target["persistent_contract"]["maintenance_regular_file_cap_bytes"],
            176 * 1024,
        )
        self.assertGreaterEqual(
            target["persistent_contract"]["maintenance_final_free_reserve_bytes"],
            56 * 1024,
        )
        for soname in ("libmbedcrypto.so.6", "libmbedtls.so.13", "libmbedx509.so.1"):
            self.assertIn(f"/lib/{soname}", target["compatibility_hashes"])

    def test_stage_web_filter_ships_ui_minified_without_pwa(self) -> None:
        assembly = (REPOSITORY / "packaging/assemble-stages.sh").read_text()
        for pattern in ("*.html", "*.css", "*.js", "*.svg", "*.webmanifest"):
            self.assertIn(pattern, assembly)
        self.assertIn("! -name '*.test.js'", assembly)
        # The persistent budget has no room for readable source, so staged
        # assets are stripped the same way the shell scripts are.
        self.assertIn("minify-web.py", assembly)
        deployable = {
            path.name for path in (REPOSITORY / "web").iterdir()
            if path.is_file()
            and path.suffix in (".html", ".css", ".js", ".svg", ".webmanifest")
            and not path.name.endswith(".test.js")
        }
        self.assertIn("app.js", deployable)
        self.assertNotIn("audio-codec.test.js", deployable)
        # The PWA shell was retired to buy that space. A service worker also
        # cached index.html and app.js, which would have kept serving the
        # pre-update UI after a firmware upgrade.
        self.assertNotIn("manifest.webmanifest", deployable)
        self.assertNotIn("icon.svg", deployable)
        self.assertNotIn("sw.js", deployable)
        app = (REPOSITORY / "web/app.js").read_text()
        self.assertIn("getRegistrations", app)

    def test_expanded_product_migration_orders_durable_replacement_first(self) -> None:
        installer = (REPOSITORY / "packaging/payload/install-upgrade.sh").read_text()
        publish = installer.index('mv -f "$root/controller.tar.gz.new"')
        activate = installer.index('mv -f "$activate.new" "$activate"')
        retire = installer.index('rm -rf "$root/boot"', activate)
        runtime = installer.index("stage_trial_runtime ||", retire)
        self.assertLess(publish, activate)
        self.assertLess(activate, retire)
        self.assertLess(retire, runtime)
        self.assertIn("JOOAN_FAIL_AFTER_STATE", installer)
        target = json.loads(
            (REPOSITORY / "packaging/targets/ja-a12.json").read_text()
        )
        self.assertIn(
            "expanded-product-0.1-validated",
            target["migration_contract"]["states"],
        )

    def test_controller_owned_ssh_survives_no_runtime_fallback(self) -> None:
        boot = (REPOSITORY / "runtime/boot/boot.sh").read_text()
        ssh = (REPOSITORY / "runtime/admin/ssh-start.sh").read_text()
        installer = (REPOSITORY / "packaging/payload/install-upgrade.sh").read_text()
        assembly = (REPOSITORY / "packaging/assemble-stages.sh").read_text()
        self.assertIn('"$JL_CONTROL/admin/ssh-start.sh" "$jl_running"', boot)
        self.assertNotIn('"$jl_running" != - ] && [ -x "$JL_CONTROL/admin/ssh-start.sh"', boot)
        self.assertIn("case \"$1\" in A|B|-)", ssh)
        self.assertIn("$JL_CONTROL/shared/entropy-ready.sh", ssh)
        self.assertIn("grep -qx 'admin:!'", ssh)
        self.assertIn('[ ! -s "$jl_keys/authorized_keys" ]', ssh)
        self.assertIn("admin:$1$joorec01$", installer)
        self.assertIn("entropy-ready.sh", assembly)

    def test_uninstall_does_not_delete_its_direct_apply_mount(self) -> None:
        uninstall = (REPOSITORY / "packaging/payload/uninstall-upgrade.sh").read_text()
        self.assertIn("Do not delete $run here", uninstall)
        self.assertNotIn('rm -rf "$root" /opt/etc/jooan-ssh "$run"', uninstall)

    def test_camera_archive_checks_use_supported_busybox_tar(self) -> None:
        paths = (
            "packaging/payload/install-upgrade.sh",
            "runtime/admin/install-controller.sh",
            "runtime/boot/local.rc",
        )
        for relative in paths:
            source = (REPOSITORY / relative).read_text()
            self.assertNotIn("gzip -t", source, relative)
            self.assertIn("tar -tzf", source, relative)

    def test_health_promotion_stays_committed_when_prune_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            fixture = Path(temporary_dir)
            root = fixture / "root"
            run = fixture / "run"
            state = root / "state"
            slots = root / "slots"
            health_dir = run / "slot-B"
            for directory in (state, slots / "A", slots / "B", health_dir):
                directory.mkdir(parents=True, exist_ok=True)
            (state / "selection").write_text("A B 1\n", encoding="utf-8")
            (run / "running").write_text("B\n", encoding="utf-8")
            health = health_dir / "health.sh"
            health.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(health, 0o755)
            os.chmod(slots, 0o500)
            try:
                result = subprocess.run(
                    [str(REPOSITORY / "runtime/admin/mark-healthy.sh"), "B"],
                    env=os.environ | {
                        "JL_ROOT": str(root),
                        "JL_RUN": str(run),
                        "JL_CONTROL": str(REPOSITORY / "runtime"),
                    },
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((state / "selection").read_text(), "B - 0\n")
                self.assertTrue((slots / "B").is_dir())
            finally:
                os.chmod(slots, 0o700)

    def test_expanded_product_migration_resumes_after_fault(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            sandbox = Path(temporary_dir)
            stage = sandbox / "stage"
            persistent = sandbox / "persistent"
            run = sandbox / "run"
            compatible = sandbox / "compatible"
            fake_bin = sandbox / "bin"
            controller = sandbox / "controller"
            for directory in (
                stage,
                persistent / "boot",
                persistent / "admin",
                persistent / "shared",
                persistent / "slots/A",
                persistent / "slots/B",
                persistent / "state",
                persistent / "config/ssh",
                compatible,
                fake_bin,
                controller / "boot",
                controller / "admin",
            ):
                directory.mkdir(parents=True, exist_ok=True)
            (controller / "boot/common.sh").write_text(
                "jl_check_storage() { return 0; }\n"
                "jl_check_maintenance_storage() { return 0; }\n"
                "jl_check_current_storage() { return 0; }\n"
                "jl_wait_free_kb() {\n"
                "  free=${FAKE_FREE_KB:-124}\n"
                "  [ -f \"$JL_ROOT/shared/dropbear.tar.gz\" ] || "
                "free=$((free + ${FAKE_DROPBEAR_KB:-59}))\n"
                "  if [ -f \"$JL_ROOT/controller.tar.gz\" ] && "
                "[ \"$(wc -c < \"$JL_ROOT/controller.tar.gz\")\" -lt 50000 ]; then "
                "free=$((free + ${FAKE_OLD_CORE_KB:-0})); fi\n"
                "  [ \"$free\" -ge \"$2\" ]\n"
                "}\n",
                encoding="utf-8",
            )
            (controller / "admin/install-runtime.sh").write_text(
                "#!/bin/sh\n"
                "stage=$1\n"
                "mkdir -p \"$JL_ROOT/slots/A\" \"$JL_ROOT/state\"\n"
                "rm -rf \"$JL_ROOT/slots/B\"\n"
                "cp \"$stage/runtime.tar.gz\" \"$stage/runtime.md5\" "
                "\"$JL_ROOT/slots/A/\"\n"
                "printf '%s\\n' '- A 0' > \"$JL_ROOT/state/selection\"\n",
                encoding="utf-8",
            )
            (controller / "admin/install-controller.sh").write_text(
                "#!/bin/sh\n"
                "[ \"$1\" = activate ] || exit 1\n"
                "cp \"$JL_ROOT/local.rc\" \"$JL_ACTIVATE\"\n",
                encoding="utf-8",
            )
            os.chmod(controller / "admin/install-runtime.sh", 0o755)
            os.chmod(controller / "admin/install-controller.sh", 0o755)
            pseudo_random = b"".join(
                hashlib.sha256(f"controller-pad-{counter}".encode()).digest()
                for counter in range(1000)
            )[:30000]
            (controller / "padding.bin").write_bytes(pseudo_random)
            controller_archive = stage / "controller.tar.gz"
            subprocess.run(
                [
                    "tar", "--sort=name", "--mtime=@0", "--owner=0", "--group=0",
                    "--numeric-owner", "-C", str(controller), "-czf",
                    str(controller_archive), ".",
                ],
                check=True,
            )
            self.assertGreater(controller_archive.stat().st_size, 25 * 1024)
            self.assertLess(controller_archive.stat().st_size, 35 * 1024)
            recovery_tree = sandbox / "recovery-tree"
            (recovery_tree / "bin").mkdir(parents=True)
            recovery_random = b"".join(
                hashlib.sha256(f"recovery-pad-{counter}".encode()).digest()
                for counter in range(2000)
            )[:59000]
            (recovery_tree / "bin/dropbear").write_bytes(recovery_random)
            subprocess.run(
                [
                    "tar", "--sort=name", "--mtime=@0", "--owner=0", "--group=0",
                    "--numeric-owner", "-C", str(recovery_tree), "-czf",
                    str(stage / "recovery.tar.gz"), ".",
                ],
                check=True,
            )
            (stage / "recovery.md5").write_text(
                legacy_md5((stage / "recovery.tar.gz").read_bytes()).decode() + "\n"
            )
            self.assertGreater((stage / "recovery.tar.gz").stat().st_size, 55 * 1024)
            self.assertLess((stage / "recovery.tar.gz").stat().st_size, 65 * 1024)
            new_runtime = stage / "runtime.tar.gz"
            new_runtime.write_bytes(b"new-runtime")
            (stage / "runtime.md5").write_text(
                legacy_md5(new_runtime.read_bytes()).decode() + "\n"
            )
            (stage / "local.rc").write_text(
                "#!/bin/sh\nJL_ROOT=/opt/custom/jooan-local\n", encoding="utf-8"
            )
            (stage / "RELEASE").write_text("0.2.0\n", encoding="utf-8")
            (stage / "release.manifest").write_text(
                "\n".join((
                    "JOOAN-SIGNED-RELEASE-V1",
                    "target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316",
                    "device_model=JA-A12",
                    "model_token=A12",
                    "release_version=0.2.0",
                    "release_sequence=2",
                    "minimum_sequence=1",
                    "artifact_kind=install",
                    "files-begin",
                    "0" * 64 + "  RELEASE",
                    "files-end",
                    "",
                )),
                encoding="utf-8",
            )
            (stage / "release.manifest.sig").write_bytes(b"signature")
            (stage / "persistent.contract").write_text(
                "JOOAN-PERSISTENT-CONTRACT-V1\n"
                "logical_regular_file_cap_bytes=180224\n"
                "final_free_reserve_bytes=81920\n"
                "state_config_regular_file_reserve_bytes=12288\n"
                "external_regular_file_reserve_bytes=4096\n"
                "maintenance_regular_file_cap_bytes=180224\n"
                "maintenance_final_free_reserve_bytes=57344\n",
                encoding="utf-8",
            )
            (stage / "migration.contract").write_text(
                "JOOAN-MIGRATION-CONTRACT-V1\n"
                f"expanded_sha256={hashlib.sha256(b'dropbear').hexdigest()}  shared/dropbear.tar.gz\n"
                f"expanded_sha256={hashlib.sha256(b'replaceable-sha').hexdigest()}  shared/jooan-sha256\n"
                f"expanded_sha256={hashlib.sha256(b'guard').hexdigest()}  shared/libjooan_guard.so\n"
                f"expanded_sha256={hashlib.sha256(b'old-stable-runtime').hexdigest()}  slots/runtime.tar.gz\n"
                "state=legacy-retired\n",
                encoding="utf-8",
            )
            component = compatible / "component"
            component.write_bytes(b"compatible")
            (stage / "compatibility.sha256").write_text(
                hashlib.sha256(component.read_bytes()).hexdigest() + "  /component\n"
            )
            sha = stage / "jooan-sha256"
            sha.write_text("#!/bin/sh\nexec sha256sum \"$@\"\n", encoding="utf-8")
            auth = stage / "jooan-auth-verify"
            auth.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            upgrade = stage / "upgrade.sh"
            upgrade.write_bytes(
                (REPOSITORY / "packaging/payload/install-upgrade.sh").read_bytes()
            )
            for executable in (sha, auth, upgrade, stage / "local.rc"):
                os.chmod(executable, 0o755)

            old_runtime = persistent / "slots/A/runtime.tar.gz"
            old_runtime.write_bytes(b"old-stable-runtime")
            (persistent / "slots/A/runtime.sha256").write_text(
                hashlib.sha256(old_runtime.read_bytes()).hexdigest() + "\n"
            )
            (persistent / "slots/B/runtime.tar.gz").write_bytes(b"old-inactive")
            (persistent / "state/selection").write_text("A - 0\n")
            (persistent / "state/controller.ready").write_text("1\n")
            (persistent / "state/prelocal-hook.disabled").write_text("unsafe\n")
            (persistent / "config/auth.db").write_text(
                "v1:120000:00112233445566778899aabbccddeeff:"
                + "11" * 32
                + ":0\n",
                encoding="utf-8",
            )
            (persistent / "config/ssh/authorized_keys").write_text(
                "ssh-ed25519 AAAATEST recovery\n", encoding="utf-8"
            )
            for relative in (
                "boot/common.sh", "boot/boot.sh", "boot/local.rc",
                "admin/install-controller.sh", "admin/install-runtime.sh",
            ):
                path = persistent / relative
                path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            for name, content in (
                ("libjooan_guard.so", b"guard"),
                ("dropbear.tar.gz", b"dropbear"),
            ):
                path = persistent / "shared" / name
                path.write_bytes(content)
                side = "guard" if name.startswith("lib") else "dropbear"
                (persistent / "shared" / f"{side}.sha256").write_text(
                    hashlib.sha256(content).hexdigest() + "\n"
                )
            (persistent / "shared/jooan-sha256").write_bytes(b"replaceable-sha")
            model = sandbox / "deviceModel"
            model.write_text("JA-A12\n")
            activate = sandbox / "local.rc"
            activate.write_text("#!/bin/sh\nold\n")
            legacy = sandbox / "legacy-open"
            (fake_bin / "killall").write_text("#!/bin/sh\nexit 0\n")
            (fake_bin / "id").write_text("#!/bin/sh\necho 0\n")
            os.chmod(fake_bin / "killall", 0o755)
            os.chmod(fake_bin / "id", 0o755)
            environment = os.environ | {
                "JOOAN_ROOT": str(persistent),
                "JOOAN_RUN": str(run),
                "JOOAN_ACTIVATE": str(activate),
                "JOOAN_LEGACY_ROOT": str(legacy),
                "JOOAN_DEVICE_MODEL_PATH": str(model),
                "JOOAN_COMPAT_ROOT": str(compatible),
                "JOOAN_PATH": f"{fake_bin}:/bin:/usr/bin:/sbin:/usr/sbin",
                "FAKE_FREE_KB": "124",
                "FAKE_DROPBEAR_KB": "59",
            }
            run.mkdir(parents=True, exist_ok=True)
            (run / "running").write_text("A\n", encoding="utf-8")
            fault_points = (
                ("JOOAN_FAIL_AFTER_STATE", "expanded-product-0.1-validated"),
                ("JOOAN_FAIL_AFTER_STATE", "keys-preserved"),
                ("JOOAN_FAIL_AFTER_STATE", "headroom-reclaiming"),
                ("JOOAN_FAIL_AT", "after-reclaim-inactive"),
                ("JOOAN_FAIL_AT", "after-reclaim-prelocal"),
                ("JOOAN_FAIL_AT", "after-reclaim-sha"),
                ("JOOAN_FAIL_AT", "after-reclaim-guard"),
                ("JOOAN_FAIL_AT", "after-reclaim-dropbear"),
                ("JOOAN_FAIL_AT", "after-reclaim-dropbear-sidecar"),
                ("JOOAN_FAIL_AFTER_STATE", "headroom-reclaimed"),
                ("LOW_FREE", "controller-copy-preflight"),
                ("JOOAN_FAIL_AT", "after-controller-copy"),
                ("JOOAN_FAIL_AT", "after-controller-rename"),
                ("JOOAN_FAIL_AFTER_STATE", "controller-published"),
                ("JOOAN_FAIL_AT", "after-recovery-copy"),
                ("JOOAN_FAIL_AT", "after-recovery-old-rename"),
                ("JOOAN_FAIL_AT", "after-recovery-rename"),
                ("JOOAN_FAIL_AT", "after-hook-rename"),
                ("JOOAN_FAIL_AFTER_STATE", "failclosed-hook-published"),
                ("JOOAN_FAIL_AFTER_STATE", "activated"),
                ("JOOAN_FAIL_AT", "after-retire-boot"),
                ("JOOAN_FAIL_AT", "after-retire-admin"),
                ("JOOAN_FAIL_AT", "after-retire-shared"),
                ("JOOAN_FAIL_AFTER_STATE", "expanded-controller-retired"),
                ("JOOAN_FAIL_AT", "after-recovery-selection"),
                ("JOOAN_FAIL_AT", "after-runtime-delete"),
                ("JOOAN_FAIL_AT", "after-runtime-copy"),
                ("JOOAN_FAIL_AT", "after-runtime-dir-rename"),
                ("JOOAN_FAIL_AT", "after-pending-selection-rename"),
                ("JOOAN_FAIL_AFTER_STATE", "runtime-staged"),
                ("JOOAN_FAIL_AFTER_STATE", "legacy-retired"),
                ("JOOAN_FAIL_AFTER_STATE", "release-sequence-published"),
            )
            for variable, point in fault_points:
                if variable == "LOW_FREE":
                    fault = subprocess.run(
                        [str(upgrade)],
                        env=environment | {"FAKE_FREE_KB": "20"},
                        capture_output=True,
                    )
                    self.assertNotEqual(fault.returncode, 0, point)
                    self.assertFalse((persistent / "controller.tar.gz.new").exists())
                    continue
                fault = subprocess.run(
                    [str(upgrade)],
                    env=environment | {variable: point},
                    capture_output=True,
                )
                self.assertNotEqual(fault.returncode, 0, point)
                if point == "headroom-reclaimed":
                    self.assertFalse(
                        (persistent / "shared/dropbear.tar.gz").exists()
                    )
                if point == "after-controller-copy":
                    self.assertTrue((persistent / "controller.tar.gz.new").is_file())
            resumed = subprocess.run(
                [str(upgrade)], env=environment, text=True, capture_output=True
            )
            self.assertEqual(
                resumed.returncode,
                0,
                resumed.stderr
                + f" state={(persistent / 'state/migration-state').read_text()!r}"
                + f" selection={(persistent / 'state/selection').read_text()!r}",
            )
            self.assertFalse((persistent / "boot").exists())
            self.assertTrue((persistent / "controller.tar.gz").is_file())
            self.assertFalse((persistent / "slots/A").exists())
            self.assertTrue((persistent / "slots/B/runtime.tar.gz").is_file())
            self.assertFalse((persistent / "recovery.old").exists())
            self.assertFalse((persistent / "recovery.new").exists())
            self.assertEqual((persistent / "state/selection").read_text(), "- B 0\n")
            self.assertIn("/opt/custom/jooan-local", activate.read_text())
            self.assertEqual((persistent / "state/release-sequence").read_text(), "2\n")
            self.assertEqual(
                (persistent / "config/ssh/passwd").read_text(), "admin:!\n"
            )

            # Exact observed partial state: expanded files already retired,
            # old 90 KiB controller + old A runtime, 108 KiB free, no recovery.
            partial = sandbox / "partial-root"
            partial_run = sandbox / "partial-run"
            partial_activate = sandbox / "partial-local.rc"
            for directory in (
                partial / "state", partial / "config/ssh", partial / "slots/A"
            ):
                directory.mkdir(parents=True, exist_ok=True)
            old_controller_tree = sandbox / "old-controller-tree"
            old_controller_tree.mkdir()
            old_controller_random = b"".join(
                hashlib.sha256(f"old-controller-{counter}".encode()).digest()
                for counter in range(3000)
            )[:90000]
            (old_controller_tree / "old.bin").write_bytes(old_controller_random)
            subprocess.run(
                [
                    "tar", "--sort=name", "--mtime=@0", "--owner=0", "--group=0",
                    "--numeric-owner", "-C", str(old_controller_tree), "-czf",
                    str(partial / "controller.tar.gz"), ".",
                ],
                check=True,
            )
            self.assertGreater((partial / "controller.tar.gz").stat().st_size, 85 * 1024)
            (partial / "local.rc").write_text(
                "#!/bin/sh\nJL_ROOT=/opt/custom/jooan-local\n", encoding="utf-8"
            )
            partial_activate.write_text(
                "#!/bin/sh\nJL_ROOT=/opt/custom/jooan-local\n", encoding="utf-8"
            )
            (partial / "state/migration-state").write_text(
                "expanded-controller-retired\n", encoding="utf-8"
            )
            (partial / "state/selection").write_text("A - 0\n", encoding="utf-8")
            (partial / "state/controller.ready").write_text("1\n", encoding="utf-8")
            (partial / "config/auth.db").write_text("customized\n", encoding="utf-8")
            (partial / "config/ssh/authorized_keys").write_text(
                "ssh-ed25519 AAAATEST recovery\n", encoding="utf-8"
            )
            (partial / "config/ssh/passwd").write_text("admin:!\n", encoding="utf-8")
            (partial / "slots/A/runtime.tar.gz").write_bytes(b"old-stable-runtime")
            (partial / "slots/A/runtime.md5").write_text(
                legacy_md5(b"old-stable-runtime").decode() + "\n", encoding="utf-8"
            )
            partial_environment = environment | {
                "JOOAN_ROOT": str(partial),
                "JOOAN_RUN": str(partial_run),
                "JOOAN_ACTIVATE": str(partial_activate),
                "FAKE_FREE_KB": "108",
                "FAKE_DROPBEAR_KB": "0",
                "FAKE_OLD_CORE_KB": "60",
            }
            partial_run.mkdir(parents=True, exist_ok=True)
            (partial_run / "running").write_text("A\n", encoding="utf-8")
            partial_result = subprocess.run(
                [str(upgrade)], env=partial_environment, text=True, capture_output=True
            )
            self.assertEqual(partial_result.returncode, 0, partial_result.stderr)
            self.assertLess((partial / "controller.tar.gz").stat().st_size, 35 * 1024)
            self.assertTrue((partial / "recovery/dropbear.tar.gz").is_file())
            self.assertFalse((partial / "recovery.old").exists())
            self.assertFalse((partial / "recovery.new").exists())
            self.assertFalse((partial / "slots/A").exists())
            self.assertTrue((partial / "slots/B/runtime.tar.gz").is_file())
            self.assertEqual((partial / "state/selection").read_text(), "- B 0\n")

            # Completed seq2 must treat legacy-retired as historical, replace
            # stable B with different seq3 bytes, and publish sequence last.
            (partial / "state/selection").write_text("B - 0\n", encoding="utf-8")
            (partial / "state/migration-state").write_text(
                "legacy-retired\n", encoding="utf-8"
            )
            (partial / "state/release-sequence").write_text("2\n", encoding="utf-8")
            (partial_run / "running").write_text("B\n", encoding="utf-8")
            seq3_runtime = b"different-sequence-three-runtime"
            (stage / "runtime.tar.gz").write_bytes(seq3_runtime)
            (stage / "runtime.md5").write_text(
                legacy_md5(seq3_runtime).decode() + "\n", encoding="utf-8"
            )
            (stage / "RELEASE").write_text("0.3.0\n", encoding="utf-8")
            (stage / "release.manifest").write_text(
                "\n".join((
                    "JOOAN-SIGNED-RELEASE-V1",
                    "target_id=jooan-ja-a12-t23n-dual-cv2005-skw6316",
                    "device_model=JA-A12",
                    "model_token=A12",
                    "release_version=0.3.0",
                    "release_sequence=3",
                    "minimum_sequence=1",
                    "artifact_kind=install",
                    "files-begin",
                    "0" * 64 + "  RELEASE",
                    "files-end",
                    "",
                )),
                encoding="utf-8",
            )
            seq3_result = subprocess.run(
                [str(upgrade)], env=partial_environment, text=True, capture_output=True
            )
            self.assertEqual(seq3_result.returncode, 0, seq3_result.stderr)
            self.assertFalse((partial / "slots/B").exists())
            self.assertEqual(
                (partial / "slots/A/runtime.tar.gz").read_bytes(), seq3_runtime
            )
            self.assertEqual((partial / "state/selection").read_text(), "- A 0\n")
            self.assertEqual((partial / "state/release-sequence").read_text(), "3\n")

            # Boot marks attempted before health; promotion makes A durable and
            # a later cleanup failure must never restore deleted B.
            (partial / "state/selection").write_text("- A 1\n", encoding="utf-8")
            (partial_run / "running").write_text("A\n", encoding="utf-8")
            health_dir = partial_run / "slot-A"
            health_dir.mkdir(parents=True, exist_ok=True)
            health = health_dir / "health.sh"
            health.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(health, 0o755)
            promoted = subprocess.run(
                [str(REPOSITORY / "runtime/admin/mark-healthy.sh"), "A"],
                env=os.environ | {
                    "JL_ROOT": str(partial),
                    "JL_RUN": str(partial_run),
                    "JL_CONTROL": str(REPOSITORY / "runtime"),
                },
                text=True,
                capture_output=True,
            )
            self.assertEqual(promoted.returncode, 0, promoted.stderr)
            self.assertEqual((partial / "state/selection").read_text(), "A - 0\n")
            self.assertTrue((partial / "slots/A/runtime.tar.gz").is_file())

    def test_runtime_installer_resumes_matching_pending_archive(self) -> None:
        source = (REPOSITORY / "runtime/admin/install-runtime.sh").read_text()
        pending = source.index('if [ "$JL_PENDING" != - ]')
        destructive = source.index('jl_write_selection - - 0')
        self.assertLess(pending, destructive)
        self.assertIn("pending runtime $JL_PENDING already matches authenticated stage", source)
        self.assertIn('"$JOOAN_SHA256" "$jl_existing/runtime.tar.gz"', source)
        for point in (
            "after-recovery-selection",
            "after-runtime-delete",
            "after-runtime-dir-rename",
            "after-pending-selection-rename",
        ):
            self.assertIn(point, source)

    def test_stage_requires_executable_upgrade_script(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            stage = Path(temporary_dir)
            with self.assertRaises(ValueError):
                validate_stage(stage)
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o644)
            with self.assertRaises(ValueError):
                validate_stage(stage)
            os.chmod(upgrade, 0o755)
            validate_stage(stage)

    @unittest.skipUnless(shutil.which("mksquashfs"), "mksquashfs is not installed")
    def test_builder_rejects_output_inside_stage(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            stage = Path(temporary_dir)
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o755)
            with self.assertRaisesRegex(ValueError, "must not be inside"):
                build_package(
                    stage,
                    stage / "JOOAN_FW_PKG",
                    model_token="A12",
                    firmware_version="05.02.31.115",
                )

    @unittest.skipUnless(shutil.which("mksquashfs"), "mksquashfs is not installed")
    def test_full_builder_is_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            stage = root / "stage"
            stage.mkdir()
            upgrade = stage / "upgrade.sh"
            upgrade.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            os.chmod(upgrade, 0o755)
            (stage / "public.txt").write_text("local firmware\n", encoding="utf-8")

            first = root / "first.pkg"
            second = root / "second.pkg"
            first_info, _ = build_package(
                stage,
                first,
                model_token="A12",
                firmware_version="05.02.31.115",
            )
            second_info, _ = build_package(
                stage,
                second,
                model_token="A12",
                firmware_version="05.02.31.115",
            )
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(first_info, second_info)
            self.assertEqual(
                (root / "first.pkg.sha256").read_text().split()[0],
                first_info.package_sha256,
            )

    @unittest.skipUnless(shutil.which("mksquashfs"), "mksquashfs is not installed")
    def test_release_entrypoint_builds_install_and_uninstall(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_dir:
            root = Path(temporary_dir)
            key, public = temporary_release_key(root)
            target = temporary_target(root, public)
            signer = load_stage_signer()
            stages = []
            for name in ("install", "uninstall"):
                stage = root / name
                stage.mkdir()
                upgrade = stage / "upgrade.sh"
                upgrade.write_text(f"#!/bin/sh\n# {name}\nexit 0\n", encoding="utf-8")
                os.chmod(upgrade, 0o755)
                (stage / "RELEASE").write_text("test-1\n", encoding="utf-8")
                if name == "install":
                    (stage / "build-provenance.json").write_text(
                        '{"schema_version":1,"source_commit":"test"}\n',
                        encoding="utf-8",
                    )
                signer.sign_stage(
                    stage,
                    target,
                    kind=name,
                    release_version="test-1",
                    release_sequence=1,
                    signing_key=key,
                )
                stages.append(stage)
            output = root / "dist"
            subprocess.run(
                [
                    str(REPOSITORY / "build.sh"),
                    "--release-version",
                    "test-1",
                    "--install-stage",
                    str(stages[0]),
                    "--uninstall-stage",
                    str(stages[1]),
                    "--out-dir",
                    str(output),
                    "--target",
                    str(target),
                    "--signing-key",
                    str(key),
                ],
                cwd=REPOSITORY,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["release_version"], "test-1")
            self.assertTrue(manifest["release_ready"])
            self.assertEqual(set(manifest["packages"]), {"install", "uninstall"})
            self.assertEqual(manifest["target"]["device"]["model"], "JA-A12")
            for filename in ("JOOAN_FW_PKG", "JOOAN_UNINSTALL"):
                package = output / filename
                self.assertTrue(package.is_file())
                inspect_package(package.read_bytes(), expected_model="A12")
            verify_bytes(
                (output / "manifest.json").read_bytes(),
                (output / "manifest.json.sig").read_bytes(),
                public,
            )


if __name__ == "__main__":
    unittest.main()
