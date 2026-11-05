# Installation

0.2.24 (release sequence 58) is a pre-release engineering image, not a
supported tag, until all physical JA-A12 promotion gates pass.

A deployment has two layers, installed separately:

- **The platform package** (`JOOAN_FW_PKG`, this repository's root) writes
  only `/opt`. It does not replace the bootloader, kernel, rootfs, appfs,
  calibration, or identity.
- **The media layer** ([jooanipc-open](../media/)) is installed by
  `media/scripts/install-camera.py`, whose `upgrade-appfs.sh` **reflashes the
  application partition** (`/dev/mtd4`, the appfs that holds the OEM
  `startapp`, `jooanipc`, modules and sensor IQ) with a rebuilt image carrying
  the media release. See [Media layer](#media-layer).

Without the media layer nothing serves video to the network: the guarded OEM
application runs for the whole uptime with its listeners on loopback
(platform-only mode).

## Before installation

1. Put the camera and workstation on an isolated VLAN with no Internet route.
   OEM GoAhead is reachable during an immutable early-boot interval before
   `/opt/etc/local.rc` can run, so router isolation is mandatory.
2. Verify every identifier in [Compatibility](COMPATIBILITY.md). The package is
   only for `JA-A12`, T23N, dual CV2005/CV2005S1, and USB SKW6316.
3. Preserve and hash a full 8 MiB SPI-NOR backup. Keep the signed uninstall
   package off-device and read [Recovery](RECOVERY.md). The appfs reflash
   cannot be undone from the camera if power fails during it.
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
200704 bytes (196 KiB), with at least 65536 bytes (64 KiB) free on `/opt`.
Updates deliberately enter SSH recovery (`selection = - - 0`), remove the old
persistent runtime, and stage one replacement while preserving at least 57344
bytes (56 KiB), above the OEM startapp cleanup threshold of 50 KiB. Do not expect
two complete runtime archives to coexist on this hardware.

## Upload

On the isolated LAN, use the included carrier tool for the first install:

```sh
python3 tools/upload_ota.py \
  --host CAMERA_IP \
  --pkg dist/JOOAN_FW_PKG
```

It validates the A12 container, POSTs it to the OEM updater (GoAhead's
`/cgi-bin/upload_app.cgi`), and waits until the camera reports the signed
release on TCP/80. First-install GoAhead is unauthenticated; never upload over
an untrusted network. Once the platform runs, GoAhead is killed every second
and later releases use the authenticated update API (`/api/v1/update`), which
runs the verified package's own installer.

## First login and migration

The Web UI is plain HTTP on TCP/80 (see
[Network security](NETWORK-SECURITY.md#plain-http)). Open it by the camera's
IP address or by `<label>.local` (default `jooan-w3u.local`); any other host
name, such as a router-assigned `name.lan`, is refused with
`403 host_rejected`. Initial credentials are:

```text
username: admin
password: admin
```

The initial password remains valid; setup is not forced and nothing is locked
until it is changed. The UI shows no banner for it: the System page shows the
password state, and `/api/v1/setup/status` reports `default_password_warning`.
The same `admin` password is synchronized to SSH, which accepts password login
from the start; optional Ed25519 authorized keys supplement it.

An early `0.1` Web password is preserved during upgrade. Because its one-way
hash cannot generate the SSH hash, status reports `ssh_password_sync: false`
until the password is changed once. Recognized `/opt/open` predecessor trees
are migrated through a journal; validated SSH material is preserved and the old
tree is retired only after activation. Unknown/partial predecessors fail closed.

Wi-Fi is a manual, transactional stage/commit operation. The platform never
edits the OEM's stored Wi-Fi configuration, which the OEM application keeps
using in the fallback modes. A native boot uses only the committed network, so
commit one before enabling native boot. The runtime removes IPv4 and IPv6
default routes; connected subnets and explicitly allowed RFC1918/ULA routes
remain.

## Media layer

`media/scripts/install-camera.py` installs a release bundle: an optional
platform package (applied through `/api/v1/update`), an optional rebuilt appfs
image with one jooanipc-open release, and file-level configuration changes.
The bundle format and the deployed layout are in
[camera release bundles](../media/docs/hardware.md#camera-release-bundles).
Pass the camera's IP address (or `.local` name) as `--camera`; the platform
API refuses other host names.

Without `--yes` only preflight runs. Preflight refuses, among other things,
when a handoff `boot-trial` or `native-failed` is latched (it shows the
recorded stage), when the media daemon did not qualify on the current boot, and
when the result would be a native boot without a committed Wi-Fi network, since
that boot fails after 300 s and latches `native-failed`. It talks to the media
daemon's HTTPS only after pinning that daemon's own certificate
(`/opt/jooanipc-open/cert.pem`, read over SSH, which authenticates the camera
by its host key).

The appfs installer (`upgrade-appfs.sh`) runs only from a qualified native
boot. It takes the platform maintenance lock, which pauses the supervisor's
runtime restarts and network-watchdog reboot, stops the media daemon, hands
the armed watchdog to a rootfs feeder, keeps a RAM copy of the current appfs,
and flashes the new image with full readback. Any failure before the commit
point reboots into the untouched appfs; a failed flash after it is rolled back
from the RAM copy. If even the rollback fails, the camera stays up for manual
recovery: see [Recovery](RECOVERY.md#appfs-reflash-recovery).

## Acceptance checklist

After a cold reboot:

1. verify the password state, login throttling, session expiry, and that a
   foreign `Host` gets `403 host_rejected`;
2. verify the boot mode (`/tmp/jipc-native.log` ends with "native boot
   qualified") and the listeners in
   [Network security](NETWORK-SECURITY.md#listeners-by-boot-mode);
3. if network time is set, verify `GET /api/v1/time` reports it synchronized;
4. verify SSH `admin` password synchronization and optional keys;
5. verify DNS-SD announcements for HTTP, SSH, and the camera service, and that
   no RTSP service is advertised;
6. verify no IPv4/IPv6 default route and no public resolver remains;
7. scan listeners and capture traffic across reboot, including early GoAhead;
8. verify the signed uninstall and recovery assets remain available.

Failure of any item keeps the image unqualified. Isolate the unit and follow
[Recovery](RECOVERY.md).
