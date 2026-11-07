# JA A12 hardware integration

Native dual-sensor capture and native encoding now work on the physical target
with the [OpenIMP OEM80 fork](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi).
The current `main_only=1` profile encodes the movable view at 1920×1080 and
the fixed view at 1024×576, configured at 15 fps; audio is optional (`audio=1`). All four
logical H.264 URLs decode: channels 1/3 share the corresponding main feed.
Physical pan,
tilt, deadman stop and repeated two-axis preset return passed. Frigate on NVR host
records the two main views and recognizes ONVIF pan/tilt and saved presets.
See the [1080p qualification receipt](camera-1080-test-20261018.json).
The [previous quality/control receipt](camera-quality-test-20261017.json)
records the four-encoder profile with paired 720p mains and 320×240 substreams.

The historical [IVDC receipt](camera-ivdc-test-20261017.json) records an initial
secondary AE fault. Replaying the OEM input-enumeration handshake initializes
the secondary runtime index and fixes that fault. Subsequent work keeps the
IVDC line-ring workspace on FS0 and uses ordinary full-frame outputs
for native encoding. Direct completions are never treated as complete NV12.
The matching GNU atomic runtime is also required; the old loader did not reject
an unresolved 64-bit atomic symbol, causing a userspace failure after first IDR.
The OpenIMP build now rejects undefined symbols and supplies `libatomic.so.1`.

Earlier [OpenIMP](camera-openimp-test-20261017.json) and
[replay](camera-test-20261017.json) receipts describe those earlier stages,
not the final native state. Uncertain DMA detachment still requires a verified
driver close or a kernel reset before reserved memory can be reused.

## Required runtime arrangement

The exact target is JOOAN JA-A12, Ingenic T23N, two CV2005/CV2005S1 sensors,
OEM Linux 3.10.14, reserved media memory, the OEM sensor/ISP/frame-channel/VPU
modules and existing per-device IQ/calibration files. Network interfaces must
already be configured: by the platform's `local.rc` and supervisor in a native
boot, or by the OEM application before a handoff. This daemon does not
replace the kernel, Wi-Fi firmware or boot environment.

Put public executable/library files in a release directory in the internal
appfs (the deployed layout) or on SD, and private files on JFFS2: the
qualified deployment uses `/opt/jooanipc-open` (0700) for the 0600 configuration,
TLS key and preset file. Its state directory is 0700. An SD card is VFAT with
public mount permissions; `chmod` there cannot make credentials private. The
small `/opt` partition must retain at least 64 KiB free; OEM startup can delete
most writable directories when it falls below 50 KiB. Store bulk backups and
logs off-camera. The daemon does not change mount options. Volatile state
would lose presets and discovery identity on reboot. Discovery remains off in
the tested deployment, which runs it next to the platform.

The retained boot environment must initialize the kernel CSPRNG before this
daemon generates credentials or serves TLS. MbedTLS uses `/dev/urandom` as the
fallback on kernels without `getrandom()`, including this Linux 3.10 target;
this applies to both its HTTPS and PSA random generators. This fixes the
observed blocking `/dev/random` reads. The on-device test used the already
running OEM system; early-boot entropy readiness remains a cold-boot
qualification requirement. MbedTLS pthread locking is enabled for concurrent
HTTPS workers and PSA state.

The original `jooanipc` statically contains its IMP implementation; there is no
reusable OEM `libimp.so` in the dump. Supply a separately obtained compatible
T23/Ace library. Similar version strings and exported symbols do not establish
ABI compatibility. The available generic T23 archive historically differed in
the VPU ioctl family and frame-buffer behavior. This program does not silently
translate an arbitrary library into the OEM ABI.

The adapter checks ELF32/little-endian/MIPS, required symbols, the SDK's version
and an optional operator-selected SHA-256. It guards returned records and waits
for actual encoded data before publishing readiness. These checks reject some
incompatibilities; they do not prove complete compatibility. A cleanup failure
poisons further hardware initialization in the same process and retains the SDK
mapping to avoid unloading code beneath potentially live SDK threads.
The reserved arena's allocation ledger is process-local. After uncertain DMA
detachment, restarting the consumer or unloading the library can reuse memory
still referenced by the kernel. Verify driver detachment or reset the camera
before another process uses reserved memory; a process restart alone is not
recovery.

