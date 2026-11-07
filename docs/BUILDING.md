# Building and releasing

Maintainer material: building and signing the platform package, the update
transports, the media layer's release bundles, and the qualification
checklist. Owners installing a published release want
[Installation](INSTALL.md).

## Inputs

| Input | Needed for | Public? |
|---|---|---|
| Ingenic GCC 5.4 multilib toolchain (`TOOLCHAIN_ROOT`) | platform and media MIPS builds | Yes: `tools/fetch-ingenic-toolchain.sh OUTPUT-DIR` downloads and checks it (needs `curl`, `unar`, `sha256sum`) |
| Extracted OEM rootfs (`OEM_ROOTFS`) | linking against the camera's ABI libraries; media QEMU tests | No: taken from the owner's own flash dump, never redistributed |
| Release signing key (`JOOAN_RELEASE_SIGNING_KEY`) | signing platform releases | No: the maintainer's ECDSA P-256 key, mode 0600, kept outside Git |
| `zopfli` | platform packaging | Yes |
| Python 3 with `cryptography` | signing and host-side verification | Yes |
| `squashfs-tools` (`unsquashfs`, `mksquashfs`) | rebuilding the appfs image | Yes |
| The camera's own appfs (`/dev/mtdblock4`, 3211264 bytes) | media release bundles | No: vendor files |
| `libimp.so` and `libatomic.so.1` from the [OpenIMP fork](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi) | media runtime | Published since v0.2.35, with the source of `libimp.so` beside it |

## Platform package

```sh
TOOLCHAIN_ROOT=/path/to/mips-gcc540-glibc222-64bit-r3.3.0 \
OEM_ROOTFS=/private/path/to/extracted-rootfs \
JOOAN_RELEASE_SIGNING_KEY=/secure/release-signing-key.pem \
./release.sh
```

`release.sh` needs a clean, committed tree (for provenance). Without
`JOOAN_RELEASE_SIGNING_KEY` it looks for
`~/.config/jooan-w3u-local-firmware/release-signing-key.pem` and refuses to
build unsigned artifacts. It builds the MIPS/uClibc runtime
(`tools/build-target.sh`), assembles the audited install and uninstall stages
(`packaging/assemble-stages.sh`), and runs `build.sh`. Outputs in `dist/`:
`JOOAN_FW_PKG`, `JOOAN_UNINSTALL`, their SHA-256 sidecars, `manifest.json` and
`manifest.json.sig`. The version and sequence come from `packaging/VERSION`
and `packaging/RELEASE_SEQUENCE`.

Publish all six files as GitHub release assets, as for v0.2.21.
`tools/upload_ota.py` needs `manifest.json` and `manifest.json.sig` next to
the package.

### Authenticity

The device authenticates the inner SHA-256 inventory using the pinned ECDSA
P-256/SHA-256 release key, checks exact component hashes, and rejects
downgrade and replay by sequence. OEM MD5 fields are carrier metadata, not the
authenticity boundary. The package is model- and hash-gated: before it writes
anything, the installer checks `/etc/deviceModel` (`JA-A12`) and every file in
the target's `compatibility_hashes` (`packaging/targets/ja-a12.json`). In
v0.2.21 that list includes `/dev/mtdblock4`, the whole appfs, so v0.2.21
installs only on an unmodified stock appfs. Later releases pin the
individual OEM files instead, so a rebuilt appfs that keeps them unchanged
still accepts platform updates.

`ci/check_compatibility.py --device-manifest FILE` compares a sha256sum-style
list taken from a camera with the target contract.

### Size budget

The package remains subject to the OEM IronMan limit: the decoded payload must
be smaller than `0x200001` bytes. The persistent steady layout stores a
compressed controller core, a separate SSH recovery bundle, and exactly one
compressed runtime. Project regular files stay below 196 KiB (`200704`
bytes), with at least 64 KiB (`65536` bytes) free on `/opt`. `zopfli` is
required because the budget has no room for `gzip -9`. It emits a smaller,
standard gzip stream that the camera reads unchanged.

