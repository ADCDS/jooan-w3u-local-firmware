# JOOAN W3-U local firmware

<p align="center">
  <img src="docs/images/w3u-camera.jpg" width="320"
       alt="The JOOAN W3-U camera held in a hand: two lenses stacked in one white pan/tilt housing, two Wi-Fi antennas, and an Ethernet pigtail">
  <br>
  <sub>The camera this project targets: JOOAN W3-U (<code>JA-A12</code>), dual lens.</sub>
</p>

This project is a hardened, local-first retrofit for one verified JOOAN W3-U
hardware revision. It removes the camera's dependency on JOOAN cloud and P2P
services while keeping the vendor kernel modules and sensor tuning that drive
its two sensors.

This is **not** a complete open-source replacement firmware. It keeps the OEM
bootloader, Linux 3.10 kernel, vendor kernel modules, sensor tuning, radio
firmware and the OEM boot scripts that load them. The OEM application
`jooanipc` stays on flash, unchanged, as a fallback, but a normal (native) boot
never starts it: the boot hook brings up the watchdog, Wi-Fi and DHCP, and the
media daemon [jooanipc-open](media/) homes pan/tilt and serves video, ONVIF
and presets on its own ports and credentials. `jooanipc` runs only in the
guarded fallback, in rescue mode and on installs without the media layer; only
then is the containment guard active. The platform at the repository root
provides the authenticated Web UI/API and SSH, Wi-Fi, clock and signed updates,
and keeps the camera off the Internet by pruning default routes; router/VLAN
isolation remains the real boundary. See [Architecture](docs/ARCHITECTURE.md)
for the boot modes and the trust boundary.

## Repository layout

| Path | Layer | License |
| --- | --- | --- |
| `/` | Platform: boot hook and supervisor, `joan-daemon` admin UI/API, SSH, Wi-Fi, clock, signed updates, containment guard for the OEM application | GPL-2.0-only |
| [`media/`](media/) | Media daemon `jooanipc-open`: RTSP, ONVIF, pan/tilt, presets, the native-boot and handoff scripts, and the camera release-bundle installer (which reflashes the appfs) | MIT |

The `libimp.so` that the media daemon loads is built from the OpenIMP fork,
branch [`jooan/oem-t23-abi`](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi),
which stays a separate repository.

`media/` was the standalone `jooanipc-open` repository until 2026-11-05. Its
full history, with the original commit IDs, is part of this repository.

`ci/check.sh` runs every check of both layers, including the media daemon's
RTSP, ONVIF and HTTPS authentication integration tests
(`make -C media test integration integration-tls`). It needs `TOOLCHAIN_ROOT`,
`OEM_ROOTFS` and the release signing key. `ci/check-hosted.sh` runs the subset
that needs none of those private inputs, and GitHub Actions
(`.github/workflows/ci.yml`) runs it on every push.

## Exact hardware scope

Use this project only with a unit that matches all of the following:

- product/model reported by the OEM firmware: `JA-A12` / package token `A12`;
- PCB: `JA-6621 V1.0`;
- SoC: Ingenic T23N;
- sensors: dual `cv2005` and `cv2005s1`;
- radio: USB SeekWave SKW6316 (`SV6160LITE` on the verified unit);
- flash: 8 MiB SPI NOR (verified unit: Puya P25Q64HA).

The retail box of the verified unit carries this label: brand `JOOAN`, model
`W3-U`, FCC ID `2BBQ4-W3U`, 5 V / 1.5 A. It only identifies the product name,
which — as noted below — is not sufficient on its own; the checklist above is
what decides compatibility.

The Thingino image named for “Jooan W3-U” targets a different
SC2336P/ATBM6132U revision and is not interchangeable. Product name and case
shape are not sufficient compatibility evidence. Read
[Compatibility](docs/COMPATIBILITY.md) before building or installing anything.

## Security model

The target release surface is local and authenticated:

- the maintenance Web UI and `/api/v1/` are plain HTTP on TCP/80. The sign-in
  password and the session cookie cross the network unencrypted, so reach the
  camera only from the trusted, isolated network it lives on (see below);
