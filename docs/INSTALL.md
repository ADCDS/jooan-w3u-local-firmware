# Installation

Installation modifies only the writable `/opt` configuration partition; it
does not rewrite the kernel, rootfs, appfs, bootloader, calibration or identity.
Preserve a flash backup and read [Recovery](RECOVERY.md) before continuing.
Use a tagged hardware-qualified release for ordinary installations; `0.1.0`
remains an engineering image until its live acceptance record is published.

## 1. Isolate the camera

Create a dedicated router/VLAN segment with no Internet route and no access
from untrusted LAN clients. Permit only the installation workstation. Keep the
policy active from power-on: the OEM GoAhead service is known to become
reachable during early boot before `/opt/etc/local.rc` can terminate it. The
router remains the controlling boundary throughout that interval.

Do not install over a port-forward, public Wi-Fi network, or ordinary shared
home LAN.

## 2. Verify exact hardware

Confirm every item in [Compatibility](COMPATIBILITY.md), including PCB
silkscreen, both sensor identities, SKW6316 radio, T23N SoC, and 8 MiB flash.
The supported package model token is `A12`.

Stop if the camera instead contains SC2336P/ATBM6132U hardware. That is the
different revision supported by the public Thingino W3-U image.

## 3. Back up and prepare recovery

Preserve and hash a complete SPI NOR backup. The installer also preserves a
failed-runtime fallback to the OEM updater. External-programmer restore remains
the strongest recovery path but is not a prerequisite imposed by this
`/opt`-only package.

Do not publish the dump. It may contain Wi-Fi credentials, identifiers, private
device data, and vendor code that this project cannot redistribute.

## 4. Build the packages

Build the reviewed install and uninstall packages:

```sh
TOOLCHAIN_ROOT=/path/to/mips-gcc540-glibc222-64bit-r3.3.0 \
OEM_ROOTFS=/private/path/to/extracted-rootfs \
./release.sh
```

Maintainers may use `build.sh` to rewrap already-audited stage directories.

To rebuild only the uninstall package:

```sh
./build-uninstall.sh \
  --release-version 1.0.0 \
  --stage /path/to/uninstall-stage
```

Both commands accept `--out-dir` (default `dist`) and
`--firmware-version` (default `05.02.31.115`). Packaging defaults to xz with a
128 KiB dictionary. Run either command with `--help` for the checked-out
version's complete interface.

The default outputs are:

```text
dist/JOOAN_FW_PKG
dist/JOOAN_FW_PKG.sha256
dist/JOOAN_UNINSTALL
dist/JOOAN_UNINSTALL.sha256
dist/manifest.json
```

Review `manifest.json` and independently verify the SHA-256 files before
delivery. A successful package build proves format integrity, not that an
arbitrary `upgrade.sh` is safe.

## 5. Deliver a qualified release on the isolated network

Use only the release's documented IronMan uploader or offline SD transport.
The OEM update transport is an installation carrier, not a secure remote update
service. Never upload a package across the Internet or an untrusted LAN.

Before confirming installation, verify:

- the live device reports `JA-A12` and the expected firmware/kernel family;
- the package model token is `A12`;
- free persistent space and MTD layout match the target manifest;
- the install package and its SHA-256 match the reviewed build;
- the uninstall package and external flash backup are immediately available;
- power will remain stable for the full install and first reboot.

Do not interrupt power while persistent data is being updated.

## 6. First boot and enrollment

Keep the camera isolated. Browse to its HTTPS address and verify the per-device
certificate fingerprint over the trusted installation channel. The initial
administrator is:

```text
username: admin
password: change-me-now
```

Change this password immediately. Setup must not be considered complete until
the default credential is rejected. Enroll an SSH public key if shell access is
needed; SSH remains key-only and should not listen until a key exists.

The generic package contains no Wi-Fi credentials. Existing compatible OEM
Wi-Fi settings are preserved. If they must change, use the manual Wi-Fi setup
over the isolated management path and verify connectivity before removing the
old path.

## 7. Acceptance checks

After a cold reboot:

1. wait for `/api/v1/status` to report hardened/ready;
2. verify both expected streams and a snapshot;
3. test bounded PTZ movement and stop;
4. verify RTSP over TCP/554 from the approved NVR;
5. verify TCP/80 only redirects to TCP/443;
6. verify the device's HTTPS fingerprint and changed admin credential;
7. verify SSH password login fails and enrolled-key login succeeds, if enabled;
8. scan from the management host and confirm no unexpected steady-state ports;
9. capture router traffic through a reboot and confirm cloud/P2P traffic cannot
   leave the VLAN;
10. verify the uninstall/rollback path remains available.

If either sensor, hardening status, authentication, or rollback check fails,
disconnect the camera from all but the recovery workstation and follow
[Recovery](RECOVERY.md).