A signed update first enters controller-owned SSH recovery
(`selection = - - 0`), deletes the old persistent runtime, then stages one
replacement while keeping at least 56 KiB (`57344` bytes) free. That is above
the OEM `startapp` cleanup threshold of 50 KiB. The already-expanded old
services continue until reboot. A failed replacement leaves SSH recovery
rather than a second slot. Do not expect two complete runtime archives to
coexist on this hardware. [Architecture](ARCHITECTURE.md) and
[Recovery](RECOVERY.md) describe the transaction.

## Update transports

**First install:** `tools/upload_ota.py --host CAMERA_IP --pkg dist/JOOAN_FW_PKG`.
Its first checks run on the host: the signed `manifest.json` against the
checkout's `packaging/targets/ja-a12.json`, then the package's name, size,
SHA-256 and A12 container. Run it from the source tree of the release being
installed: a different tree's target contract rejects the manifest. It
then POSTs the package to the OEM updater (GoAhead's
`/cgi-bin/upload_app.cgi`, unauthenticated) and polls
`/api/v1/setup/status` on TCP/80 until the signed version and sequence
appear. The default timeout is 300 s (`--timeout`). `--no-wait` cannot prove
success and always exits non-zero. A camera that already runs the platform gets
no upload: the tool says so, and exits non-zero unless that is the same release.
First-install GoAhead is unauthenticated, so never upload over an untrusted
network.