- every HTTP request, sign-in and static files included, must carry a `Host`
  that is a private or loopback IP literal or the camera's `<label>.local`
  name; anything else, such as a router-assigned `name.lan`, gets
  `403 host_rejected`. This blocks DNS rebinding;
- the administrator is `admin` with initial password `admin`. It stays valid
  until changed and nothing is locked behind changing it; the System page and
  `/api/v1/setup/status` report whether the initial value is still in use;
- SSH on TCP/22 uses the same `admin` password as the Web UI; changing it
  updates both services, and optional Ed25519 authorized keys may be added;
- the platform keeps the password only as PBKDF2-SHA256 (Web UI) and MD5-crypt
  `$1$` (SSH, the strongest format the camera's uClibc supports). The OEM
  application's loopback RTSP gets a separate random secret;
- release artifacts never contain a shared private key; the one key the
  platform generates for itself only serves TLS on the loopback MQTT sink that
  the OEM application connects to in the fallback modes;
- video, ONVIF, pan/tilt and presets come from the media daemon: RTSP on
  TCP/18554 and HTTPS REST plus ONVIF on TCP/18443, with its own credentials,
  its own TLS certificate and a per-address failed-login throttle;
- optional network time (SNTP) from the gateway or another server on the
  camera's network; off by default, and unauthenticated beyond reply checks;
- embedded DNS-SD advertises HTTP, SSH and the camera service (never RTSP)
  under a configurable `.local` hostname, and answers only peers on a directly
  attached subnet;
- IPv4 and IPv6 default routes are removed and continuously pruned. Connected
  local-subnet routes remain, so local clients work without an Internet route.
  Administrator-allowlisted RFC1918/ULA prefixes are reinstalled as specific
  routes through the pruned gateway, so management clients on another local
  subnet can still be answered without ever restoring a default route. There
  is no packet filter;
- `telnetd` and GoAhead are killed every second, in every boot mode;
- in the modes where the OEM application runs, the exact-binary containment
  guard blocks its cloud/P2P egress, keeps its listeners on loopback and keeps
  its speaker and motion-alarm playback muted.

The generic image contains no Wi-Fi credentials. A native boot uses only the
network committed through the platform
(`/opt/custom/jooan-local/config/wifi.json`); the OEM's stored Wi-Fi
configuration stays on flash, untouched, and only the OEM application uses it.
Native boot therefore requires a committed network: without one it fails after
300 s, and the following boots end in OEM rescue mode because the handoff
needs that network too. Enter Wi-Fi credentials only over a trusted local
connection.

There is an immutable early-boot interval in which the retained OEM GoAhead
service may listen before `/opt/etc/local.rc` can run. Keep the camera on a router
VLAN or physically isolated network that blocks untrusted clients and Internet
access. Camera-side controls are defense in depth, not a substitute for router
isolation. See [Network security](docs/NETWORK-SECURITY.md).

## Repository contents

- `build.sh`, `build-uninstall.sh` and `release.sh`: reproducible release
  entry points;
- `packaging/`: exact-target manifest and IronMan package assembly;
- `runtime/`: boot hook, supervisor, slot and maintenance scripts;
- `src/`: `joan-daemon`, the containment guard, the release verifiers and
  the package tooling;
- `web/`: the maintenance Web UI;
- `tests/`, `tools/tests/` and `ci/`: host-side validation;
- `docs/`: installation, recovery, architecture, API, and security guidance;
- `media/`: the media daemon, its scripts, tests and documentation.

`release.sh` builds the deployable MIPS/uClibc runtime, assembles the audited
install/uninstall stages, and emits both packages under `dist/`. It requires
`TOOLCHAIN_ROOT`, a private compatible `OEM_ROOTFS` extraction for link-time
ABI libraries, and the `zopfli` compressor (the persistent budget has no room
for `gzip -9`; zopfli emits a smaller, standard gzip stream the camera reads
unchanged). The private inputs are not redistributed. The package is model- and hash-gated
and remains subject to the OEM IronMan limit of `0x200001` bytes. The persistent
steady layout stores a compressed controller core, a separate SSH recovery
bundle, and exactly one compressed runtime. Project regular files remain below
196 KiB (`200704` bytes), with at least 64 KiB free on `/opt`. A signed update
first enters controller-owned SSH recovery, deletes the old persistent runtime,
then stages one replacement while retaining at least 56 KiB free—above the OEM
startapp cleanup threshold of 50 KiB. The already-expanded old services continue
until reboot; a failed replacement leaves SSH recovery rather than a second slot.

The repository and its published releases intentionally contain no factory
secrets, Wi-Fi credentials, device keys, flash dumps, OEM firmware, vendor
kernel modules, sensor IQ files, radio firmware, or other vendor binaries.
Where a retained OEM component is required, the build/install workflow must use
files already present on the owner's compatible camera or inputs supplied
privately by that owner. A media-layer bundle carries an appfs image rebuilt
from the owner's own camera, so it stays private.

## Start here

1. Confirm every hardware identifier in [Compatibility](docs/COMPATIBILITY.md).
2. Put the camera on an isolated recovery network.
3. Preserve a complete flash backup before modifying the unit.
4. Read [Recovery](docs/RECOVERY.md). The platform package writes only
   `/opt`; a runtime that fails its health check leaves controller-owned SSH
   recovery. The media layer's installer reflashes the application partition
   (`/dev/mtd4`); power loss during that flash needs an SPI programmer and the
   backup.
5. Follow [Installation](docs/INSTALL.md).
6. Sign in as `admin` / `admin`. The System page shows the password state; a
   password change there also applies to SSH.

Do not expose an unprovisioned or freshly rebooted camera directly to the
Internet or to an untrusted LAN.

## Feature coverage

The OEM app drives this hardware through `jooanipc`'s local command surface (the
P2P/MQTT/CGI "DP" handlers). The table lists the capabilities that surface exposes
on the verified `JA-A12` unit — leaving out the cloud/P2P transport and account
plumbing — and whether the retrofit reimplements each one over its local,
authenticated interfaces. The retrofit deliberately prioritizes live control and
security; several OEM conveniences are intentionally not (yet) reimplemented.

