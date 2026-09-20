# Local API v1

## Contract and qualification status

`joan-daemon` and the bundled Web UI implement the versioned routes below.
HTTPS is the production default on TCP/443; TCP/80 accepts only GET/HEAD and
redirects to HTTPS. Plain HTTP exists only as an explicit host-test mode.

Implemented software is not the same as hardware qualification. Routes backed
by retained OEM media, MQTT, PTZ, audio, Wi-Fi, or the updater may return a
stable 4xx/5xx error until their helper is healthy, and the `0.1.0` line has no
supported tag until the physical-camera gates pass.

All routes except setup status and session creation require an authenticated
administrator session. State-changing HTTP requests require the session's CSRF
token. Audio WebSockets additionally require a same-origin request and the
session CSRF value in the negotiated subprotocol.

## Authentication

A fresh image uses:

```text
username: admin
password: change-me-password
```

The initial password remains valid; setup is not forced. Login and the status
response carry `default_password_warning: true` until it is changed, and the
Web UI keeps that warning visible. Password rotation invalidates existing Web
sessions and atomically publishes synchronized credentials for HTTPS, RTSP, and
SSH. An upgraded early `0.1` database whose original password cannot be
recovered preserves Web login but reports `ssh_password_sync: false` until the
administrator changes the password once.

SSH uses user `admin` and the synchronized password. Optional Ed25519 public
keys supplement password authentication; private keys are never uploaded.

## Route summary

| Route | Method/transport | Purpose |
|---|---|---|
| `/api/v1/setup/status` | `GET` | Initial-password and credential-sync state |
| `/api/v1/setup/password` | `POST` | Rotate the shared administrator password |
| `/api/v1/session` | `POST`, `GET`, `DELETE` | Create, inspect, or end a session |
| `/api/v1/status` | `GET` | Build, local-only, route, feature, and helper status |
| `/api/v1/network/wifi` | `POST`, `PUT`, `DELETE` | Stage, commit, or roll back a Wi-Fi transaction |
| `/api/v1/network/routes` | `GET`, `PUT` | Inspect or add explicitly allowed RFC1918/ULA routes |
| `/api/v1/network/mdns` | `GET`, `PUT` | Inspect or change the persistent `.local` hostname |
| `/api/v1/streams` | `GET` | Enumerate main/sub RTSP and fMP4 resources |
| `/api/v1/video/{main,sub}/init.mp4` | `GET` | fMP4 initialization segment |
| `/api/v1/video/{main,sub}/fragment.mp4?after=N` | `GET` | Next fMP4 fragment after sequence `N` |
| `/api/v1/snapshot` | `GET` | JPEG snapshot through loopback-only OEM GoAhead |
| `/api/v1/ptz/lease` | `POST` | Acquire the short exclusive PTZ lease |
| `/api/v1/ptz/move` | `POST` | Start a bounded directional move or jog |
| `/api/v1/ptz/stop` | `POST` | Stop movement and release the lease |
| `/api/v1/ptz/home` | `POST` | List/set/go to the home position |
| `/api/v1/ptz/presets` | `GET`, `POST`, `PUT`, `DELETE` | List, save/update, recall, or delete presets |
| `/api/v1/operations/{id}` | `GET` | Poll an accepted asynchronous OEM operation |
| `/api/v1/ssh/authorized-keys` | `GET`, `POST`, `DELETE` | Inspect, add, or remove optional Ed25519 keys |
| `/api/v1/update` | `POST`, `PUT` | Stage then apply an authenticated release |
| `/api/v1/audio/mic` | WebSocket | Listen to the camera microphone |
| `/api/v1/audio/talk` | WebSocket | Press-to-talk audio path |

Clients must ignore unknown response fields and treat a 202 response as
accepted, not completed; poll its `/api/v1/operations/{id}` URL.

## Media and controls

The PWA consumes the main and sub fMP4 routes. The daemon converts the retained
local RTSP/RTP H.264 streams into initialization segments and bounded fragmented
MP4; it does not transcode video. Stream readiness is reported separately for
main and sub. RTSP remains available on TCP/554 for local NVR clients through
the daemon's Digest-authenticated proxy; the username is `admin` and the
password is synchronized with WebUI/SSH. The OEM upstream is loopback-only on
TCP/8554.

The audio WebSockets bridge to the bounded local audio router. The UI supports
microphone listening and explicit press-to-talk; microphone playback pauses
while talking to reduce feedback. OEM alarm/voice playback is discarded and
the active-high speaker amplifier is enabled only for the authenticated,
deadman-bounded talkback lease. These paths remain hardware-promotion gates.

PTZ requires a short lease so a lost browser cannot leave the motor running.
The Web UI exposes directional jog/stop, home, and saved presets. Commands use
the loopback OEM command channel and complete asynchronously.

## Local networking and DNS-SD

The runtime removes IPv4 and IPv6 default routes and repeatedly prunes any that
reappear. Connected local-subnet routes remain. `/api/v1/network/routes` accepts
only explicit RFC1918 or ULA routes; it cannot add a public or default route.

The embedded responder advertises A plus DNS-SD PTR/SRV/TXT records for HTTPS,
SSH, RTSP, and `_jooan-camera._tcp` on UDP/5353. The configured label is
advertised as `label.local`. DNS-SD is link-local; routed VLAN discovery needs
a trusted reflector, while direct address access does not.

Wi-Fi replacement is transactional: POST stages/applies a candidate, PUT
commits it after reconnect/health, and DELETE rolls it back. The generic image
contains no SSID or passphrase and preserves the existing OEM Wi-Fi setting.

## Signed updates

Release stages carry a complete SHA-256 inventory signed with deterministic
ECDSA P-256/SHA-256. The device verifier pins the public key and target ID,
checks every staged file, enforces the exact hardware/ABI hashes, and rejects a
release sequence that is not strictly newer. The outer OEM MD5 remains carrier
format/corruption metadata; it is not the authenticity boundary.

The update API separates staging from apply. Applying a verified package uses
the loopback-only OEM updater. An uninstall artifact is separately signed and
declares destructive-removal semantics; it is not a state rollback.

## Errors and secrets

Errors use an appropriate HTTP status and a JSON body with a stable code and a
human-readable message. Responses and logs must not expose passwords, session
tokens, cookies, Wi-Fi passphrases, private keys, raw OEM replies containing
secrets, or filesystem paths that disclose them.
