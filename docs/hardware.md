# JA A12 hardware integration

Native dual-sensor capture and native encoding now work on the physical target
with the [OpenIMP OEM80 fork](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi).
Four H.264 channels decoded with FFmpeg: both main views at 1280×720 and both
substreams at 320×240, configured at 15 fps with audio disabled. Physical pan,
tilt, deadman stop and repeated two-axis preset return passed. Frigate on NVR host
records the two main views and recognizes ONVIF pan/tilt and saved presets.
See the [quality/control qualification receipt](camera-quality-test-20261017.json).

The historical [IVDC receipt](camera-ivdc-test-20261017.json) records an initial
secondary AE fault. Replaying the OEM input-enumeration handshake initializes
the secondary runtime index and fixes that fault. Subsequent work keeps the
IVDC line-ring workspace on FS0 and uses ordinary full-frame outputs FS1/2/4/5
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
already be configured by the retained operating system. This daemon does not
replace the kernel, Wi-Fi firmware or boot environment.

Put public executable/library files on SD and private files on JFFS2: the
qualified deployment uses `/opt/jooanipc-open` (0700) for the 0600 configuration,
TLS key and preset file. Its state directory is 0700. The SD card is VFAT with
public mount permissions; `chmod` there cannot make credentials private. The
small `/opt` partition must retain at least 64 KiB free; OEM startup can delete
most writable directories when it falls below 50 KiB. Store bulk backups and
logs off-camera. The daemon does not change mount options. Volatile state
would lose presets and discovery identity on reboot. Discovery remains off in
the tested deployment to coexist with the retained local controller.

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

Set `sensors=2`, `fps=15`, `audio=0`, `flip0=3` and `flip1=3` for the tested
profile. One published sensor is also permitted, but both physical slots are
registered and enumerated because `direct_mode=2` AE visits both. Other FPS
settings and OpenIMP audio are explicitly rejected. Both installed sensors need
horizontal and vertical flip for an upright scene. These bits also align manual
PTZ controls with the displayed image; stored presets remain raw coordinates.

Place the compiler-matched `libatomic.so.1` beside `libimp.so` and set
`LD_LIBRARY_PATH` to that directory. Pin the library with `JIPC_LIBIMP_SHA256`.
Startup logs identify each SDK operation before entering it. The client creates
an unbound FS0 workspace first, enables it before normal outputs, and releases
it after stopping dependent streams. Main/sub logical channels 0/1 use FS1/2;
logical 2/3 use FS4/5. Crop is disabled so the scaler shows the complete scene.
High-resolution profiles beyond the tested dimensions need separate reserved
memory/performance qualification.

## Recovered layouts

| Record | Bytes | Important facts |
| --- | --- | --- |
| Sensor registration | 84 | CV2005 address 0x35 and CV2005S1 address 0x36, both I2C adapter 0, separate sensor IDs |
| Frame-source attribute | 84 | 60-byte prefix, unknown word at 0x3c, extended crop through 0x50 |
| Encoder attribute | 164 | H.264/Smart configuration from the OEM path |
| Encoder pack | 32 | Target 32-bit addresses and timestamp |
| Encoder stream | 16 | Target pointer, pack count and sequence fields |
| Audio frame | 32 | Pointer and physical address, 64-bit timestamp, length |

Logical video streams 0/1 belong to sensor 0 and 2/3 to sensor 1. The recovered
encoder-channel mapping is 0/4/1/5. JPEG uses the sensor's substream group, so
snapshot resolution is the configured substream resolution. Native audio
capture converts 16 kHz PCM into 8 kHz PCMA. Speaker input is 8 kHz PCMA; the backend
assembles the SDK's 40 ms PCM blocks and enables GPIO63 with its settling delay.

The earlier research's 60-byte frame-source declaration was incomplete. This
project uses 84 bytes and compile-time assertions, with evidence derived from
the OEM getter/setter/creator copies and field dump. `include/imp_abi.h`
records exact target layouts.

## Qualification boundaries

Four native H.264 channels, both real sensor views, physical PTZ/presets, clean
native shutdown/restart and Frigate recording/ONVIF interoperability have been
exercised. Audio is disabled. Native JPEG, microphone/speaker, light/IR-cut
behavior, arbitrary resolutions, long-duration soak and cold power-cycle
recovery are not qualified by this run. The broad `hardware_validated` status
field remains false because that complete hardware feature matrix is pending;
`ready` and actual stream counters report current capture state.

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

## Optional guarded boot handoff

