# Installation

This guide is for someone who owns a JOOAN W3-U camera and can follow
instructions and copy commands, but is not a Linux expert. Maintainers
building their own releases want [Building and releasing](BUILDING.md).

## Read this first

As of 2026-11-07, two releases are on
[GitHub Releases](https://github.com/ADCDS/jooan-w3u-local-firmware/releases):

- **v0.2.21** (2026-10-16) is the one this guide walks through, and the last
  release that has been installed on a stock camera. It installs over Wi-Fi with
  no extra hardware. You get a local web console, SSH, RTSP video from both
  lenses and ONVIF pan/tilt and presets for an NVR such as Frigate, and the
  camera can no longer reach the JOOAN cloud. The video still comes from the
  vendor's camera program, which keeps running under confinement.
- **v0.2.36** (2026-11-07) adds the open media daemon in
  [`media/`](../media/), and is what the rest of this repository describes.
  **No camera has yet been taken from stock to v0.2.36 with these
  instructions.** See [The media layer](#the-media-layer), including what has
  and has not been tested, before choosing it.
- **Each installs only on a camera whose vendor firmware matches the verified
  unit's.** Before it writes anything, the installer compares vendor files with
  the verified unit's: v0.2.21 the whole application partition, and v0.2.36 the
  twelve files it depends on. If they differ, it refuses and leaves the camera as
  it was. The media layer then asks more of your application partition: every
  file it keeps must be there, and the camera must have the CV2005 sensors.
  Nobody knows yet whether cameras sold today ship the same vendor firmware.
- No release is "supported" yet. Every release so far is an engineering
  pre-release, tested on one camera.

## Over the air or hardware?

| What | How | Extra hardware |
|---|---|---|
| First install | Over Wi-Fi, through the vendor firmware's built-in web updater | None |
| Later updates and uninstall | Over Wi-Fi, from the web console's Firmware section | None |
| Backup of the camera's flash | Over SSH, after the install ([step 7](#7-back-up-the-camera-over-ssh)) | None |
| Chip-level backup or restore | With an SPI flash programmer and SOIC-8 clip, camera opened | Optional for the install. It is the only way to recover a camera that no longer boots |
| Media layer (v0.2.36, not yet run from stock) | Over Wi-Fi and SSH. It rewrites the camera's application partition | None to install. A programmer and a backup are the only way back if power is lost while it writes |

**You do not need a microSD card, a USB cable, a serial adapter or a flash
programmer to install v0.2.21, and you do not need to open the camera.** The
Ethernet lead on the camera is not used in these instructions.

A microSD card is never required. When one is inserted, the firmware writes
crash logs to it. The media layer can also run its program files from a card
in its fallback boot mode, but that is maintainer territory, not an
installation route (see [The media layer](#the-media-layer)).

## What you need

- **The camera.** It must be a JOOAN W3-U whose label or vendor app says model
  `JA-A12`. Other cameras sold as "W3-U" use different chips: read
  [Compatibility](COMPATIBILITY.md). The installer checks the model and the
  vendor files and refuses a camera that differs. It cannot see the circuit
  board, so opening the camera is the only complete check (Compatibility
  explains what to look for).
- **The JOOAN phone app.** The app is how the vendor firmware gets onto your
  Wi-Fi. This project has no other way to put a stock camera on the network.
- **A Wi-Fi router** where:
  - the camera can join a WPA2 network. One OpenWrt access point with a
    Qualcomm ath11k radio could not be joined. Realtek-based access points
    worked (see [Compatibility](COMPATIBILITY.md#wi-fi-access-point-interoperability));
  - you can block one device's Internet access. Routers call this parental
    controls, access control or a firewall rule;
  - your PC is on the same network as the camera.
- **A PC** running Windows 10 or later, macOS or Linux, with:
  - Python 3.9 or newer;
  - the Python package `cryptography`;
  - an SSH client. It is built into Windows 10 and later, macOS and Linux.
- **About 30 minutes** and a stable power supply for the camera (5 V, 1.5 A).
  Do not unplug the camera while it is installing.

## Step by step

### 1. Set up the camera with the JOOAN app

Follow the app's normal setup and connect the camera to the Wi-Fi network it
will stay on. Changing the camera's network later is harder (see
[step 8](#8-use-the-camera)). The app pairs the camera through JOOAN's cloud,
so do this while the camera can still reach the Internet; step 3 blocks it
afterwards. (The vendor firmware also has a setup hotspot and an Ethernet
DHCP path, which might avoid the app; neither has been tested with this
guide.)

If the app offers a camera firmware update, decline it. A newer vendor
firmware changes the files the installer checks, and the installer then
refuses the camera.

**It worked if** the app shows live video.

### 2. Find the camera's IP address

Look in your router's list of connected devices. The vendor firmware asks the
router for an address under the name `JAIPC`. The camera's MAC address is in the app's device
information. It helps to reserve the address for the camera in the router's
DHCP settings so it does not change.

**It worked if** the camera answers `ping CAMERA_IP` from your PC. Here and
below, replace `CAMERA_IP` with the address you found, such as `192.168.1.50`.

### 3. Block the camera's Internet access

Block the camera, and only the camera, from the Internet at the router. Keep
your PC on the same network.

For a few seconds at every power-on, the vendor's web server and updater run
before this firmware takes over. The block keeps that window away from the
Internet. From now on the JOOAN app can no longer reach this camera. That is
expected.

**It worked if** the app shows the camera as offline and `ping CAMERA_IP`
still answers.

### 4. Prepare the PC

Download into one new folder:

1. The **v0.2.21 source code**, from
   <https://github.com/ADCDS/jooan-w3u-local-firmware/archive/refs/tags/v0.2.21.zip>.
   Unzip it. This gives a folder `jooan-w3u-local-firmware-0.2.21`.
2. These six files from the
   [v0.2.21 release page](https://github.com/ADCDS/jooan-w3u-local-firmware/releases/tag/v0.2.21).
   Put them together in a folder named `release` next to the source folder:
   `JOOAN_FW_PKG`, `JOOAN_FW_PKG.sha256`, `JOOAN_UNINSTALL`,
   `JOOAN_UNINSTALL.sha256`, `manifest.json` and `manifest.json.sig`.

The source must be v0.2.21, not the current `main` branch. The current upload
tool rejects the v0.2.21 package with `signed release target/ABI contract does
not match this repository`.

Install `cryptography`.

Linux or macOS:

```sh
python3 -m pip install --user cryptography
```

On Debian or Ubuntu, `sudo apt install python3-cryptography` works too.

Windows (Command Prompt):

```bat
py -m pip install cryptography
```

**It worked if** this prints a version number:

```sh
python3 -c "import cryptography; print(cryptography.__version__)"
```

On Windows, type `py` in place of `python3`, here and in step 5.

### 5. Install

Open a terminal (on Windows, Command Prompt) in the
`jooan-w3u-local-firmware-0.2.21` folder and run:

```sh
python3 tools/upload_ota.py --host CAMERA_IP --pkg ../release/JOOAN_FW_PKG
```

The tool checks the package's signature, sends the package to the camera's
vendor updater, and waits for the camera to restart with the new firmware.
Expect one to five minutes. Leave the camera powered.

**It worked if** the last line is:

```text
[+] jooan-local is active
```

If it fails:

| Message | Meaning and what to do |
|---|---|
| `error: signed release target/ABI contract does not match this repository` | You are running a different version of the source. Use the v0.2.21 zip from step 4 |
| `... requires the Python 'cryptography' package` | Install it as in step 4 |
| `[-] unexpected OEM response HTTP ...` | The camera's vendor updater refused the upload. Stop here |
| `[-] package upload completed but jooan-local did not become healthy` | The camera took the package, but this firmware did not start. Most likely the installer found different vendor files and refused the camera before writing anything. The camera keeps its vendor firmware. Stop here and see [If something goes wrong](#if-something-goes-wrong) |
| No answer at all | Check `CAMERA_IP` and that the PC and camera are on the same network |

### 6. Sign in

Open `http://CAMERA_IP/` in a browser and sign in:

```text
username: admin
password: admin
```

The password stays `admin` until you change it in the System section. The
same password is used for SSH and RTSP, and changing it changes all three.

**It worked if** `http://CAMERA_IP/api/v1/setup/status` shows
`"release_version":"0.2.21"`.

### 7. Back up the camera over SSH

Recommended. This saves a copy of every flash partition to your PC with no
extra hardware. Only this copy can bring this camera back if a later update
goes wrong: the vendor files it contains are not published anywhere. Restoring
a camera that no longer boots also needs a flash programmer (see
[Recovery](RECOVERY.md#external-spi-recovery)).

The camera has seven partitions, `mtd0` to `mtd6`. This command lists them:

```sh
ssh admin@CAMERA_IP cat /proc/mtd
```

Then save each partition. Run each command and enter the password when asked.
On Windows, use Command Prompt, not PowerShell. Windows PowerShell's `>`
damages binary files.

```sh
ssh admin@CAMERA_IP "cat /dev/mtdblock0" > camera-mtd0.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock1" > camera-mtd1.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock2" > camera-mtd2.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock3" > camera-mtd3.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock4" > camera-mtd4.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock5" > camera-mtd5.bin
ssh admin@CAMERA_IP "cat /dev/mtdblock6" > camera-mtd6.bin
```

**It worked if** the seven file sizes add up to 8388608 bytes (8 MiB), and for
`camera-mtd4.bin` (the application partition) the MD5 sum on the camera:

```sh
ssh admin@CAMERA_IP md5sum /dev/mtdblock4
```

matches the one on the PC: `md5sum camera-mtd4.bin` (Linux),
`md5 camera-mtd4.bin` (macOS) or `certutil -hashfile camera-mtd4.bin MD5`
(Windows).

These files hold this camera's identity, its Wi-Fi password and its keys.
Keep them private, never share them, and never write them to another camera.

### 8. Use the camera

With v0.2.21:

- **Video:** RTSP on port 554, user `admin` and the administrator password.
  `rtsp://CAMERA_IP:554/live/ch0` is the lens on the pan/tilt head and
  `rtsp://CAMERA_IP:554/live/ch1` is the fixed lens.
- **Pan/tilt and presets:** ONVIF at `CAMERA_IP`, port 80, same user and
  password.
- **SSH:** `ssh admin@CAMERA_IP`, same password.
- **Wi-Fi:** the camera stays on the network the app set up. To move it,
  enter the new network under Network → Wi-Fi in the web console, stage
  it, and commit it once the camera has reconnected. A change that is never
  committed is rolled back.
- **Clock:** the camera has no battery-backed clock. After a power cut, press
  "Sync time & zone to this device" under System → Camera time.

The v0.2.21 documentation describes these features. Read it at the tag:
[README](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/README.md)
and [API](https://github.com/ADCDS/jooan-w3u-local-firmware/blob/v0.2.21/docs/API.md).
The documents on `main` describe 0.2.36.

## Updating later

Install a newer published release from the web console. In the Firmware
section, choose its `JOOAN_FW_PKG`, wait for "signature verified", then apply
it. The camera reboots and is offline for about 90 seconds. A release is
accepted only if it is signed by this project and newer than the one
installed.

**Do not update a v0.2.21 camera to 0.2.22 or later unless you will install the
media layer straight after.** From 0.2.22 the platform no longer serves video
itself, so until then the camera has no video.

Releases after 0.2.21 answer only when they are addressed by a private IP
address or by the camera's own name. The name is `jooan-w3u` unless you
change it under Network → Local name. It may stand alone or end in `.local`,
`.lan`, `.home.arpa` or `.internal`, as in `jooan-w3u.local` or
`jooan-w3u.lan`. Any other name gets `403 host_rejected` (see
[Network security](NETWORK-SECURITY.md#host-and-origin-checks)).

## Uninstalling

In the Firmware section, apply the `JOOAN_UNINSTALL` file from the release you
installed. After the reboot the camera runs its vendor firmware unconfined
again: the vendor web server comes back, and the camera can reach the cloud
again unless the router still blocks it. The camera keeps the vendor's own
Wi-Fi settings. The platform's settings, including a Wi-Fi network committed
in it, are deleted. Details are in
[Recovery](RECOVERY.md#booting-camera-rollback).

**With the media layer installed, uninstalling does not remove it.** Its files in
`/opt/jooanipc-open` stay, including the stream password and the TLS private key,
so delete them first over SSH: `rm -rf /opt/jooanipc-open`. The application
partition also stays the reduced one, without the vendor's voice prompts and other
sensors' drivers. **Do not try to write it back on a running camera:** the program
running from that partition dies as it is erased, and a reset before the write ends
needs a flash programmer. Putting the original back takes a flash programmer and the
`my-appfs.img` you saved.

## If something goes wrong

- **The camera boots its vendor firmware after step 5.** If the installer
  refused the camera, it wrote nothing and the camera is unchanged. You can
  go back to the JOOAN app by removing the router block.
- **The web console or SSH stops answering after an update.** Power-cycle the
  camera once and wait three minutes. If it still does not answer, follow
  [Recovery](RECOVERY.md).
- **The camera no longer boots at all.** Only a chip-level restore of your
  backup with a flash programmer brings it back
  ([Recovery](RECOVERY.md#external-spi-recovery)).

When asking for help, never post flash images, passwords or keys (see
[Security policy](../SECURITY.md#reporting-a-vulnerability)).

## The media layer

From 0.2.22, video, ONVIF and pan/tilt come from the open media daemon
[jooanipc-open](../media/) instead of the vendor's program. In a native boot,
the default since 0.2.23, the vendor's program never starts. That is the
setup the rest of this repository describes.

**This install has not yet run on a real camera.** The only test camera got its
media layer by hand, is mounted out of reach, and cannot be brought back to
stock to try it. What has been checked, and what has not:

| Step | Checked |
|---|---|
| The image | Built from the real stock partition, it has the same files, byte for byte, as the image running on the test camera |
| The configuration | What the installer writes passes the shipped daemon's own check |
| The installer | Its decisions and order, against a stand-in for the camera; its preflight, against the real camera, which it correctly refuses |
| The flash script, `first-appfs.sh` | Only read and checked statically. It has never run. Most of its sequence comes from the script behind every later update, which has run many times |
| Installing 0.2.36 from stock | Not repeated since v0.2.21, through the same updater |

If the flash fails its readback, the script restores the partition from a copy
in RAM. **If power is lost while it writes, only a flash programmer and a backup
of that partition can bring the camera back** (see
[Recovery](RECOVERY.md#appfs-reflash-recovery)). If you need the camera to keep
working, stay on v0.2.21 until someone has reported this working.

You need a computer with Linux or WSL, with `squashfs-tools`, Python 3, OpenSSH
and OpenSSL, and the v0.2.36 files from the release page.

1. **Install the platform.** Follow steps 4 and 5 with the v0.2.36 source and
   files, or from v0.2.21 update as in [Updating later](#updating-later). The
   camera now has no video until the last step. The web console stays at
   `http://CAMERA_IP/`.
2. **Back up the camera** ([step 7](#7-back-up-the-camera-over-ssh)).
3. **Commit its Wi-Fi network** under Network → Wi-Fi in the web console, even
   if it is the one the camera is on. A native boot uses only the network
   committed there, not the one the JOOAN app set up.
4. **Unpack** `jooanipc-open-*.tar.gz` and `libimp-*.tar.gz` next to each other,
   and open a terminal in the `jooanipc-open-*` folder.
5. **Copy your camera's application partition, and build your image from it.**
   They contain vendor files, so keep them private:

   ```sh
   ssh admin@CAMERA_IP 'cat /dev/mtdblock4' > my-appfs.img
   sh media/tools/build-appfs.sh --first my-appfs.img bin/jooanipc-open ../libimp-*/libimp.so bin/libatomic.so.1 my-bundle
   ```

   It refuses a partition that lacks a file it needs, or holds a different
   vendor program from the one the platform pins.

6. **Check the camera.** This changes nothing:

   ```sh
   CAMERA_PASSWORD=admin python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP
   ```

7. **Install,** choosing the password for the camera's video streams:

   ```sh
   CAMERA_PASSWORD=admin STREAM_PASSWORD=your-choice python3 media/scripts/install-camera.py my-bundle --camera CAMERA_IP --yes-untested
   ```

   Leave the camera powered. **It worked if** the last line says the release is
   running. The admin UI is now at `https://CAMERA_IP/`, with the camera's own
   certificate, so the browser warns once. Sign in with the admin password.
   `http://CAMERA_IP/` now only redirects there.

The boot checks itself: a native boot that records a failure, or is cut short
three times in a row, makes the next boot fall back to the vendor's program. So does
one with no gateway after 5 minutes, until a native boot has qualified once (see
[Recovery](RECOVERY.md#a-media-change-that-stops-native-boot)).

**On Windows,** build the bundle in WSL, then run the installer with `py` in
Command Prompt, using `set` for each password. It uses the OpenSSH that comes
with Windows, and needs `openssl` on the path, which Git for Windows includes.
OpenSSH 8.4 or later (Windows 11) takes the password from `CAMERA_PASSWORD`;
Windows 10's version asks for it at every connection, so add an SSH key in the
web console first. This has not been run on Windows either.

**Later media updates** start from your camera's current partition, with
`build-appfs.sh` without `--first`, and a `bundle.json` you write for it (see
[camera release bundles](../media/docs/hardware.md#camera-release-bundles)). The
installer then also needs `MEDIA_PASSWORD`, the stream password, to verify the
result. The admin UI's System → Media form only accepts packages
signed with this project's release key. Since an image holds vendor files, none
is published, so that form is for the key's holder or anyone building the
platform with their own key.

**SD card:** the media layer's fallback boot mode can run the daemon from a
microSD card instead of the application partition. Only the vendor's program
mounts the card, so this works only in that fallback mode. It is not a way
around any of the steps above.

The format of a bundle is in
[camera release bundles](../media/docs/hardware.md#camera-release-bundles).
