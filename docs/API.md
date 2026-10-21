# Local API v1

## Contract and qualification status

`joan-daemon` and the bundled Web UI implement the versioned routes below,
over plain HTTP on TCP/80. Nothing is encrypted in transit: a sign-in sends the
password and every later request carries the session cookie in the clear, so
use the API only from the trusted network the camera is isolated on.

Implemented software is not the same as hardware qualification. Routes backed
by MQTT, Wi-Fi, the clock, or the updater may return a
stable 4xx/5xx error until their helper is healthy, and the `0.1.0` line has no
supported tag until the physical-camera gates pass.

All routes except setup status and session creation require an authenticated
administrator session. State-changing HTTP requests require the session's CSRF
token.

## Authentication

A fresh image uses:

```text
username: admin
password: admin
```

The initial password remains valid; setup is not forced. Login and the status
response carry `default_password_warning: true` until it is changed, and the
Web UI keeps that warning visible. Password rotation invalidates existing Web
sessions and atomically publishes synchronized credentials for the Web UI and
SSH. (It is also pushed to the OEM's loopback-only RTSP during bootstrap, which
`rtsp_password_sync` reports; no client uses that service.) An upgraded early
`0.1` database whose original password cannot be
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
| `/api/v1/status` | `GET` | Build, local-only, route, feature, network-time, and helper status |
| `/api/v1/network/wifi` | `POST`, `PUT`, `DELETE` | Stage, commit, or roll back a Wi-Fi transaction. The SSID joins its 5 GHz BSS when a pre-connect scan finds one strong enough for sustained video, falling back to 2.4 GHz otherwise |
| `/api/v1/network/routes` | `GET`, `PUT` | Inspect or set the allowlist of RFC1918/ULA prefixes reachable through the pruned gateway. Specific prefixes only; a default route is never restored |
| `/api/v1/network/quality` | `GET` | Read current Wi-Fi association/quality, Ethernet link/speed and cumulative interface counters |
| `/api/v1/network/mdns` | `GET`, `PUT` | Inspect or change the persistent `.local` hostname |
| `/api/v1/time` | `GET`, `PUT` | Read the clock and network-time state; configure network time, or set the clock from the client (UTC epoch seconds, as a JSON string). See [Camera time](#camera-time) |
| `/api/v1/timezone` | `PUT` | Set the stored time zone (`gmt_tz`, a `"GMT-03:00"`-style offset). The burned-in OSD overlay adopts it on the next camera restart. |
| `/api/v1/ssh/authorized-keys` | `GET`, `POST`, `DELETE` | Inspect, add, or remove optional Ed25519 keys |
| `/api/v1/update` | `POST`, `PUT` | Stage then apply an authenticated release |

Clients must ignore unknown response fields.

## Media

`joan-daemon` is the platform and serves no media. Video, ONVIF, pan/tilt,
presets and the NVR (Frigate) integration belong to the separate media daemon
[jooanipc-open](https://github.com/ADCDS/jooanipc-open), which replaces the
OEM `jooanipc` after boot and documents its own ports, credentials and API.
A native boot never starts the OEM `jooanipc`; the guarded handoff fallback
still uses it to bring up Wi-Fi and home the motor (see
[Architecture](ARCHITECTURE.md)). Without the media daemon, the guard keeps the OEM
RTSP (TCP/8554) and ONVIF (TCP/8899) services on loopback, so this firmware
alone exposes no video or PTZ. The Web UI is a maintenance console and plays no
video; the guard keeps OEM alarm and voice playback muted.

`GET /api/v1/status` reports `"media":"external"` under `features`, which no
longer has `rtsp` or `ptz` entries. `/api/v1/streams`, the optional OEM audio
pause (`/api/v1/system/audio-pause`) and the `/onvif/` subset on port 80 were
removed in 0.2.22.

## Camera time

The board has no RTC. Boot restores a coarse saved time that lags real time by
the downtime, and ONVIF clients such as Frigate measure the camera's clock
offset only once, so a camera reboot broke them until they restarted. An
optional SNTP client keeps the clock on UTC. It is off by default. The Web UI's
Camera time card shows its state and sets its server.

`PUT /api/v1/time` with `{"ntp":"gateway"}` or `{"ntp":"192.0.2.1"}` (an IPv4
address, optionally with `:port`) turns it on, `{"ntp":""}` turns it off, and
anything else is a 400 `invalid_ntp`. The setting persists as `ntp-server` in
the state directory. `gateway` is the IPv4 gateway the supervisor last pruned
from the default route, re-read on every attempt; the port defaults to 123.
The client retries every 30 s until it gets a valid reply, then polls hourly.
Steps of 2 s or more go through the integration helper's `time-set`, which also
saves the time the next boot restores. While an OEM `jooanipc` process exists
no step is made and `error` reads "waiting for the OEM application to hand
over media". `PUT /api/v1/time` with
`{"epoch":"<UTC seconds>"}` still sets the clock manually.

`GET /api/v1/time` returns the clock and the client's state; the `ntp` object
also appears in `GET /api/v1/status`:

```json
{"epoch":1767225600,"ntp":{"server":"gateway","resolved":"192.0.2.1","synchronized":true,"attempts":3,"last_offset_ms":-12.500,"last_sync_age_seconds":40,"error":""}}
```

With network time off, `ntp` is `{"server":"","synchronized":false}`.
`last_offset_ms` and `last_sync_age_seconds` appear once a reply has been
accepted. Replies must echo a random origin cookie and pass leap, version,
mode, stratum, NTP-era and timing checks; SNTP is otherwise unauthenticated, so
use a server on the camera's own network (see
[Network security](NETWORK-SECURITY.md#network-time)).

## Local networking and DNS-SD

The runtime removes IPv4 and IPv6 default routes and repeatedly prunes any that
reappear. Connected local-subnet routes remain. `/api/v1/network/routes` accepts
only explicit RFC1918 or ULA routes; it cannot add a public or default route.

The embedded responder advertises A plus DNS-SD PTR/SRV/TXT records for HTTP,
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
