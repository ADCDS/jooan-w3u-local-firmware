# Architecture

## Design choice

The retrofit is deliberately incremental. The verified JA-A12 has a customized
T23 dual-sensor pipeline for which there is no complete redistributable open
replacement. The retrofit keeps the OEM kernel, vendor kernel modules and the
sensor tuning that make the CV2005/CV2005S1 pair work, and replaces the
userspace above them: the media daemon [jooanipc-open](../media/) drives the
vendor drivers through an OpenIMP build and serves video, ONVIF and pan/tilt,
while the platform at the repository root provides a hardened local control
plane. The OEM application `jooanipc` stays on flash, unchanged, as the
fallback media owner.

```text
Administrator browser / SSH          NVR (Frigate)
        |                                  |
  TCP/443 admin UI (media), TCP/22   TCP/18554 RTSP, TCP/18443 HTTPS + ONVIF
        |                                  |
   platform (repository root)        media daemon jooanipc-open (media/)
   joan-daemon, dropbear,            OpenIMP libimp.so
   supervisor: Wi-Fi, routes,              |
   clock, signed updates           vendor ISP/VPU/sensor/motor drivers
                                           |
                                   cv2005 + cv2005s1 sensors

Fallback modes only: OEM jooanipc (vendor IMP), confined by
libjooan_guard.so; the platform's loopback MQTT sink and API stub serve it.

Bootloader -> OEM Linux 3.10 -> vendor modules -> OEM startapp
           -> /opt/etc/local.rc -> platform supervisor + media boot
```

## Retained layer

The following remain device-supplied and proprietary:

- bootloader and OEM Linux 3.10 kernel;
- ISP, VPU, sensor, audio, motor, and SeekWave kernel modules;
- sensor IQ/calibration and radio firmware;
- the OEM boot scripts that load those modules, including `startapp`, which
  sources `/opt/etc/local.rc`;
- `jooanipc`, which a native boot never starts; it runs only in the fallback
  modes below.

These components are not imported into this repository or a generic release.
They remain on the owner's camera. Their behavior is treated as an untrusted
dependency behind a narrow boundary. The media layer's installer rewrites the
application partition (`/dev/mtd4`) with an image rebuilt from the owner's
own appfs: the OEM files the platform pins stay byte-identical, and the
partition gains one jooanipc-open release.

## Open retrofit layer

The platform is responsible for:

- model-gated, transactional installation and uninstall packages;
- a local HTTP listener with authenticated session handling;
- the versioned `/api/v1/` management contract;
- password-synchronized SSH for user `admin`, with optional authorized keys;
- no-default-route local networking, plus exact-process egress containment
  whenever the OEM application runs;
- the boot hook that selects the boot mode and, in a native boot, starts the
  watchdog feeder, Wi-Fi and DHCP;
- embedded mDNS/DNS-SD discovery;
- optional SNTP network time from a server on the camera's network;
- health checks, update verification, and rollback/fallback handling.

The media daemon owns RTSP, ONVIF, pan/tilt homing, presets and the NVR
integration on its own ports and credentials (see [media/](../media/README.md)).

The generic image does not personalize Wi-Fi or embed keys. The per-device key
for the loopback MQTT sink, the administrator credential, optional SSH
authorized keys, and the committed Wi-Fi network live in device-local
persistent state and are excluded from release artifacts. The initial password
remains usable until changed; `default_password_warning` records that
condition, and nothing is locked behind it.

## Boot modes

The OEM `startapp` sources `/opt/etc/local.rc` and afterwards calls
`my_watch_dog &` and `jooanipc &` by name. `local.rc` redefines those names,
and the files in `/opt/jooanipc-open/` select the mode:

| Mode | Selected when | What runs |
|---|---|---|
| Native (default, deployed) | `enabled` and `native` present, no `native-failed` | `jooanipc` and `my_watch_dog` are no-ops; the OEM application never starts. The containment guard is installed but nothing loads it. |
| Handoff (fallback) | `enabled` present, and `native` absent, `native-failed` latched or `native-boot.sh` missing | The guarded OEM `jooanipc` brings up Wi-Fi and homes the motor, then `boot-handoff.sh` stops it and starts jooanipc-open. |
| OEM rescue | the handoff is selected but `boot-trial` is latched, or the handoff gives up | The guarded OEM `jooanipc` runs for the whole uptime. |
| Platform-only | no `enabled` (no media layer installed) | The guarded OEM `jooanipc` runs for the whole uptime. |
| Controller invalid | the platform controller cannot be loaded | OEM `jooanipc` is suppressed; `telnetd` and GoAhead are killed every second. |