Legend: ✓ implemented by the platform · ~ partial · ✗ not implemented ·
→ provided by the media daemon (`media/`), not the platform.

| Capability (OEM app · `jooanipc`) | Retrofit |
|---|---|
| **Video** | |
| Live view, dual sensor (OEM: main 2304×1296 + sub 640×360) | → RTSP for an NVR such as Frigate; the qualified profile is 1920×1080 + 640×360. The platform Web UI is a maintenance console and plays no video |
| Direct RTSP for NVR / VLC clients | → TCP/18554, Digest authentication |
| JPEG snapshot | → ~ `GET /api/v1/snapshot` on TCP/18443, unavailable in the qualified `main_only=1` profile; the platform has no snapshot route |
| Video flip / mirror | → `flip`, `hflip`, `vflip` controls |
| Digital zoom | ✗ |
| OSD timestamp / logo overlay | ~ only from the OEM application (fallback modes); the media daemon draws no overlay |
| **Pan / tilt** | |
| Jog (up / down / left / right) | → over ONVIF and the REST API |
| Presets — save / recall / delete | → 16 named presets |
| Control from an NVR (Frigate) | → over ONVIF on TCP/18443 |
| Home / calibration | → homes on every boot (the media daemon in a native boot, the OEM application before a handoff) |
| Cruise / patrol schedule | ✗ |
| Auto-tracking | ✗ |
| Privacy / PTZ-hide schedule | ✗ |
| **Audio** | |
| Listen (camera microphone) | ✗ (the qualified media profile runs with audio disabled) |
| Two-way talk | ✗ (the qualified media profile runs with audio disabled) |
| Speaker volume | ✗ |
| Custom voice / alert clips | ✗ |
| **Light & night vision** | |
| Day / Night / IR-cut mode | → manual `night`/`ircut` only; automatic day/night is not implemented |
| Floodlight / white light (+ schedule) | → manual `white_light` on/off; no schedule |
| **Detection & alarms** | |
| Motion detection (+ sensitivity, zones) | ✗ |
| Human / person detection (+ area) | ✗ |
| **Storage (microSD)** | |
| Scheduled / continuous recording | → ~ rotating local files; off in the qualified profile, which runs without an SD card |
| Playback | → ~ authenticated download of local recordings |
| Card format / capacity | ✗ |
| **Time** | |
| Set clock (from phone / UTC) | ✓ manually, or by optional SNTP from a local server |
| Time zone | ~ stored in the OEM configuration; only the OEM application's OSD uses it, on its next start |
| **Network** | |
| Wi-Fi credentials | ✓ transactional stage / commit / roll back; prefers 5 GHz, falls back to 2.4 GHz when 5 GHz is weak/absent |
| Wi-Fi scan (nearby APs) | ✗ |
| Signal / RSSI readout | ✓ `/api/v1/network/quality` (signal, rate, counters) |
| **System** | |
| Firmware update | ✓ signed, sequence-gated (platform); the media layer is updated by its installer |
| Change password | ✓ (synchronized to SSH) |
| Reboot | ✗ (only implicitly, when a signed update is applied) |
| Device info / UID | ~ version and state in `/api/v1/status` |