The [OpenIMP OEM80 fork](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi)
now has an explicit provider path. Its T23 public records match the recovered
FS84/encoder164/stream16/pack32 layout. Select it with all three variables:

```sh
export OPENIMP_T23_DRIVER_ABI=oem80
export OPENIMP_T23_ENCODER=native
export OPENIMP_T23_NATIVE_FALLBACK=0
```

Set `sensors=2`, `fps=15` (or lower), `flip0=3` and `flip1=3` (and `audio=0` or `1`) for the tested
profile. With `main_only=0`, one published sensor is also permitted, but both
physical slots are registered and enumerated because `direct_mode=2` AE visits
both. The current `main_only=1` mode requires two published sensors. Other FPS
settings are explicitly rejected; `audio=1` is accepted. Both installed sensors need
horizontal and vertical flip for an upright scene. These bits also align manual
PTZ controls with the displayed image; stored presets remain raw coordinates.

Place the compiler-matched `libatomic.so.1` beside `libimp.so` and set
`LD_LIBRARY_PATH` to that directory. Pin the library with `JIPC_LIBIMP_SHA256`.
Startup logs identify each SDK operation before entering it. The client creates
an unbound FS0 workspace first, enables it before normal outputs, and releases
it after stopping dependent streams. In `main_only=1`, logical 0/1 share
FS1/group0/encoder0, and logical 2/3 share FS5/group1/encoder1. Only slots 0/2
own SDK resources, and each encoded frame is dequeued once. Frame publication,
IDR routing, metadata and watchdog freshness account for both aliases. Local
recording skips the duplicate feeds. Crop is disabled so the scaler shows
the complete scene.

The retained driver's frame-switch path reloads scaler geometry but omits the
shared DMA stride registers. Enabling FS1 and FS4 at different widths therefore
overwrites the same scaler's stride. The new graph uses scaler 1 for the movable
lens (FS1) and scaler 2 for the fixed lens (FS5), leaving FS2/4 unused. The earlier
asymmetric graph produced repeated image regions even though H.264 decoded;
the new graph's actual images were inspected as well as decoded. No kernel
patch or new OpenIMP library is required for this application-side workaround.

The qualified settings are `width=1920`, `height=1080`, `width1=1024`,
`height1=576`, `bitrate=3072`, `bitrate1=1536` and `gop_frames=15`. Reserved-memory
peak is 20,388 KiB of 21,988 KiB, or 20,596 KiB with the timestamp on, leaving
1,392 KiB. Both sensor drivers advertise 1920×1080 input; the fixed output is
scaled down from it, with the same framing at any size (a corner moved 0.5 px
between 640×360 and 1024×576). `fps` may be anything up to 15: on camera1, 10 and 5
gave exactly that many frames on both streams.

What each stream costs in reserved memory, measured: one NV12 frame in its pool, plus
about 222 KiB and 642 KiB per 640×360 worth of pixels in the encoder. So beside
1920×1080, a fixed view of 1280×720 would need 2,980 KiB more than 640×360 and
does not fit, and a real 640×360 substream on the pan/tilt lens would need about
1,200 KiB, which leaves under 200 KiB once the fixed view is 1024×576 and the
timestamp is on. Its URL keeps sharing the main feed: an NVR can scale that itself.

A release from before these limits refuses these values at its boot check, so a bundle
that rolls back past them must also set `width1=640`, `height1=360`, `bitrate1=768` and
`fps=15` in `config.camera_ini`. OEM evidence
shows 2304×1296 outputs with 1920×1080 sensor input, supporting ISP upscaling
for that OEM mode. The full-frame native encoder still cannot fit that 3 MP
profile with the fixed view in this reserved arena.

With `main_only=0`, logical 0/1 use FS1/2 and 2/3 use FS4/5. This older native
layout retains independent substreams and requires matching main dimensions
through 1280×720. The new mode requires two sensors and rejects a primary
larger than 1920×1080 or a fixed view larger than 1024×576. Independent substream
settings are unused. Other layouts need separate memory and image qualification.
To return to the earlier four-encoder profile, set `main_only=0`,
`width=1280`, `height=720`, `width1=0`, `height1=0`, `bitrate=2048`,
`bitrate1=1536`, `sub_width=320`, `sub_height=240` and `sub_bitrate=256`
together; changing only `main_only` leaves incompatible main dimensions.

