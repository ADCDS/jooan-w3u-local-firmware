# JA A12 hardware integration

The hardware backend is implemented but has not been run on the physical
camera. The SDK contract tests replace function pointers with doubles; QEMU
tests execute the rebuilt MIPS application with file-backed media. Neither
establishes that the target can capture a fresh sensor frame or encode it.

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

The source build installs nothing on a camera. It performs no rootfs writes,
flashing, module reload or automatic termination of existing media processes.
The IMP backend refuses a concurrent OEM media owner. Begin physical tests from
volatile storage using the established recovery procedure; keep firmware and
factory artifacts outside this repository.