Beyond the OEM app, the retrofit adds a local maintenance **Web UI**, **SSH**
(password-synchronized, optional Ed25519 keys), **mDNS** `.local` naming,
**local-only egress** (continuous default-route pruning), a **native boot**
that never starts the OEM application, exact-binary `jooanipc` **cloud/P2P
containment** for the fallback modes, and optional **network time** (SNTP).

## Status and limitations

The platform implements a maintenance Web UI over plain HTTP; transactional
Wi-Fi; password-synchronized SSH with optional keys; embedded DNS-SD; optional
SNTP network time; signed, sequence-gated updates; and exact-binary `jooanipc`
containment for the fallback modes. It has served no media since 0.2.22. The
media daemon in `media/` owns video, ONVIF, pan/tilt and presets, and since
0.2.23 it boots natively without the OEM application. The current `0.2` line
(0.2.24, release sequence 58) keeps the compressed controller core, the
persistent SSH recovery bundle, and a journaled single-runtime maintenance
transaction for updates and migration. Host/native and emulated tests exercise
these contracts.

No release is supported or hardware-qualified yet. The software remains a
pre-release until signed install/uninstall, migration, Wi-Fi, network time,
SSH, routing, power-loss, and cold-recovery gates pass on the
physical JA-A12 unit and a supported tag is published.

The verified SKW6316 firmware connects to Realtek-based WPA bench access
points, but could not authenticate to the tested OpenWrt/ath11k AP even though
the BSS was visible and credentials matched. Channel, HE/HT mode, legacy rates,
WPA mode, UTF-8 SSID, virtual BSSID, nl80211/wext, and OEM network-manager
interference were ruled out. Qualify the intended access point before
committing a Wi-Fi transaction. Native boot and the handoff both wait for the
committed network and a gateway on `wlan0`, so without working Wi-Fi the
camera ends in OEM rescue mode (see [boot modes](docs/ARCHITECTURE.md#boot-modes)).

This is a hardware-specific retrofit for experienced users. The OEM
application `jooanipc` stays on flash as the fallback media owner. It is a
large, proprietary, unaudited process; where it runs, confinement reduces its
network reach but does not make it trustworthy. Neither open daemon drops root
privileges (see [Security policy](SECURITY.md)). Power-loss-safe installation,
rollback, and the documented network policy are release requirements, not
optional hardening suggestions.

See [Third-party components](docs/THIRD_PARTY.md) for licensing boundaries and
[Security policy](SECURITY.md) for reporting vulnerabilities.

## License

The platform (the repository root: original code and documentation outside
`media/`) is licensed under [GPL-2.0-only](LICENSE); every platform source file
carries an `SPDX-License-Identifier: GPL-2.0-only` header. `media/` is
MIT-licensed ([media/LICENSE](media/LICENSE)) and its files carry MIT SPDX
headers. Each file keeps its own license, including where an MIT script sources
a GPL one at run time.

`joan-daemon` and the platform's release verifiers are built against Mbed TLS
headers and dynamically link the Mbed TLS libraries already in the camera's
OEM rootfs; the project ships no Mbed TLS code or binaries. Because Apache-2.0
and GPL-2.0-only are generally considered incompatible, the copyright holder
grants an additional permission to link the platform code with Mbed TLS. The
exact text is in [Third-party components](docs/THIRD_PARTY.md#mbed-tls-linking-exception).

No license is granted here for OEM or third-party artifacts that are not
included in the repository.
