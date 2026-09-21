# Installation

`0.1.0` is a pre-release engineering image, not a supported tag, until all
physical JA-A12 promotion gates pass. Installation writes only `/opt`; it does
not replace the bootloader, kernel, rootfs, appfs, calibration, or identity.

## Before installation

1. Put the camera and workstation on an isolated VLAN with no Internet route.
   OEM GoAhead is reachable during an immutable early-boot interval before
   `/opt/etc/local.rc` can run, so router isolation is mandatory.
2. Verify every identifier in [Compatibility](COMPATIBILITY.md). The package is
   only for `JA-A12`, T23N, dual CV2005/CV2005S1, and USB SKW6316.
3. Preserve and hash a full 8 MiB SPI-NOR backup. Keep the signed uninstall
   package off-device and read [Recovery](RECOVERY.md).
4. Confirm stable power. Do not interrupt an update.

## Build

Maintainers build both signed artifacts with an external release key:

```sh
TOOLCHAIN_ROOT=/path/to/mips-gcc540-glibc222-64bit-r3.3.0 \
OEM_ROOTFS=/private/path/to/extracted-rootfs \
JOOAN_RELEASE_SIGNING_KEY=/secure/release-signing-key.pem \
./release.sh
```

Outputs include `dist/JOOAN_FW_PKG`, `dist/JOOAN_UNINSTALL`, SHA-256 sidecars,
and `manifest.json`. The device authenticates the inner SHA-256 inventory using
the pinned ECDSA P-256/SHA-256 release key, checks exact component hashes, and
rejects downgrade/replay by sequence. OEM MD5 fields are carrier metadata, not
the authenticity boundary.

The steady contract stores a compressed controller core, separate SSH recovery
bundle, and exactly one compressed runtime. Regular-file content is capped at
184320 bytes (180 KiB), with at least 77824 bytes (76 KiB) free on `/opt`.
Updates deliberately enter SSH recovery (`selection = - - 0`), remove the old
persistent runtime, and stage one replacement while preserving at least 57344
bytes (56 KiB), above the OEM startapp cleanup threshold of 50 KiB. Do not expect
two complete runtime archives to coexist on this hardware.

## Upload

On the isolated LAN, use the included carrier tool:

```sh
python3 tools/upload_ota.py \
  --host CAMERA_IP \
  --pkg dist/JOOAN_FW_PKG
```

It validates the A12 container, POSTs it to the OEM updater, waits for HTTPS on
443, and prints the per-device certificate SHA-256 fingerprint. Record and
verify that fingerprint on the isolated segment. First-install GoAhead is
unauthenticated; never upload over an untrusted network. Later releases use the
authenticated HTTPS update API.

## First login and migration

HTTPS is the default. TCP/80 only redirects. Initial credentials are:

```text
username: admin
password: change-me-password
```

The public initial password remains valid; setup is not forced. The UI/API
shows a persistent warning until it is changed. Change it promptly. The same
`admin` password is synchronized to SSH and RTSP; optional Ed25519 authorized
keys supplement SSH password authentication.

An early `0.1` Web password is preserved during upgrade. Because its one-way
hash cannot generate the SSH hash, status reports `ssh_password_sync: false`
until the password is changed once. Recognized `/opt/open` predecessor trees
are migrated through a journal; validated SSH material is preserved and the old
tree is retired only after activation. Unknown/partial predecessors fail closed.

Existing OEM Wi-Fi settings are preserved. Replacement Wi-Fi is a manual,
transactional stage/commit operation. The runtime removes IPv4 and IPv6 default
routes; connected subnets and explicitly allowed RFC1918/ULA routes remain.

## Acceptance checklist

After a cold reboot:

1. verify HTTPS identity, warning state, login throttling, and session expiry;
2. verify main/sub fMP4 playback, both direct RTSP streams, and snapshot;
3. verify mic listening and bounded press-to-talk;
4. verify PTZ jog/stop, home, and preset save/recall/delete;
5. verify SSH `admin` password synchronization and optional keys;
6. verify DNS-SD announcements for HTTPS, SSH, RTSP, and the camera service;
7. verify no IPv4/IPv6 default route and no public resolver remains;
8. scan listeners and capture traffic across reboot, including early GoAhead;
9. verify the signed uninstall and recovery assets remain available.

Failure of any item keeps the image unqualified. Isolate the unit and follow
[Recovery](RECOVERY.md).