## Recovered layouts

| Record | Bytes | Important facts |
| --- | --- | --- |
| Sensor registration | 84 | CV2005 address 0x35 and CV2005S1 address 0x36, both I2C adapter 0, separate sensor IDs |
| Frame-source attribute | 84 | 60-byte prefix, unknown word at 0x3c, extended crop through 0x50 |
| Encoder attribute | 164 | H.264/Smart configuration from the OEM path |
| Encoder pack | 32 | Target 32-bit addresses and timestamp |
| Encoder stream | 16 | Target pointer, pack count and sequence fields |
| Audio frame | 32 | Pointer and physical address, 64-bit timestamp, length |
| Audio input channel parameter | 12 | Buffer depth, AEC channel, reserved word (OpenIMP's T23 layout) |

Logical video streams 0/1 belong to sensor 0 and 2/3 to sensor 1. The recovered
independent-encoder mapping is 0/4/1/5.

A still uses encoder 2 or 3 on a group of that lens: its substream group in the
four-encoder layout, so the still has the substream's size, and in `main_only=1`
its only group, so the still has the main stream's size. The JPEG channel exists
only for the request. Its bitstream is borrowed from the shared H.264 area, so it
allocates no reserved memory, and the VPU takes the job between two H.264
pictures. Measured on camera1 on 2026-11-06, with Frigate connected and both
streams being read:

| | 1920×1080 | 640×360 |
|---|---|---|
| Size | about 370 KB | about 38 KB |
| Waiting for the encoder | 45-106 ms | 17-86 ms |
| Create, start, stop, destroy | 1-19 ms | 1-17 ms |
| Whole request with a new TLS connection | 1.2-1.8 s | 0.4-1.2 s |

A 1080p still every 2 s for 24 s left 347 and 358 frames on the two streams, with
350 and 357 and no stills, and a worst gap of 0.15 s. The control lock has to be
free during the wait: with it held, H.264 output is not drained, the wait reached
412 ms, and each such wait took 0.4 s of video from both lenses.

Audio runs on two threads of its own, so neither can stall video. The
microphone thread is the only `IMP_AI` caller: it opens capture at 16 kHz
mono with 40 ms periods after video is ready, polls with a 100 ms timeout,
averages pairs of samples down to 8 kHz and encodes PCMA. Timestamps come
from the sample count, not from dequeue time, so they step exactly 40 ms; a
frame more than 150 ms late re-anchors forward (a gap), and frames that would
run more than 150 ms ahead are dropped, so time never repeats. Silence fills
gaps longer than 120 ms. After 5 s without audio the thread closes capture
and retries with a backoff of 10 s doubling to 300 s; an ABI mismatch turns
off only the microphone. The speaker thread is the only `IMP_AO` caller: one
320-sample frame (40 ms, from 8 kHz PCMA) per blocking send. It never calls
`IMP_AO_FlushChnBuf` or `IMP_AO_ClearChnBuf`, which wait on the driver
without a bound on this kernel.

`/dev/dsp` belongs to the daemon: opening it from a shell while the daemon
runs takes the device away from the SDK and stops video.

The earlier research's 60-byte frame-source declaration was incomplete. This
project uses 84 bytes and compile-time assertions, with evidence derived from
the OEM getter/setter/creator copies and field dump. `include/imp_abi.h`
records exact target layouts.

## Qualification boundaries

Both real sensor views through all four H.264 URLs, physical PTZ/presets, clean
native shutdown/restart and Frigate recording/ONVIF interoperability have been
exercised. Audio (microphone, talk over the RTSP backchannel and HTTP, the
boot gate and the mutes) passed on the camera on 2026-11-05
([receipt](camera-audio-test-20261105.json)); the long soak with audio on is
still running. Light/IR-cut
behavior, arbitrary resolutions, long-duration soak and cold power-cycle
recovery are not qualified by this run. The broad `hardware_validated` status
field remains false because that complete hardware feature matrix is pending;
`ready` and actual stream counters report current capture state.

The 1080p control check observed 100 motor steps on the first nudge and 193 on
the repeat. Both were single continuous runs with no delayed second movement;
the saved-position recalls passed. The first-use variation is recorded for
follow-up; its cause is not established, and this release does not change motor
code. A 350 ms command should not be read as a guarantee of identical travel.

The earlier 2026-10-17 replay run passed 73 native core checks, offline SDK contracts, six RTSP
and eight HTTP/ONVIF protocol tests, local-subnet discovery probes, TCP/UDP
FFmpeg replay decoding, RAM recording/download, controls in replay, and
credential reload. The process sampled 1776 KiB RSS and 1832 KiB high-water RSS.
Its temporary files were removed; OEM PID 393 and the local controller stayed
running. The camera clock was approximately 68340 seconds behind the client;
WSSE tests compensated client-side without changing the camera clock.

The earlier generic external library, SHA-256
`bd904ec8f4123965ded8c0c57c25ad5c3ae0fdc17aa4a6374278a7c822cb31d9`,
uses the 88-byte `0xc05863xx` VPU ioctl family. The retained OEM driver implements
the 80-byte `0xc05063xx` family. Stopping the OEM application does not resolve
this ABI mismatch. No SDK initialization was attempted with that known
incompatible candidate; later capture/PTZ results use the source-built OEM80 fork.

Remote protocol checks accept `test_rtsp.py --host ADDRESS --local-bind 0.0.0.0`
and `test_http_onvif.py --clock-offset SECONDS` for a camera whose clock differs
from the client. The latter is the camera's UTC clock minus the client's.
`build/mips/udp-probe ADDRESS PORT` exchanges one bounded datagram over
stdin/stdout for discovery checks through SSH when the client is outside the
camera's local subnet. It accesses no camera devices and exits 2 on timeout.

The source build installs nothing on a camera. It performs no rootfs writes,
flashing, module reload or automatic termination of existing media processes.
The IMP backend refuses a concurrent OEM media owner. Begin physical tests from
volatile storage using the established recovery procedure; keep firmware and
factory artifacts outside this repository.

## Guarded boot handoff (fallback)

`scripts/oem-boot-handoff.sh`, installed as `/opt/jooanipc-open/boot-handoff.sh`
(0700), is the fallback boot path. It is not installed or activated by the
normal build. Since platform 0.2.23 the platform's `/opt/etc/local.rc` owns the
hook: with `/opt/jooanipc-open/enabled` present it runs the
[native boot](#native-boot) when `native` is set, and this script instead when
`native` is absent or `native-failed` is latched. Do not add a hook to `local.rc` by hand; a platform update replaces
that file. In this path the guarded OEM application still performs Wi-Fi
setup, optional SD mounting and motor homing; the helper replaces media
ownership after that bootstrap, rather than replacing the entire boot
environment.

The root-owned 0600 `deployment.env` specifies `JIPC_RELEASE` (one versioned
directory under `/mnt/sd_card/jooanipc-open/releases/` or
`/mnt/mtd/jooanipc-open/releases/`), `JIPC_LIBIMP_SHA256`, and `JIPC_APP_MD5`, `JIPC_IMP_MD5`,
`JIPC_ATOMIC_MD5` for the three release files. Optional `JIPC_WIFI_BSSID` plus `JIPC_WIFI_FREQ` (its proven frequency in MHz)
select a proven AP for this camera: after the platform applies its
committed network, the helper pins that existing network and waits for a stable
association to that BSSID. Frequency is applied before selection so an existing
5GHz network block cannot prevent a chosen 2.4GHz AP from being reached. An
explicit reassociation is required when the selected network was already active.
It changes no Wi-Fi credentials or AP/router
configuration. Omit it to keep ordinary AP selection. MD5 checks detect deployment
corruption; they are not a signature or adversarial integrity guarantee. The
client additionally verifies the library's pinned SHA-256. Keep release access
limited to trusted administrators. The private camera config must use that
release's library and `watchdog=1`. This setting is local-only and defaults off;
the daemon never terminates other owners itself. Changing `watchdog` on a
configuration reload is rejected with an explicit process-restart requirement;
closing an owned FD leaves the hardware timer armed.

Create the 0600 `enabled` file only after checking configuration and shell syntax.
The helper requires a boot-specific syslog marker followed by the complete
successful `motor_step_reset` sequence: stages 418, 427, 449 (idle), 459 and
555 (idle), with no reset error or return timeout. It also requires a committed,
stable platform runtime that has applied the committed Wi-Fi network, live
SSH, a stable association with a gateway on `wlan0`, the release's mounted
filesystem and adequate JFFS2 space. Internal
releases require the canonical path on the read-only
`/dev/mtdblock4` SquashFS mounted at `/mnt/mtd`; path aliases and shadowing mounts
are rejected. An internal release does not require an SD card.
It then persists `boot-trial`, stops identity-checked OEM watchdog
and media processes, and checks every task's descriptors, IVDC address zero
and absence of Linux TAINT_DIE. It launches the replacement with the explicit
OEM80/native/no-fallback flags. The new process owns `/dev/watchdog`, sets its
OEM 180-second timeout and feeds it only while all four streams remain fresh.
After stopping OEM media, a bounded 45-second network recheck catches late
bootstrap network changes before native ownership. After 185 seconds of
process/watchdog health, final platform/network readiness
is retried for up to 60 attempts while native health remains mandatory; then the
helper clears `boot-trial`. A single lost ARP response does not end the trial.

A failure before transfer leaves OEM running if it is still alive (OEM rescue
for that boot). A failure after transfer retains
the trial latch and the armed watchdog provides reset recovery. Later handoff
boots then keep guarded OEM rescue instead of repeatedly attempting the
handoff; the latch does not stop a native boot when `native` is set and
`native-failed` is not.
Inspect `/tmp/jipc-handoff.log`, `/tmp/jooanipc-open.log`, kernel health and all
thread FDs before removing a failed trial latch. Removing `enabled` disables
the media layer altogether (platform-only mode: the guarded OEM application
runs for the whole uptime); do not revive another media owner on uncertain
DMA state.

### Internal-flash packaging

The JA-A12 has 8 MiB SPI NOR, with a 3136 KiB application partition and a
384 KiB writable configuration partition. Install executable releases in a
rebuilt application image; `/opt` is reserved for configuration, keys, presets
and platform state. Preserve at least 64 KiB free there. OEM `startapp`
deletes most of `/opt` if free space falls below 50 KiB at boot.

The retained OEM executable must remain byte-identical because its containment
guard verifies its hash. Its linked libraries, boot scripts, ISP/motor modules,
and this board's CV2005/CV2005S1 sensor drivers and tuning remain necessary.
Preserve `sensor_cv2005s1false.ko` for the secondary-detection fallback.
The qualified board-specific image uses 1 MiB XZ blocks, without voice prompts
or unrelated SC sensor files, occupies 3092 KiB including the native runtime,
and leaves 44 KiB in the application partition. A physical power cycle without
SD passed the complete guarded handoff and native watchdog trial. Both Frigate
views produced clean 15 fps recordings. See the
[internal-flash receipt](camera-internal-flash-test-20261020.json).
Such pruning is specific to this verified board. Firmware images and factory
data remain private artifacts.

For Frigate-only recording, retain `recording=0` and set `record_dir` to a
writable volatile path such as `/run/jooanipc-open/recordings`. The recorder
initializes its directory even when recording is disabled. Update both the
private configuration's `libimp` path and `deployment.env` when moving a release;
do not write video into the small NOR configuration partition.

The board has no RTC. Preserve the platform's `last-time` mechanism.
For Frigate set `onvif.ignore_time_mismatch: true`; its client reads the camera's
public UTC and offsets WSSE timestamps while retaining PasswordDigest. Use
`tls_insecure: false` on this Frigate version: its option also changes token
encoding. Configure ONVIF host as HTTPS and the actual 18443 port. Native RTSP
on 18554 uses ch0 for the movable view and ch2 for the second lens. Frigate's
recording timeline uses the NVR host's clock.

Frigate measures that offset once, when its ONVIF session starts. The restored
`last-time` lags real time by the camera's downtime plus the save interval, so
after a camera reboot every WSSE timestamp falls outside the 300-second window:
PTZ and presets fail with `'NoneType' object has no attribute 'getroottree'`
until Frigate restarts. Keep the camera on real UTC with SNTP from a server on
its own network, normally the gateway; that needs inbound UDP 123 on the
gateway from the camera's network and nothing else. The platform owns the
clock from its release 0.2.22: set its network time to
`gateway` and leave `ntp_server` empty here, so one process steps the clock.
Its steps also persist `last-time`, so the next boot starts close to real time.
The platform never steps the clock while the OEM application runs, so in a
handoff boot its first step comes after the handoff.

`ntp_server` is for deployments without the platform. The client retries
every 30 seconds until it synchronizes, then polls hourly, and steps the clock
when the offset reaches one second. Replies must echo a random origin cookie;
it is otherwise unauthenticated SNTP. `GET /api/v1/status` reports `clock`.

That ONVIF offset is cached by the tested Frigate client. In the 1080p reboot
test, the camera clock moved backward by about 967 seconds; video recovered
automatically, but Frigate's old WSSE timestamps were rejected before any motor
command ran. Restarting Frigate refreshes its ONVIF clock offset and profile
metadata. A later camera reboot with another clock jump may require the same
refresh. The daemon continues to enforce authentication and timestamp checks.

The deployed Frigate inputs use `preset-rtsp-restream` against go2rtc on
`127.0.0.1:8554`: each lens has one camera producer shared by recording,
detection and live clients. This avoids duplicate camera traffic on Wi-Fi.
An earlier test improved an initially weak connection by selecting a stronger
AP. Subsequent reboot tests exposed a different problem: forcing the working
5 GHz association onto that 2.4 GHz AP was followed by lost IP access. The
current deployment leaves `JIPC_WIFI_BSSID` and `JIPC_WIFI_FREQ` empty, preserving
the platform's association. Recovery and native qualification
succeeded on 5 GHz. Do not reinstate the old AP pin without testing its boot
behavior; a healthy running association does not establish safe cross-band
reassociation during bootstrap.

The Frigate CPU detector can temporarily fill its two-frame processing queue
during full-scene motion. Observed processing drops recovered to the configured
5 fps; live capture/recording remained available. SHM had more than 115 MiB free,
so its sizing recommendation did not indicate an exhausted frame store.

## Native boot

Native boot is the default and the deployed mode. With platform 0.2.23 or
later, `/opt/jooanipc-open/native` (next to `enabled`) selects a boot in which
the OEM application never starts. The platform's `local.rc` turns
`startapp`'s `jooanipc` and `my_watch_dog` into no-ops, feeds the hardware
watchdog with the rootfs `watchdog`, starts `wpa_supplicant` with an empty
network list and a persistent `udhcpc`, and runs `scripts/native-boot.sh`
installed as `/opt/jooanipc-open/native-boot.sh`. The platform supervisor then
applies the committed Wi-Fi network (`/opt/custom/jooan-local/config/wifi.json`);
the OEM's stored network is not used, so native boot requires a committed one.

`native-boot.sh` verifies the release as the handoff does, waits up to 300 s
for a gateway on `wlan0`, stops the feeder, launches the daemon (which
requires `motor_home=1`, homes pan/tilt and arms its own watchdog) and
qualifies once the daemon has stayed healthy for 185 s, one watchdog period,
with homing complete and the network still up. Its stages are kept in
`/opt/jooanipc-open/native-trial` until qualification. A boot that finds that
file renames it `native-failed` (keeping the stage) and boots the guarded
handoff instead, as does any boot without `native`. Nothing clears
`native-failed` automatically; delete it to retry. If nothing takes over the
feeder within ten minutes it is killed without the magic close, so a hung
native boot resets into that fallback. If the boot lock is already held by
another boot script, `native-boot.sh` records `failed: boot lock held by
another boot script` in `native-trial` and exits, so that reset latches
`native-failed` instead of repeating the same boot.

`/tmp/jipc-native.log` records the boot; it ends with "native boot qualified;
OEM application never started" on success. The containment guard is
installed but unused in this mode.

## Camera release bundles

`scripts/install-camera.py` installs both layers of a JA-A12 deployment from
one bundle directory: a signed platform package, applied through its
authenticated update API, and a rebuilt appfs image carrying one jooanipc-open
release, flashed on the camera by `scripts/upgrade-appfs.sh`. Each layer
reboots the camera; the installer waits for the media daemon to qualify again
(native or handoff boot) before continuing. File-level configuration changes
are staged first, so a bundle with one layer needs one reboot. Every touched
file is copied to `BUNDLE/backups/` beforehand and restored if a step fails
before the first reboot. Without `--yes` only the host and camera preflight
runs. Give `--camera` the camera's IP address or its mDNS label as a name
(`camera1.local`, `camera1.lan`, ...): the platform API answers
`403 host_rejected` to any other host name.

Preflight refuses a camera that is not the qualified flash layout, has less
than 64 KiB free on `/opt`, has a handoff `boot-trial` latched, has
`native-failed` latched (it shows the recorded stage), did not qualify the
media daemon on the current boot, already runs the bundle's platform sequence
or newer, or has an appfs other than the one the bundle expects. It also
refuses a bundle whose result would be a native boot without a committed Wi-Fi
network, since that boot fails after 300 s and latches `native-failed`.

The installer reads the media daemon's own certificate
(`/opt/jooanipc-open/cert.pem`) over SSH, which authenticates the camera by its
host key, and pins it for the HTTPS status checks on 18443 instead of
disabling verification. The host name is not checked: the certificate is
self-signed and the camera is addressed by IP.

`bundle.json` (schema 1) names the artifacts and their hashes:

```json
{
  "schema": 1, "name": "camera-YYYYMMDD-N", "board": "JA-A12",
  "jooan_local": {"release_version": "0.2.24", "release_sequence": 58,
                  "package": "JOOAN_FW_PKG", "package_sha256": "…"},
  "jooanipc_open": {"release": "<app sha256 prefix>-<libimp sha256 prefix>",
                    "image": "appfs.img", "image_md5": "…",
                    "expected_current_appfs_md5": "…", "app_md5": "…",
                    "imp_md5": "… (only when libimp changes)", "imp_sha256": "…",
                    "installer": "upgrade-appfs.sh", "installer_md5": "…"},
  "config": {"camera_ini": {"set": {}, "unset": ["ntp_server"]},
             "files": {"/opt/custom/jooan-local/config/ntp-server": "gateway\n",
                       "/opt/jooanipc-open/state/presets.v1": null},
             "verify_ntp": true}
}
```

Either layer may be omitted. `files` maps camera paths to new contents, to
`{"from": "<file in the bundle>", "mode": "700"}`, or to `null` for deletion.
With a media layer, `camera_ini` changes go into the staged configuration the
appfs installer commits, so new keys meet the binary that accepts them. Bundles contain vendor-derived appfs images and backups
holding the camera password, so keep them private.

The appfs installer upgrades only a native boot: it refuses to run while the
OEM application or its watchdog is running. It takes the platform maintenance
lock (`/run/jooan-local/state.lock`), which pauses the supervisor's runtime
restarts and network-watchdog reboot, stops the native media owner, hands the
armed watchdog to a rootfs feeder, keeps a RAM copy of the current partition,
and verifies the flash by full readback. Any failure before the flash commit
point reboots into the unchanged partition. After it, a failed flash is rolled
back from the RAM copy, retried up to three times; if the rollback still
fails, the camera stays up for manual recovery with both images in `/tmp`
(see [appfs reflash recovery](../../docs/RECOVERY.md#appfs-reflash-recovery)).
Power loss during the flash leaves no `startapp` and needs an SPI programmer.
Stop the NVR during a platform update (`--nvr-stop`/`--nvr-start`): memory
pressure from both streams has stalled the board during that transaction.

A bundle from `tools/build-appfs.sh --first` (`"first": true`) instead installs
the first release on a camera with only the platform, with
`scripts/first-appfs.sh`. It has not yet run on a camera; see
[The media layer](../../docs/INSTALL.md#the-media-layer).

The admin UI can also install a release, from a package made by
`tools/make-media-update.py`. The daemon verifies its signature with the platform's
verifier, then has the signed `scripts/apply-media-update.sh` check the camera
and run the signed `scripts/upgrade-appfs.sh`. Nothing is flashed until the owner
confirms, and a package not confirmed in 15 minutes is forgotten. On camera1 an
install with Frigate connected took 4 s to upload and check, video kept its 15 fps
meanwhile, and the page was back about 90 s after confirming.
