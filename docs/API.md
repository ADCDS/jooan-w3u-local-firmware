# Local API v1

## Contract and qualification status

`joan-daemon` and the bundled Web UI implement the versioned routes below.
HTTPS is the production default on TCP/443; TCP/80 accepts only GET/HEAD and
redirects to HTTPS. Plain HTTP exists only as an explicit host-test mode.

Implemented software is not the same as hardware qualification. Routes backed
by retained OEM media, MQTT, PTZ, Wi-Fi, or the updater may return a
stable 4xx/5xx error until their helper is healthy, and the `0.1.0` line has no
supported tag until the physical-camera gates pass.

All routes except setup status and session creation require an authenticated
administrator session. State-changing HTTP requests require the session's CSRF
token. ONVIF (below) authenticates each SOAP request instead.

## Authentication

A fresh image uses:

```text
username: admin
password: admin
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
| `/api/v1/network/wifi` | `POST`, `PUT`, `DELETE` | Stage, commit, or roll back a Wi-Fi transaction. The SSID joins its 5 GHz BSS when a pre-connect scan finds one strong enough for sustained video, falling back to 2.4 GHz otherwise |
| `/api/v1/network/routes` | `GET`, `PUT` | Inspect or set the allowlist of RFC1918/ULA prefixes reachable through the pruned gateway. Specific prefixes only; a default route is never restored |
| `/api/v1/network/quality` | `GET` | Read current Wi-Fi association/quality, Ethernet link/speed and cumulative interface counters |
| `/api/v1/network/mdns` | `GET`, `PUT` | Inspect or change the persistent `.local` hostname |
| `/api/v1/tls/identity` | `GET`, `PUT`, `DELETE` | Report, enroll or discard the HTTPS certificate. `PUT` takes one PEM bundle (private key plus chain, any order) and is how a camera gets a certificate that phones and televisions already trust; `DELETE` returns to a generated self-signed identity. Both need a restart to take effect. |
| `/api/v1/time` | `PUT` | Set the camera clock from the client (UTC epoch seconds, as a JSON string) |
| `/api/v1/timezone` | `PUT` | Set the stored time zone (`gmt_tz`, a `"GMT-03:00"`-style offset). The burned-in OSD overlay adopts it on the next camera restart. |
| `/api/v1/streams` | `GET` | Enumerate the main/sub RTSP streams |
| `/api/v1/ssh/authorized-keys` | `GET`, `POST`, `DELETE` | Inspect, add, or remove optional Ed25519 keys |
| `/api/v1/update` | `POST`, `PUT` | Stage then apply an authenticated release |

Clients must ignore unknown response fields.

## Media and controls

Watching, recording and steering belong to an NVR such as Frigate; the Web UI
is a maintenance console and plays no video. Both sensors are available as
H.264 over RTSP on TCP/554 through the daemon's Digest-authenticated proxy; the
username is `admin` and the password is the administrator password. The OEM
upstream is loopback-only on TCP/8554. Pan/tilt and presets are ONVIF, below.
The camera's RTSP streams carry no usable audio, and the retrofit ships no
listen or talk path; the guard keeps OEM alarm and voice playback muted.

## ONVIF for NVRs

Network video recorders such as Frigate control pan/tilt and presets over
ONVIF. The daemon answers the few Profile S operations they need on the HTTPS listener,
at `/onvif/device_service`, `/onvif/media` and `/onvif/ptz`. There is no
separate port and no WS-Discovery: point the client at the camera's address and
port 443.

- **Authentication.** Every operation except `GetSystemDateAndTime` needs a
  WS-Security UsernameToken with a PasswordDigest for user `admin` and the
  administrator password shared with the Web UI, SSH and RTSP. A token works
  once. Its `Created` time is hashed but not bounded, because the camera clock is
  only as good as its last manual sync. Failures count against the Web login's
  per-address budget (five a minute). Requests carrying a foreign `Origin` are
  refused.
- **Media.** One profile, `ch0`: sensor A (2304×1296, H.264), the one on the
  pan/tilt head. Sensor B is fixed and has nothing to offer here; its RTSP URL
  is unchanged.
- **Pan/tilt.** Each `ContinuousMove` is one coarse nudge along the dominant
  axis: the daemon starts the motor, stops it 350 ms later (about 190 motor
  steps, roughly 17°) and only then replies. The live picture lags several
  seconds, so a hold could not be aimed, and a fixed step lands the same way
  every time. The OEM moves one axis at a time at a single speed, so the
  velocity only picks the direction. `Stop` is accepted and has nothing left to
  do. There is no zoom, no relative or absolute move, and no position or move
  status, so NVR autotracking is unavailable.
- **Presets.** `GetPresets`, `GotoPreset`, `SetPreset` and `RemovePreset` act on
  the OEM's six preset slots (tokens `0`–`5`). `SetPreset` needs a
  `PresetName` (1–64 characters, no quotes or backslashes). With a `PresetToken`
  it re-saves that slot at the current position. The OEM refuses a save where a
  preset already exists at the same position, and the fault reports its status
  (`-2`). After a recall, further moves and preset commands are refused for 20 s
  while the head travels: a command mid-travel lands the head somewhere else.
- **Discovery.** `GetCapabilities` and `GetProfiles` complete the set. It is
  what Frigate uses, with the flash budget as the limit. Any other operation
  returns `ter:ActionNotSupported`.

The OEM's own ONVIF service stays confined to loopback TCP/8899. It ignores
credentials, zeep-based clients cannot talk to it, and its preset operations
do nothing. The daemon uses only its `ContinuousMove` and `Stop`, as the motor
path. The OEM runs a move until it is told to stop, so the daemon always issues
the Stop itself and retries an unconfirmed one.

A Frigate camera entry for the pan/tilt sensor looks like this:

```yaml
cameras:
  jooan_main:
    onvif:
      host: https://192.0.2.10   # the camera; the https:// prefix selects TLS
      port: 443
      user: admin
      password: <administrator password>
```

Frigate then shows direction arrows and a preset list. It cannot save presets,
and it reads the list only when it connects. Save, rename and delete them with
`tools/onvif_presets.py` (standard-library Python; `nudge` aims the head first),
then restart Frigate.

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

Authenticated `GET /api/v1/network/quality` is read-only and sampled on demand
(the Network UI refreshes on entry or on pressing Refresh; there is no polling).
Example response:

```json
{"wifi":{"associated":true,"ssid":"Family Room WiFi","signal_dbm":-48,"rate_mbps":72.2,"rx_bytes":"123456","tx_bytes":"234567","rx_packets":"1400","tx_packets":"1500"},"ethernet":{"link":false,"speed_mbps":null,"rx_bytes":"0","tx_bytes":"0","rx_packets":"0","tx_packets":"0"}}
```

`associated` and `link` are booleans or `null` when status is unavailable.
SSID is emitted only for a completed association, capped at 64 characters and
sanitized to a limited printable subset (`?` substitutes unsupported bytes).
`signal_dbm`, `rate_mbps` and `speed_mbps` are numbers or `null` if absent;
Wi-Fi signal/rate are null when not associated, Ethernet speed is null when down.
Counters are decimal strings (to avoid JavaScript integer precision loss), or
`null` when unreadable; they are cumulative since boot and can wrap/reset.
Only fixed `wlan0` and `eth0` are inspected. No BSSID/MAC, IP, gateway,
passphrase, PSK, account credentials, or unfiltered tool output is returned.
The endpoint requires an administrator session and responds with 502
`integration_failed` if its integration helper fails.

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