The containment guard is active only in the handoff, rescue and platform-only
modes. `telnetd` and GoAhead are killed every second in every mode; with a
valid controller the supervisor also prunes default routes.

A native boot works like this:

1. `local.rc` starts the rootfs `/sbin/watchdog` feeder on `/dev/watchdog`.
   If the feeder is still running after ten minutes, a timer kills it without
   the magic close, so a native boot that hangs is reset by the hardware
   watchdog.
2. It starts `wpa_supplicant` with an empty network list and a persistent
   `udhcpc` on `wlan0`. The supervisor then applies the committed network from
   `/opt/custom/jooan-local/config/wifi.json`. The OEM's stored Wi-Fi
   configuration is not used, so native boot requires a committed network.
3. `native-boot.sh` verifies the release, waits up to 300 s for a gateway on
   `wlan0`, stops the feeder and launches jooanipc-open, which homes pan/tilt
   (`motor_home=1`) and takes over `/dev/watchdog` (`watchdog=1`).
4. After the daemon reports ready, the boot qualifies once it has stayed
   healthy for 185 s with homing complete and the network still up.

Until it qualifies, `native-trial` on `/opt` records the stage reached. A boot
that finds `native-trial` renames it `native-failed` (keeping the recorded
stage) and takes the handoff instead. Nothing clears `native-failed`
automatically; delete it to retry native boot. If `native-boot.sh` finds the
boot lock already held, it records `failed: boot lock held by another boot
script` in `native-trial` and exits, so the ten-minute feeder timer resets the
camera into the handoff rather than repeating the same boot.

The handoff writes `boot-trial` before it stops the OEM application and
clears it once jooanipc-open has run healthy for 185 s. A latched `boot-trial`
makes later handoff boots leave the OEM application running (rescue). The
handoff also waits until the platform has applied the committed Wi-Fi network
and a gateway on `wlan0` answers. Without a committed network and working
Wi-Fi, a native boot fails, the handoff gives up, and the camera ends in
rescue mode.

## Current implementation boundary

The implementation includes deterministic signed packaging, a compressed
controller core plus one compressed runtime and separate SSH recovery, telnet
and GoAhead suppression, transactional Wi-Fi, password-synchronized Dropbear
with optional keys, the HTTP daemon with a maintenance Web UI, a local TLS
MQTT sink, optional SNTP network time, embedded DNS-SD, and an exact-binary
`LD_PRELOAD` guard for the OEM application. Where that application runs, the
guard resolves only loopback names and the two approved OEM cloud names (to
loopback aliases), redirects its MQTT to the local sink and its outbound
port-443 connections to a loopback stub, denies all other non-loopback
traffic, rewrites every listener bind to loopback (its RTSP from TCP/554 to
`127.0.0.1:8554`, its ONVIF to `127.0.0.1:8899`), and keeps its speaker
muted. See [the guard](../src/guard/README.md).