`scripts/oem-boot-handoff.sh` is an opt-in JA-A12 deployment helper. It is not
installed or activated by the normal build. Keep the existing guarded
`/opt/etc/local.rc`, network/SSH controller, kernel/modules and factory data.
The OEM application still performs Wi-Fi setup, SD mounting and motor homing;
the helper replaces media ownership after that bootstrap, rather than replacing
the entire boot environment.

Install the reviewed script at `/opt/jooanipc-open/boot-handoff.sh` (0700) and
append this additive hook to the existing local.rc, keeping an exact backup:

```sh
if [ -f /opt/jooanipc-open/enabled ] && [ -r /opt/jooanipc-open/boot-handoff.sh ]; then
    /bin/sh /opt/jooanipc-open/boot-handoff.sh </dev/null >/tmp/jipc-handoff.log 2>&1 &
fi
```

The root-owned 0600 `deployment.env` specifies `JIPC_RELEASE` (the versioned
SD release directory), `JIPC_LIBIMP_SHA256`, and `JIPC_APP_MD5`, `JIPC_IMP_MD5`,
`JIPC_ATOMIC_MD5` for the three release files. Optional `JIPC_WIFI_BSSID` plus `JIPC_WIFI_FREQ` (its proven frequency in MHz)
select a proven AP for this camera: after the retained controller applies its
committed network, the helper pins that existing network and waits for a stable
association to that BSSID. Frequency is applied before selection so an existing
5GHz network block cannot prevent a chosen 2.4GHz AP from being reached. An
explicit reassociation is required when the selected network was already active.
It changes no Wi-Fi credentials or AP/router
configuration. Omit it to keep ordinary AP selection. MD5 checks detect deployment
corruption; they are not a signature or adversarial integrity guarantee. The
client additionally verifies the library's pinned SHA-256. Keep SD access
limited to trusted administrators. The private camera config must use that
release's library and `watchdog=1`. This setting is local-only and defaults off;
the daemon never terminates other owners itself. Changing `watchdog` on a
configuration reload is rejected with an explicit process-restart requirement;
closing an owned FD leaves the hardware timer armed.

Create the 0600 `enabled` file only after checking configuration and shell syntax.
The helper requires a boot-specific syslog marker followed by `motor reset
finish`, a committed/stable retained controller, live SSH, SD mount and adequate
JFFS2 space. It then persists `boot-trial`, stops identity-checked OEM watchdog
and media processes, and checks every task's descriptors, IVDC address zero
and absence of Linux TAINT_DIE. It launches the replacement with the explicit
OEM80/native/no-fallback flags. The new process owns `/dev/watchdog`, sets its
OEM 180-second timeout and feeds it only while all four streams remain fresh.
After stopping OEM media, a bounded45-second network recheck catches late
bootstrap network changes before native ownership. After185 seconds of
process/watchdog health, final controller/network readiness
is retried for up to60 attempts while native health remains mandatory; then the
helper clears `boot-trial`. A single lost ARP response does not end the trial.

The installed OEM wrapper also redirects its bootstrap stdout to the private
`/tmp/jooan-oem-bootstrap.log` for diagnosis. Preserve the guarded preload and
existing library search path when adding that redirection.

A failure before transfer leaves OEM running if it is still alive. A failure after transfer retains
the trial latch and the armed watchdog provides reset recovery. The next boot
keeps guarded OEM rescue instead of repeatedly attempting native startup.
Inspect `/tmp/jipc-handoff.log`, `/tmp/jooanipc-open.log`, kernel health and all
thread FDs before removing a failed trial latch. Removing `enabled` disables
future handoff; do not revive another media owner on uncertain DMA state.

The board has no RTC. Preserve the retained controller's `last-time` mechanism.
For Frigate set `onvif.ignore_time_mismatch: true`; its client reads the camera's
public UTC and offsets WSSE timestamps while retaining PasswordDigest. Use
`tls_insecure: false` on this Frigate version: its option also changes token
encoding. Configure ONVIF host as HTTPS and the actual 18443 port. Native RTSP
on 18554 uses ch0 for the movable view and ch2 for the second lens. Frigate's
recording timeline uses the NVR host's clock. No camera egress or router changes
are required for this arrangement.

The deployed Frigate inputs use `preset-rtsp-restream` against go2rtc on
`127.0.0.1:8554`: each lens has one camera producer shared by recording,
detection and live clients. This avoids duplicate camera traffic on Wi-Fi.
A later native test ran into poor service on the initially selected AP;
reassociation to the stronger gateway AP restored both NVR views at5fps with
zero skipped frames. Media encoding and kernel health remained good during
that network incident. The AP choice is kept in the private deployment manifest.
