# JA A12 hardware integration

Native sensor capture remains unqualified. The [OpenIMP physical trial](camera-openimp-test-20261017.json)
reached sensor/tuning and encoder setup, but failed at FrameSource REQBUFS with
ENOMEM. The missing OEM reserved-memory IVDC workspace handshake forced a
contiguous normal-RAM allocation that could not succeed. A separate secondary
AE-slot NULL dereference and teardown fault required a reboot.

The source now registers both physical sensors and avoids duplicate global ISP
startup. The guarded retry found no suitable contiguous block even after
compaction and stopped before ISP initialization. The next port requirement is
the IVDC workspace's setup and verified release lifecycle. This final retry
left OEM stopped under bounded watchdog custody, as requested; the trial
receipt records its reboot deadline.

In the [earlier replay run](camera-test-20261017.json), the physical T23N ran the
rebuilt daemon with file-backed media. Native IMP startup correctly refused the
active OEM owner before loading an SDK. The SDK contract tests replace calls
with doubles. Replay and SDK doubles do not establish native capture or encoding.

## Required runtime arrangement

The exact target is JOOAN JA-A12, Ingenic T23N, two CV2005/CV2005S1 sensors,
OEM Linux 3.10.14, reserved media memory, the OEM sensor/ISP/frame-channel/VPU
modules and existing per-device IQ/calibration files. Network interfaces must
already be configured by the retained operating system. This daemon does not
replace the kernel, Wi-Fi firmware or boot environment.

The example paths use an SD card, but its filesystem must support the program's
private files and atomic updates. The retained kernel supports VFAT, JFFS2 and
SquashFS; do not assume ext4 support. On VFAT, permissions come from mount
options: use an operator-controlled mount with `fmask=0177,dmask=0077` so files
appear as 0600 and directories as 0700, or choose a suitable private writable
filesystem. `chmod` alone does not establish private VFAT permissions. Keep
configuration, TLS keys and `state_dir` private. Discovery refuses identity
files with group/world permissions, so an unsuitable mount prevents startup.
The daemon never changes mount options. A volatile state directory loses its
discovery identity and PTZ presets at reboot.

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

The [OpenIMP OEM80 fork](https://github.com/ADCDS/openimp/tree/jooan/oem-t23-abi)
now has an explicit provider path. Its T23 public records match the recovered
FS84/encoder164/stream16/pack32 layout. Select it with all three variables:

```sh
export OPENIMP_T23_DRIVER_ABI=oem80
export OPENIMP_T23_ENCODER=native
export OPENIMP_T23_NATIVE_FALLBACK=0
```

Set `sensors=1` and `audio=0` for this bring-up path; this publishes the first
sensor's main/sub streams. The adapter registers both physical sensors because
the retained `direct_mode=2` ISP runs AE for both slots. Registering only the
first slot caused a kernel NULL dereference in the secondary AE path. Other
OpenIMP publication configurations are rejected while dual-sensor IVDC and
audio remain unqualified. Use an explicit `libimp` path and pin the built library using
`JIPC_LIBIMP_SHA256`. Recognition of the provider and matching public records
does not establish functioning sensor capture or hardware encoding. Startup
logs identify the next SDK operation before entering it, including operations
that can block inside the retained driver.

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

## Qualification still required

Before treating the hardware backend as a working replacement, independently
capture and decode native H.264 from both sensors, check frame freshness and
timing, exercise microphone/speaker and native JPEG, verify controls against
the real driver, and prove teardown restores the known OEM baseline. LAN
multicast discovery and a real NVR also require interoperability testing.

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
incompatible candidate, and no capture or physical PTZ/light/audio success is
claimed.

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
