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
**640×360**, both at 15 fps. With `audio=1` the microphone streams on the
main URLs and you can talk through the speaker (see [Audio and talk](#audio-and-talk);
[hardware test](docs/camera-audio-test-20261105.json)). `main_only=1` creates two
physical H.264 encoders; the four existing URLs remain available, with each
substream URL sharing its lens's main feed. Bitrates are 3072/768 kbps with a
one-second GOP. Physical pan/tilt and persistent named presets work, and
Frigate records both views. The new graph uses separate ISP scaler slots to
avoid the shared-stride corruption from the previous asymmetric attempt.
The stock application's 2304×1296 stream (apparently upscaled by the ISP from
the sensors' 1080p input; see [hardware notes](docs/hardware.md)) still exceeds the current full-frame memory
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
| Discovery | IPv4 WS-Discovery Probe/Resolve, stable identity, response limits and interface-local discovery; off by default (`discovery=0`), enable with `discovery=1` |
| Recording | Rotating H.264/PCMA files, metadata, storage retention, IDR recovery after drops, authenticated download |
| Clock | Optional SNTP client for the RTC-less board: random origin cookie, clock steps of one second or more, status under `clock` |
| PTZ | Steady OEM-style 350 ms ONVIF nudges, 1.2-second REST lease, 16 named persisted presets; replay simulation |
| Lights | Native white light, IR-cut/manual day/night and speaker amplifier controls |
| Audio | Microphone: 16 kHz capture to 8 kHz PCMA on the RTSP URLs, timestamps from the sample count, silence over gaps, restart with backoff after a stall; a microphone fault never stops video. Talk: ONVIF RTSP backchannel and `POST /api/v1/talk`, one talker at a time, half-duplex, amplifier switched per session with a 500 ms deadman mute. Replay: silence (or a test tone), talk written to a capture file |
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
as `replay`; camera controls are simulations and audio is PCMA silence
(`JIPC_REPLAY_TONE=1` sends a 500 Hz tone instead). Set `JIPC_SPEAKER_CAPTURE`
to a file path to record what talkers send.

The admin UI is `http://127.0.0.1:8080/` (it signs in through the platform; see
[Admin UI](#admin-ui)). Video URLs are
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
| `GET /` and `GET /ui/<file>` | The admin UI, without credentials |
| `/api/joan/<path>` | Relayed to the platform's `/api/<path>` on loopback (GET, POST, PUT, DELETE) |
| `GET /api/v1/status` | Backend readiness, streams, platform state and recording errors |
| `GET /api/v1/config` | Configuration with password redacted |
| `POST /api/v1/config` | Validated public settings; filesystem/listener/library settings remain local-only |
| `POST /api/v1/control` | `{ "operation": "ptz", "value":50, "value2":0 }` and documented platform/ISP controls |
| `POST /api/v1/recording` | `{ "enabled":true }` starts recording; false stops |
| `GET /api/v1/recordings` | Bounded file listing; download a listed name under the same path |
| `GET /api/v1/snapshot?channel=0` | Native JPEG when the backend can produce one |
| `POST /api/v1/talk` | Binary `audio/PCMA`, 8 kHz mono, at most 8000 bytes (1 s) per request: `200` queued, `409` another talker or the 2 s queue is full, `503` speaker unavailable |

`/health` and `/healthz` report readiness without credentials and are never
throttled. ONVIF's public time query is read-only and never counts as a failed
login, but like every other request it is refused while its address is
throttled; all other SOAP operations require Basic or fresh WSSE
authentication. Supported motor, preset, light and speaker controls are described
in [platform controls](docs/platform.md). Unknown actions fail explicitly.

## Admin UI

The daemon serves the admin UI at `/` from files compiled into the binary
(`ui/`, embedded by `cmake/embed-ui.cmake`; see [ui/README.md](ui/README.md)).
There is one sign-in, with the platform's admin account:

- `/api/joan/<path>` is relayed to `http://127.0.0.1/api/<path>`, the
  platform daemon on the same camera. The platform authenticates: its
  `joan_session` cookie and `X-CSRF-Token` pass through (no other cookie
  does), and on HTTPS its cookie gains `Secure`. Requests that change state
  need this page's `Origin`, which the relay checks and does not forward.
  Failed sign-ins count toward the per-address throttle. Two relays run at a
  time, so slow platform calls never hold up ONVIF; a relay waits at most
  15 s and returns `502` if the platform does not answer.
- The media API also accepts that platform session, checked against the
  platform and cached for 3 s. Changes need the same `X-CSRF-Token` and the
  page's `Origin`. Basic, Digest and WS-Security for NVRs are unchanged.
- Requests that carry a cookie or `X-Requested-With` get no
  `WWW-Authenticate` challenge on `401`, so the browser does not open its own
  password dialog over the sign-in page.
- Every response sends a strict `Content-Security-Policy` (same origin only,
  no inline code), `X-Frame-Options: DENY` and `Referrer-Policy: no-referrer`.

`JIPC_JOAN_PORT` in the daemon's environment points the relay at another
loopback port (tests only).

## Audio and talk

`audio=1` turns on both directions; `audio=0` (the default) leaves the audio
device closed.

**Microphone.** Each main URL (`/live/ch0`, `/live/ch2`) carries a second
track, PCMA 8 kHz mono, with RTP timestamps that advance exactly 8 per byte.
Frigate records it with `preset-record-generic-audio-aac`.

**Talk.** A client asks for the speaker track by sending
`Require: www.onvif.org/ver20/backchannel` with `DESCRIBE`. The SDP then
gains a third `m=audio` line, `sendonly`, PCMA/8000, `trackID=2`. Without the
header the SDP is unchanged. The talk track works over RTSP-over-TCP only
(UDP gets `461`), and a session may set up only that track. go2rtc does all
of this with `#backchannel=1`; Frigate shows its microphone button for such a
stream when the page is opened over HTTPS:

```yaml
go2rtc:
  streams:
    jooan_main:
      - rtsp://admin:<password>@<camera>:18554/live/ch0#backchannel=0
    jooan_main_talk:
      - rtsp://127.0.0.1:8554/jooan_main#backchannel=0
      - rtsp://admin:<password>@<camera>:18554/live/ch0#backchannel=1#media=audio
cameras:
  jooan_main:
    live:
      streams:
        Main: jooan_main_talk
```

Keep `#backchannel=0` on recording and detection sources: go2rtc reconnects a
source when talk starts, and the separate talk stream keeps that away from
recordings. `POST /api/v1/talk` takes the same audio over HTTPS.

The speaker rules:

- One talker at a time. Another connection, or another RTP source on the
  same connection, gets `409` (HTTP) or is ignored (RTSP).
- Each session switches the amplifier on, waits 200 ms, and plays from a 2 s
  queue. RTSP audio more than 600 ms ahead is cut back to 300 ms; HTTP
  talkers are told when the queue is full.
- 500 ms without audio ends the session: 80 ms of silence, then mute.
  `TEARDOWN` or a closed connection ends it after at most 200 ms of queued
  audio.
- Half-duplex: the microphone sends silence while someone talks and for
  300 ms after, so the talker does not hear themselves. Set
  `JIPC_TALK_FULL_DUPLEX=1` in the daemon's environment to turn this off.
- Talk is refused until the platform's boot mute hook has finished (the
  marker `/run/jooan-local/speaker-hook-released`), so nothing plays during
  boot.
- Three failed speaker writes close the output, mute, and refuse talk for
  5 s, doubling to 60 s on repeated faults. The output closes after 10 s idle.
- Levels start at the stock firmware's values: microphone analog gain 22
  and volume 80, speaker analog gain 25 and volume 60. Change them with
  `POST /api/v1/control`: `{"operation":"volume","value":60,"value2":1}`
  (-30 to 120, 60 is unity, about 0.5 dB per step) or
  `{"operation":"gain","value":22,"value2":0}` (the codec's analog stage,
  0 to 31). `value2` is 0 for the microphone and 1 for the speaker. A
  microphone change applies within about 100 ms; a speaker gain change at
  the next talk after 10 s of quiet. Changes last until the daemon restarts.

`GET /api/v1/status` reports both directions under `audio`: microphone state,
volume and gain (and the speaker's gain), frame, silence-fill, dropped and
restart counts; speaker state, sessions,
played and dropped milliseconds, refusals and faults.

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
