# Jooanipc Open

> This directory was the standalone `jooanipc-open` repository until
> 2026-11-05. It now lives in `media/` next to the
> [platform](../README.md) it runs on. Build, tests and license (MIT) are
> unchanged; run the commands below from `media/`.

A buildable local camera daemon for the JOOAN W3-U JA-A12. It implements local
streaming, administration, camera controls and recording, with a workstation
replay backend and an external-IMP hardware backend for the retained OEM Linux
kernel and drivers. It has no vendor cloud or TUTK/P2P client.

**Native dual-sensor capture works on the physical T23N with our OpenIMP
OEM80 fork:** the movable view runs at **1920×1080**, the fixed view at
**640×360**, both at 15 fps with audio disabled. `main_only=1` creates two
physical H.264 encoders; the four existing URLs remain available, with each
substream URL sharing its lens's main feed. Bitrates are 3072/768 kbps with a
one-second GOP. Physical pan/tilt and persistent named presets work, and
Frigate records both views. The new graph uses separate ISP scaler slots to
avoid the shared-stride corruption from the previous asymmetric attempt.
The original 2304×1296 output still exceeds the current full-frame memory
budget. See the [1080p receipt](docs/camera-1080-test-20261018.json) for artifacts
and qualification limits. The previous four-encoder mode remains available
with matching main dimensions through 1280×720 and independent substreams;
its [earlier receipt](docs/camera-quality-test-20261017.json) is retained.
The deployment preserves the platform's working Wi-Fi association instead
of forcing an AP switch. In the default native boot the OEM application never
starts: the [platform](../README.md)'s `local.rc` brings up the watchdog,
Wi-Fi and DHCP, and this daemon homes pan/tilt itself (`motor_home=1`) and
owns the hardware watchdog (`watchdog=1`). The guarded OEM handoff, in which
the OEM application brings up Wi-Fi and homes the motor before this daemon
takes over, remains the fallback; see [native boot](docs/hardware.md#native-boot)
and [boot modes](../docs/ARCHITECTURE.md#boot-modes). This is a local-services
replacement, with the feature boundaries below; full OEM feature parity is not
claimed.
The boot scripts support versioned releases in internal application
flash as well as SD storage. `scripts/install-camera.py` installs a release
bundle covering this daemon and the [platform](../README.md); see
[camera release bundles](docs/hardware.md#camera-release-bundles). Its appfs
installer reflashes the application partition. Internal releases run without an SD card; private
configuration and presets stay in the writable flash partition. See
[internal-flash packaging](docs/hardware.md#internal-flash-packaging) for the
JA-A12 storage budget and required boot dependencies.

## Implemented services

| Service | Implementation and validation |
| --- | --- |
| Video | Four H.264 URLs; two main encoders with shared aliases or four independent encoders; bounded rings, SPS/PPS cache, IDR recovery |
| RTSP | Digest authentication, TCP interleaving, UDP unicast, H.264 FU-A, PCMA, RTCP sender reports and BYE |
| HTTPS and REST | MbedTLS 3.6.7, Basic authentication over TLS, bounded requests, private atomic configuration |
| Failed-login throttle | Per peer address across HTTP Basic, ONVIF WS-Security and RTSP Digest: 10 failures within 60 s of the first block that address for the rest of the window (HTTP `429`, RTSP `503` with `Retry-After: 60`); requests without credentials never count |
| ONVIF subset | Device/Media/PTZ queries and control, WSSE PasswordDigest, replay protection, authenticated stream/snapshot URLs |
| Discovery | IPv4 WS-Discovery Probe/Resolve, stable identity, response limits and interface-local discovery |
| Recording | Rotating H.264/PCMA files, metadata, storage retention, IDR recovery after drops, authenticated download |
| Clock | Optional SNTP client for the RTC-less board: random origin cookie, clock steps of one second or more, status under `clock` |
| PTZ | Steady OEM-style 350 ms ONVIF nudges, 1.2-second REST lease, 16 named persisted presets; replay simulation |
| Lights | Native white light, IR-cut/manual day/night and speaker amplifier controls |
| Audio | Native microphone PCM to 8 kHz PCMA, PCMA speaker input; replay produces labeled simulated silence |
| Snapshots | JPEG path in independent-substream mode; unavailable with `main_only=1` |
| Hardware adapter | Runtime `dlopen` of an operator-supplied compatible `libimp`, guarded output records, state-gated cleanup |

Automatic exposure-driven day/night, motion/person detection, autotracking,
zoom, vendor provisioning/cloud, proprietary command compatibility, ONVIF
encoder-setting writes and firmware flashing are not implemented. Replay has
no sensor/JPEG encoder or speaker; unavailable operations return errors. ONVIF
is a documented subset, not a certification claim. RTSP is IPv4 and live-only;
multicast, SRTP, tunneling and seeking are not supported. HTTPS connections
are reused and idle connections park outside the request workers. Motor
commands and readiness checks do not wait on encoder polling. JPEG snapshots
in independent-substream mode still serialize their SDK transaction, so taking
one can briefly pause video. The 1080p mode advertises no ONVIF snapshot support;
clients can extract stills from RTSP. PTZ preset positions
and custom labels persist; arbitrary movement timeouts are unsupported.

## Build on the workstation

Requires a C11 compiler, CMake 3.24+, Make and Python 3. Initial configure fetches
the official MbedTLS 3.6.7 release and verifies its pinned SHA-256. cJSON 1.7.19 is
vendored with its MIT license. No system packages or global configuration are
changed by the build.

```sh
make
make test
./build/host/jooanipc-open --version
```

For a source-archive mirror or offline build, provide the same verified archive:

```sh
cmake -S . -B build/host \
  -DJIPC_MBEDTLS_ARCHIVE=/absolute/path/mbedtls-3.6.7.tar.bz2
cmake --build build/host --parallel 2
```

## Run with a real encoded fixture

Generate test material locally; none is copied from a camera:

```sh
ffmpeg -f lavfi -i testsrc2=size=320x240:rate=15 -t 2 \
  -c:v libx264 -preset ultrafast \
  -x264-params 'threads=1:keyint=15:scenecut=0:aud=1:repeat-headers=1' \
  -f h264 /tmp/camera.h264

./build/host/jooanipc-open --init config/local.ini --input /tmp/camera.h264
./build/host/jooanipc-open --config config/local.ini
```

`--init` creates a 0600 file with a random password and prints that new password
once. It refuses an existing destination. Update the private file's dimensions
to match your fixture. Empty input1/2/3 reuse input0. Replay reports its backend
as `replay`; camera controls are simulations and audio is PCMA silence.

The authenticated maintenance page is `http://127.0.0.1:8080/`. Video URLs are
`rtsp://admin:<password>@127.0.0.1:8554/live/ch0` through `ch3`; `/stream0..3` and
`/live/0..3` are aliases. Main/sub profiles for sensor 0 are 0/1, sensor 1 are 2/3.
With `main_only=1`, channels 0/1 both carry sensor 0's main feed and 2/3 both
carry sensor 1's main feed. Dimensions and bitrates describe the actual feed;
`alias_of` in status identifies shared channels. Independent substream settings
are unused in this mode, and local recording writes only channels 0 and 2.

HTTP binds to loopback by default. Configure both `tls_cert` and `tls_key` for
HTTPS on the LAN. Plain LAN HTTP requires the explicit `allow_plain_http=1`
setting; the program does not silently discard a requested TLS configuration.
Passwords are stored in the operator's 0600 configuration because RTSP Digest
and ONVIF interoperability require access to a reusable credential. They are
redacted from the configuration API and are not included in the repository.

## Build for the camera

The supported compiler is Ingenic GCC 5.4 multilib with `-muclibc`, matching the
camera's uClibc 0.9.33.2 ABI. A normal glibc MIPS cross-compiler is not a substitute.

```sh
TOOLCHAIN_ROOT=/absolute/path/to/ingenic-toolchain make mips
file build/mips/jooanipc-open
```

The resulting ELF uses `/lib/ld-uClibc.so.0`. MbedTLS and cJSON are built into the
program; only the normal OEM C/POSIX libraries are linked dynamically. `libimp`
is supplied separately at runtime. Our OpenIMP build requires its matching
compiler-provided `libatomic.so.1` beside the library, on `LD_LIBRARY_PATH`.
No vendor IMP binary is distributed in this repository.
An optional `JIPC_LIBIMP_SHA256` environment variable pins that external file.

Read [hardware integration](docs/hardware.md) before trying the IMP backend.
Use the [target configuration template](config/ja-a12.example.ini), choose a new
private password, provide TLS files and a compatible external library, and
retain the known OEM modules, sensor IQ data, reserved-memory setup and motor
calibration. The daemon refuses a running OEM media owner; it does not itself
kill `jooanipc`, load modules, rewrite boot scripts or flash the camera. Those
steps belong to the scripts: [native-boot.sh](scripts/native-boot.sh) and the
[handoff](scripts/oem-boot-handoff.sh) (installed as
`/opt/jooanipc-open/native-boot.sh` and `/opt/jooanipc-open/boot-handoff.sh`,
which the platform's `local.rc` runs), and
[upgrade-appfs.sh](scripts/upgrade-appfs.sh), which reflashes the appfs. Their
arrangement is documented in hardware integration.

## Test the actual MIPS build under QEMU

Requires `qemu-mipsel`, a private compatible rootfs extraction, FFmpeg with
libx264, OpenSSL's certificate utility and Python 3. Only the rebuilt application
and test programs are executed. No OEM application is started and no emulated
ISP, VPU or sensor is claimed.

```sh
OEM_ROOTFS=/absolute/path/to/private/rootfs make qemu-test
```

The single-stack integration runner checks RTSP/HTTP/ONVIF/discovery protocols,
TLS certificate validation, real FFmpeg TCP/UDP decoding, recording/download
decoding, replay controls, password persistence and reload. Test files and
credentials live in a disposable private directory. Raw receipts go under
`build/verification/`. The old uClibc loader may warn that the host
`/etc/ld.so.cache` is corrupt under QEMU; this is a host cache-format mismatch,
not a failed test. SDK contract tests deliberately inject a cleanup failure and
verify that further native initialization is refused.

```sh
make integration
make integration-tls
# Also exercise the two-encoder layout and its four logical URLs:
python3 tests/integration.py --binary build/host/jooanipc-open --tls --main-only
```

Run full builds/stacks sequentially. The default build has two compiler jobs;
CTest and integration tests run serially. The [verification record](docs/verification.json)
separates the original build/replay evidence from physical-camera qualification;
[the native receipt](docs/camera-native-test-20261017.json) records subsequent hardware work.
The [camera test receipt](docs/camera-test-20261017.json) records on-device
TLS, RTSP, recording and discovery checks, the fixed entropy wait, and the
then-current incompatible-SDK capture blocker. OEM services remained running
during that historical replay test. The later native deployment uses OpenIMP
and replaces OEM media ownership.

A local staging tree includes the binary, example configurations, documentation
and dependency licenses:

```sh
make stage
```

This writes only under `build/stage/`. Use `cmake --install build/mips --prefix`
with a local staging path to package the target binary without deploying it.

## Configuration and API

Configuration is strict `key=value`, with one setting per line and no duplicate
keys. String settings cannot contain control characters or leading/trailing
whitespace; spaces inside values are preserved. `bind` requires a numeric IPv4
address. Paths are interpreted from the process working directory. The examples
list supported settings. SIGHUP reloads the file after cleanly stopping owned
services; command-line overrides retain precedence. Video-setting API changes
persist configuration and restart owned services; clients reconnect afterward.

Authenticated REST endpoints:

| Method and path | Behavior |
| --- | --- |
| `GET /api/v1/status` | Backend readiness, streams, platform state and recording errors |
| `GET /api/v1/config` | Configuration with password redacted |
| `POST /api/v1/config` | Validated public settings; filesystem/listener/library settings remain local-only |
| `POST /api/v1/control` | `{ "operation": "ptz", "value":50, "value2":0 }` and documented platform/ISP controls |
| `POST /api/v1/recording` | `{ "enabled":true }` starts recording; false stops |
| `GET /api/v1/recordings` | Bounded file listing; download a listed name under the same path |
| `GET /api/v1/snapshot?channel=0` | Native JPEG when the backend can produce one |
| `POST /api/v1/talk` | Binary `audio/PCMA`, 8 kHz mono |

`/health` and `/healthz` report readiness without credentials and are never
throttled. ONVIF's public time query is read-only and never counts as a failed
login, but like every other request it is refused while its address is
throttled; all other SOAP operations require Basic or fresh WSSE
authentication. Supported motor, preset, light and speaker controls are described
in [platform controls](docs/platform.md). Unknown actions fail explicitly.

## Source and license

Published validation reports anonymize deployment hostnames and preset labels,
and omit boot identifiers and absolute motor positions. Measurements and
qualified binary hashes are preserved. Historical source revision IDs in
older reports refer to the private history before publication sanitization;
public commit IDs differ after that rewrite.

Original application code is MIT licensed. Dependencies retain their own
licenses and exact source pins; see [third-party components](docs/third-party.md)
and `dependencies.lock.json`. No firmware dump, vendor SDK binary, factory
credential, sensor IQ blob or generated recording is committed. The repository
does not depend on a sibling research checkout to build the workstation app.