Viewing, recording and steering moved to the NVR (Frigate over RTSP and ONVIF)
in 0.2.14. The camera no longer remuxes video for browsers or routes audio,
which also retired the per-viewer fMP4 buffers and the audio router process: on
a 38 MB board that memory was part of what the OOM killer fought over. The
router's sources and host tests remain in `src/audio/`; it is not built for
the camera or shipped. Audio now comes from the media daemon itself: with
`audio=1` it puts the microphone on its RTSP URLs and plays talk audio that
arrives over the RTSP backchannel or its HTTPS API (see
[Audio and talk](../media/README.md#audio-and-talk)).

Since 0.2.22 `joan-daemon` is the platform only. The Digest RTSP proxy on
TCP/554, the ONVIF subset on port 80 and the optional OEM audio pause were
removed; the guard's audio-pause and talkback hooks remain, but no shipped
component sends to them. Since 0.2.23 the media daemon boots natively, as
described above.

The board has no RTC, so boot restores a coarse saved time that lags by the
downtime. When enabled (it is off by default), the SNTP client in `joan-daemon`
keeps the clock on UTC from the gateway or another local server and steps it
through the integration helper's `time-set`, which also saves the time the
next boot restores. It never steps while an OEM `jooanipc` process exists:
a step during that bootstrap killed it. In rescue and platform-only modes that
process runs for the whole uptime, so network time never steps the clock
there. See [API](API.md#camera-time).

In steady state, the compressed controller core, SSH recovery bundle, and one
runtime must total no more than 200704 bytes (196 KiB), with at least 65536 bytes
(64 KiB) free. Updates are maintenance transactions, not A/B coexistence: after
the controller and recovery path are durable, selection becomes `- - 0`, the old
persistent runtime is removed, and one signed candidate is staged while at least
57344 bytes (56 KiB) remain free. This exceeds the OEM 50 KiB cleanup threshold.
The trial applies the committed Wi-Fi network while it runs and restarts a
candidate whose first start failed; one that still fails
stays as the only runtime (nothing older exists to restore) and the supervisor
keeps restarting it, with controller-owned SSH for recovery. Successful health
promotion selects the candidate and restores the 64 KiB steady reserve. The candidate is healthy
when `joan-daemon` runs and listens on port 80; if the OEM application is
running (fallback modes only), it must also have the containment guard mapped.
Before 0.2.24 health also required the OEM application, so no platform update
could pass its trial on a native boot.

No kernel-wide firewall is claimed. The supervisor repeatedly removes IPv4 and
IPv6 default routes and filters public resolvers; explicitly configured
RFC1918/ULA and connected routes remain. DHCP, SSH and the open daemons are
separate processes, while the router/VLAN remains the hard device-wide boundary.

## Feature coverage

The user-facing comparison with the vendor firmware is in
[OEM comparison](OEM-COMPARISON.md). This table is the engineering view.

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
| Cruise / patrol | ✓ preset tour with a time at each stop, pausing after someone steers; no schedule of its own |
| Auto-tracking | → by the NVR, through ONVIF field-of-view moves and `MoveStatus` |
| Privacy / PTZ-hide schedule | ✓ both lenses black, microphone muted, head turned away; on demand or on a daily schedule |
| **Audio** | |
| Listen (camera microphone) | → PCMA on the main RTSP URLs with `audio=1` |
| Two-way talk | → ONVIF RTSP backchannel, the admin UI and `POST /api/v1/talk` with `audio=1` |
| Speaker volume | → `volume` control with `audio=1` |
| Custom voice / alert clips | ✗ |
| **Light & night vision** | |
| Day / Night / IR-cut mode | → day, night, automatic (ISP exposure, hysteresis) or schedule; infrared LEDs on or off at night; remembered |
| Floodlight / white light (+ schedule) | → manual, schedule, or a timed trigger (for Home Assistant on an NVR event); remembered |
| **Detection & alarms** | |
| Motion detection (+ sensitivity, zones) | ✗ |
| Human / person detection (+ area) | ✗ |
| **Storage (microSD)** | |
| Scheduled / continuous recording | → ~ rotating local files; off in the qualified profile, which runs without an SD card |
| Playback | → ~ authenticated download of local recordings |
| Card format / capacity | ✗ |
| **Time** | |
| Set clock (from phone / UTC) | ✓ manually, or by optional SNTP from a local server |
| Time zone | ✓ stored in the OEM configuration; scheduled restarts and the media daemon's schedules follow it (the OEM application's OSD also uses it, on its next start) |
| **Network** | |
| Wi-Fi credentials | ✓ transactional stage / commit / roll back; prefers 5 GHz, falls back to 2.4 GHz when 5 GHz is weak/absent |
| Wi-Fi scan (nearby APs) | ✓ `/api/v1/network/wifi/scan` and in the admin UI |
| Signal / RSSI readout | ✓ `/api/v1/network/quality` (signal, rate, band, address, gateway, counters) |
| Static IP | ✓ `/api/v1/network/ip`, through the 90-second Wi-Fi trial |
| **System** | |
| Firmware update | ✓ signed, sequence-gated (platform); the media layer is updated by its installer |
| Change password | ✓ (synchronized to SSH) |
| Reboot | ✓ button and API, refused during an update or a trial; optional daily restart at a set time |
| Reset to defaults | ✓ admin UI, API or a 5-second press of the reset button; keeps Wi-Fi |
| Device info / UID | ~ version and state in `/api/v1/status` |

Beyond the OEM app, the retrofit adds a local maintenance **Web UI**, **SSH**
(password-synchronized, optional Ed25519 keys), **mDNS** `.local` naming,
**local-only egress** (continuous default-route pruning), a **native boot**
that never starts the OEM application, exact-binary `jooanipc` **cloud/P2P
containment** for the fallback modes, and optional **network time** (SNTP).

## Boot and network sequencing

The OEM boot sequence can launch GoAhead before `/opt/etc/local.rc` runs. That
early interval is immutable without replacing earlier OEM boot components. The
runtime then kills GoAhead every second and never reopens it, but cannot
eliminate that immutable early interval. The external router or VLAN must
block untrusted peers and Internet access before power is applied.

In a native boot the listeners are TCP/80 (the platform API; its pages
redirect to the admin UI), TCP/22 (SSH,
password login from the start; keys are optional), UDP/5353 (mDNS), the
platform's two idle loopback listeners for the OEM application, and the media
daemon's TCP/443 (the admin UI), TCP/18443 and TCP/18554. The fallback modes add the OEM application's
loopback-only listeners. [Network security](NETWORK-SECURITY.md#listeners-by-boot-mode)
lists them per mode. No cloud or P2P path is part of the supported
architecture.

## Memory pressure and PTZ homing

The board has about 38 MB of RAM and runs with only a few megabytes free, so
the kernel OOM killer does fire. It takes the largest process, which used to
be the retained media process; the OEM watchdog then rebooted the camera and
flagged the reboot "silent". A silent boot skips PTZ homing and trusts the last
saved position, which the OEM saves only after manual jogs and never after a
preset recall, so a crash that followed a recall shifted the whole PTZ frame
and every preset with it.

When the OEM application runs, the loader wrapper in `local.rc` therefore
removes `/opt/silence_reboot` so every boot homes, and lowers that process's
`oom_score_adj`. In a native boot `native-boot.sh` lowers jooanipc-open's
instead, and the daemon homes once per boot. `start.sh` raises
`joan-daemon`'s, so the OOM killer takes the platform runtime first. When it
does, the supervisor restarts it (rate-limited, never while maintenance holds
the state lock) and writes the kernel's report to `jooan-local/` on the
microSD card when one is mounted, since `dmesg` does not survive a reboot. In
the fallback modes the OEM application reconnects to the local MQTT bridge
within about 20 seconds.

Heavy memory pressure can also stall the board before any OOM kill, until the
hardware watchdog resets it without a log. Installing an update while an NVR
pulled both streams has reproduced this; stop the NVR during an update.

`dmesg` does not survive a reboot, so the supervisor keeps two small files in
its state directory. `reset-history` gets one line per boot (time, the reset
address the kernel reports in `dmesg`, and the network watchdog's reboot count;
last 20 boots; the time is the restored clock, before network time corrects it).
`net-loss.log` gets the clock, uptime, Wi-Fi state and the tail
of `dmesg` when the gateway has been unreachable for 10 seconds, at most 3 times
per boot.

## Package format

The host tooling assembles the reviewed runtime and wraps it as an OEM IronMan update package.
The exact target manifest records `JA-A12`, package token `A12`, T23N, both
sensors, SKW6316, the `0x200001` payload limit, and the format's `0x60`-byte
header/trailer properties. Install and uninstall packages have separate staging
trees and hashes.

The carrier is only a delivery mechanism. The inner stage has a complete
SHA-256 inventory signed with deterministic ECDSA P-256/SHA-256 and verified
against a pinned public key on the camera. Release sequences reject downgrade
and replay. The included installer also performs exact model/retained-component
preflight, single-runtime maintenance staging, health validation, and SSH recovery.

The `0.1` migration is journaled. It recognizes the earlier manual-admin or
developer-launcher `/opt/open` trees, preserves validated SSH identity material
where available, publishes the compressed replacement, and retires the
recognized predecessor only after activation. Unknown or partial predecessor
trees fail closed. Expanded in-place controller layouts require their explicit
backed-up migration rather than silent deletion.

The media layer is not part of this package. Its release lives in the appfs
image that `media/scripts/install-camera.py` flashes; see
[camera release bundles](../media/docs/hardware.md#camera-release-bundles).

## Non-goals

This release does not claim:

- a new kernel or bootloader;
- an open CV2005 sensor/ISP implementation;
- removal or auditability of `jooanipc`;
- compatibility with every product sold as W3-U;
- safety on an Internet-facing network.