**Later releases:** once the platform runs, GoAhead is killed every second.
Updates go through the authenticated `/api/v1/update`: `POST` stores and
verifies the package, and `PUT` with its `id` applies it and reboots (see
[API](API.md#signed-updates)). The web console's Firmware section drives the
same API.

### Upgrades from 0.1

An early `0.1` web password is preserved during upgrade. Its one-way hash
cannot generate the SSH hash, so status reports `ssh_password_sync: false`
until the password is changed once. Recognized `/opt/open` predecessor trees
are migrated through a journal. Validated SSH material is preserved, and the
old tree is retired only after activation. Unknown or partial predecessors
fail closed.

## Checks

`ci/check.sh` runs every check of both layers, including the media daemon's
RTSP, ONVIF and HTTPS authentication integration tests
(`make -C media test integration integration-tls`). It needs `TOOLCHAIN_ROOT`,
`OEM_ROOTFS` and the release signing key. `ci/check-hosted.sh` runs the
subset that needs none of those private inputs, and GitHub Actions
(`.github/workflows/ci.yml`) runs it on every push. The test plan is in
[tests/TEST_PLAN.md](../tests/TEST_PLAN.md).

## Media layer

The media daemon builds from public inputs (`TOOLCHAIN_ROOT=... make -C media
mips`; see [media/README.md](../media/README.md#build-for-the-camera)).
`libimp.so` comes from the OpenIMP fork. A camera release puts the daemon and
those two libraries into a versioned directory,
`/mnt/mtd/jooanipc-open/releases/<app sha256 prefix>-<libimp sha256 prefix>`,
inside a rebuilt appfs image.

`media/tools/build-appfs.sh BASE_IMAGE OLD_RELEASE_ID STRIPPED_BINARY OUT_DIR`
rebuilds that image on the workstation from a copy of the camera's own appfs.
It replaces the daemon of an **existing** release directory and keeps its
libraries, or also replaces `libimp.so` when given one more argument.

`media/tools/build-appfs.sh --first STOCK_IMAGE STRIPPED_BINARY LIBIMP LIBATOMIC OUT_DIR`
makes the first image from a stock appfs. It keeps what
`media/config/ja-a12-appfs-keep.txt` lists, which drops files this board does not
use (voice prompts, other sensors' drivers) so that the image fits (see
[internal-flash packaging](../media/docs/hardware.md#internal-flash-packaging)),
refuses a stock image that lacks one of those files or does not match the two
files the platform pins, and writes a whole bundle.

`media/scripts/install-camera.py BUNDLE --camera CAMERA_IP [--yes]` installs a
release bundle. The bundle holds `bundle.json`, an optional platform
package, an optional appfs image, and configuration changes. The format is in
[camera release bundles](../media/docs/hardware.md#camera-release-bundles).
`CAMERA_PASSWORD` (SSH and the platform API) and `MEDIA_PASSWORD` (the media
daemon; defaults to `CAMERA_PASSWORD`) come from the environment. SSH uses
`sshpass` when it is installed, or otherwise `ssh`'s own `SSH_ASKPASS`, which
also works with the OpenSSH in Windows from 8.4. It is not Paramiko because the
camera's Dropbear offers only `chacha20-poly1305@openssh.com`, which Paramiko
does not have: 5.1.0 fails with "no acceptable ciphers". Give `--camera` the camera's private IP
address or its own name (see
[Host and origin checks](NETWORK-SECURITY.md#host-and-origin-checks)).

Without `--yes` only preflight runs. Preflight refuses, among other things:

- a handoff `boot-trial` or `native-failed` that is latched (it shows the
  recorded stage);
- a media daemon that did not qualify on the current boot;
- a result that would be a native boot without a committed Wi-Fi network,
  since that boot fails after 300 s and latches `native-failed`.

It talks to the media daemon's HTTPS only after pinning that daemon's own
certificate (`/opt/jooanipc-open/cert.pem`). The certificate is read over SSH,
which authenticates the camera by its host key.

The appfs installer (`upgrade-appfs.sh`) runs only from a qualified native
boot. It takes the platform maintenance lock, which pauses the supervisor's
runtime restarts and network-watchdog reboot. It then stops the media daemon,
hands the armed watchdog to a rootfs feeder, keeps a RAM copy of the current
appfs, and flashes the new image with full readback. `native-boot.sh` is replaced
at the same commit point, so a release always boots with the script it was tested
with. Any failure before the commit point reboots into the untouched appfs. A
failed flash after it is rolled back from the RAM copy, along with the boot script. If even the rollback fails, the camera stays
up for manual recovery: see [Recovery](RECOVERY.md#appfs-reflash-recovery).
Stop the NVR during a platform update (`--nvr-stop`/`--nvr-start`).

**The first media release** goes on with a bundle from `build-appfs.sh --first`,
using the same installer. Its preflight wants a camera with platform sequence 69
or later and the stock application running, the CV2005 sensors, the appfs its
bundle was built from, a committed Wi-Fi network and 96 KiB free in `/opt`, and no
media layer. The installer then
makes the camera's TLS certificate, stages `camera.ini` from
[`media/config/ja-a12.example.ini`](../media/config/ja-a12.example.ini) and
`deployment.env`, and runs `first-appfs.sh`. That script is separate from
`upgrade-appfs.sh`, so that the path every later update takes stays as it was
qualified. It takes the lock, hands the watchdog from `my_watch_dog` to the
rootfs feeder, stops `jooanipc` and `syslogd`, and from there works like
`upgrade-appfs.sh`; `enabled` and `native` appear only at its commit point.

**That path has not yet run on a camera,** so it asks for `--yes-untested`
instead of `--yes`. The checks that have been done are listed in
[The media layer](INSTALL.md#the-media-layer).

**From the browser,** System → Media takes a package from
`media/tools/make-media-update.py BUNDLE_DIR`, signed with the release key: a
manifest with the sums of the image, the libraries and the scripts, the three
scripts (`native-boot.sh` is the third), and the image. The daemon checks the signature with the platform's
verifier and the camera's state with the signed `apply-media-update.sh`, then
keeps the package in RAM for 15 minutes for the owner to confirm. That script
then runs the signed `upgrade-appfs.sh`. The package holds vendor files and is
specific to one camera's partition, so it is private. It carries only the release:
a bundle that also changes `camera.ini` or files is refused, and has to go through
the installer.

## GitHub releases

`tools/make-github-release.py BUNDLE_DIR --openimp CHECKOUT --out DIR` puts a
release together from a clean checkout on `origin/main`, then `gh release create`
publishes it. It takes `dist/` as built, and reads only the daemon and its two
libraries out of the bundle's image, so what is published is what has been
running on the test camera. It also archives the OpenIMP source that rebuilds
that exact `libimp.so`, and the notices. Its last step refuses anything that
looks like a partition image, has the name of a vendor file, or holds a private
key, including inside the archives. Publish from the workstation, because
`dist/` needs the OEM rootfs and the signing key.

## Qualification checklist

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
