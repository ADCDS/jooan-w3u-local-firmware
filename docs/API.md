# Local API v1

## Contract and qualification status

`joan-daemon` and the bundled Web UI implement the versioned routes below,
over plain HTTP on TCP/80. With the media layer installed, the admin UI is the
media daemon's HTTPS page at `https://<camera>/`: it relays these routes as
`/api/joan/<path>` to `127.0.0.1:80` (see
[media/README.md](../media/README.md#admin-ui)), and every non-API page on
port 80 answers `302` to `https://<host>/`. Nothing is encrypted in transit: a sign-in sends the
password and every later request carries the session cookie in the clear, so
use the API only from the trusted network the camera is isolated on.

Implemented software is not the same as hardware qualification. Routes backed
by MQTT, Wi-Fi, the clock, or the updater may return a
stable 4xx/5xx error until their helper is healthy, and no release (the
current one is 0.2.38, release sequence 72) has a supported tag until the
physical-camera gates pass.

Every request, sign-in and static files included, must carry a `Host` that is
a private or loopback IPv4 literal (10/8, 172.16/12, 192.168/16, 127/8), a
loopback or ULA IPv6 literal, or the configured label bare or under `.local`,
`.lan`, `.home.arpa` or `.internal` (for example `camera1.local` or
`camera1.lan`); anything else gets `403 host_rejected` (see
[Network security](NETWORK-SECURITY.md#host-and-origin-checks)).

All routes except setup status and session creation require an authenticated
administrator session (`401 authentication_required`). State-changing HTTP
requests require the session's CSRF token in `X-CSRF-Token`
(`403 csrf_rejected`), and an `Origin` header, when present, must be
`http://<Host>` (`403 origin_rejected`). Sessions expire after 30 minutes
without a request, unless the sign-in asked to be remembered.

`POST /api/v1/session` with `"remember":true` adds `Max-Age=2592000` to the
cookie and answers `"remember":true`. Such a sign-in lasts 30 days and survives
camera restarts. At most 8 are kept (the oldest is replaced), as SHA-256 of the
cookie, in `sessions.db` in the platform's state directory, which is written
only at sign-in, sign-out and a change of password. Signing out ends that one; changing the
password ends them all, before the new one is saved.

## Authentication

A fresh image uses:

```text
username: admin
password: admin
```

The initial password remains valid; setup is not forced, and nothing is
locked until it is changed. `/api/v1/setup/status`, the session responses and
`/api/v1/status` carry `default_password_warning: true` while the initial
value is in use. The Web UI shows no banner for it; its System page shows the
password state ("initial value" or "changed").

After five failed sign-ins from one address within a minute, further attempts
from it get `429 rate_limited` until that minute is over. A new password must
be 1 to 128 characters without control characters. Password rotation
invalidates existing Web sessions and atomically publishes synchronized
credentials for the Web UI and SSH. The platform keeps them only as
PBKDF2-SHA256 (Web UI) and MD5-crypt `$1$` (SSH, the strongest format the
camera's uClibc supports).

The OEM application's loopback-only RTSP has its own credential: a random
24-hex-character secret in `config/rtsp.password`, regenerated at every
`joan-daemon` start (which also overwrites the plaintext admin password that
releases before 0.2.24 kept there) and at every password change. The MQTT
bridge pushes it to the OEM application when that application connects, which
happens only in the fallback modes; `rtsp_password_sync` in `/api/v1/status`
reports whether the OEM application confirmed it. No client uses that
service.

An upgraded early `0.1` database whose original password cannot be
recovered preserves Web login but reports `ssh_password_sync: false` until the
administrator changes the password once; the Web UI then shows a banner.

SSH uses user `admin` and the synchronized password. Optional Ed25519 public
keys supplement password authentication; private keys are never uploaded.

## Route summary

| Route | Method/transport | Purpose |
|---|---|---|
| `/api/v1/setup/status` | `GET` | Initial-password and credential-sync state |
| `/api/v1/setup/password` | `POST` | Rotate the shared administrator password |
| `/api/v1/session` | `POST`, `GET`, `DELETE` | Create, inspect, or end a session |
| `/api/v1/status` | `GET` | Build, local-only, route, feature, network-time, and helper status |
| `/api/v1/network/wifi/scan` | `GET` | Nearby networks, strongest first (takes 5 to 10 seconds). See [Local networking](#local-networking-and-dns-sd) |
| `/api/v1/network/wifi` | `GET`, `POST`, `PUT`, `DELETE` | Read the state of a Wi-Fi trial; stage, commit, or roll back a Wi-Fi transaction. The SSID joins its 5 GHz BSS when a pre-connect scan finds one strong enough for sustained video, falling back to 2.4 GHz otherwise |
| `/api/v1/network/ip` | `GET`, `PUT` | Read or change how the camera gets its address, automatic or static. See [Local networking](#local-networking-and-dns-sd) |
| `/api/v1/network/routes` | `GET`, `PUT` | Inspect or set the allowlist of RFC1918/ULA prefixes reachable through the pruned gateway. Specific prefixes only; a default route is never restored |
| `/api/v1/network/quality` | `GET` | Read current Wi-Fi association/quality, band and gateway, Ethernet link/speed and cumulative interface counters |
| `/api/v1/network/mdns` | `GET`, `PUT` | Inspect or change the persistent `.local` hostname |
| `/api/v1/time` | `GET`, `PUT` | Read the clock and network-time state; configure network time, or set the clock from the client (UTC epoch seconds, as a JSON string). See [Camera time](#camera-time) |
| `/api/v1/timezone` | `PUT` | Set the time zone stored in the OEM configuration (`gmt_tz`, a `"GMT-03:00"`-style offset). Scheduled restarts and the media daemon's day/night and white-light schedules follow it; the OEM application's burned-in OSD also uses it, on its next start (fallback modes). |
| `/api/v1/ssh/authorized-keys` | `GET`, `POST`, `DELETE` | Inspect, add, or remove optional Ed25519 keys (up to 8). `GET` returns them in `authorized_keys`, one key per line; `POST` adds one, and a key already present changes nothing; `DELETE` takes `{"id":"<line number>"}` or `{"id":"all"}` |
| `/api/v1/system/restart` | `POST` | Restart the camera. See [Restarting](#restarting) |
| `/api/v1/system/restart-schedule` | `GET`, `PUT` | Read or set a daily restart. See [Restarting](#restarting) |
| `/api/v1/system/reset` | `POST` | Reset to defaults, keeping Wi-Fi. See [Reset to defaults](#reset-to-defaults) |
| `/api/v1/update` | `POST`, `PUT`, `GET` | Stage then apply an authenticated release; `GET` says how the last install ended: `{"state":"running","reason":"","restart":""}`, and `restart` is `needed` when it did not restart because a media flash holds the lock |

Clients must ignore unknown response fields.

## Media

`joan-daemon` is the platform and serves no media. Video, ONVIF, pan/tilt,
presets and the NVR (Frigate) integration belong to the media daemon
[jooanipc-open](../media/), which documents its own ports (TCP/18554 RTSP,
TCP/18443 HTTPS and ONVIF), credentials and API. A native boot never starts the
OEM `jooanipc`; the guarded handoff fallback still uses it to bring up Wi-Fi
and home the motor before jooanipc-open takes over (see
[boot modes](ARCHITECTURE.md#boot-modes)). In the modes where the OEM
application runs, the guard keeps its RTSP (`127.0.0.1:8554`) and ONVIF
(`127.0.0.1:8899`) on loopback and its alarm and voice playback muted. The Web
UI is a maintenance console and plays no video.

`GET /api/v1/status` reports `"media":"external"` under `features`, which no
longer has `rtsp` or `ptz` entries. `/api/v1/streams`, `/api/v1/snapshot`, the
optional OEM audio pause (`/api/v1/system/audio-pause`) and the `/onvif/`
subset on port 80 are gone; they answer `404 route_not_found`.

## Camera time

The board has no RTC. Boot restores a coarse saved time that lags real time by
the downtime, and ONVIF clients such as Frigate measure the camera's clock
offset only once, so a camera reboot broke them until they restarted. An
optional SNTP client keeps the clock on UTC. It is off by default. Once it or a person
has set the clock, `/run/jooan-local/clock-set` says so, until the next boot: the media
layer's schedules and timestamp wait for it, and a daily restart runs at its set time only
then, and otherwise once a day counted from the start. Every restart the platform asks for
first saves the time. A daily restart also waits while the gateway is not answering. The Web UI's
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
over media"; in rescue and platform-only modes that process runs for the whole
uptime, so the clock is never stepped there. `PUT /api/v1/time` with
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

## Restarting

`POST /api/v1/system/restart` answers `200 {"ok":true}` and restarts the camera
about 2 seconds later. It refuses with `409 {"ok":false,"reason":"..."}` while
a restart would do harm: `update` (an update or a media install is running),
`wifi-trial` (a Wi-Fi change is waiting to be kept), `boot-trial` (a new runtime
is being qualified), `no-runtime` (a platform update stopped half way and none is
on flash: see [Recovery](RECOVERY.md)) or `starting` (this boot is still qualifying).

`GET /api/v1/system/restart-schedule` returns `{"enabled":true,"time":"04:00"}`
or `{"enabled":false,"time":null}`. `PUT` with `{"time":"04:00"}` sets a daily
restart in the camera's time zone (`/api/v1/timezone`), and `{"time":""}` turns
it off; a bad time is a 400 `invalid_time`. The boot supervisor checks it every
few seconds. It skips the restart when the camera has been up for less than an
hour, the gateway is not answering, or one of the conditions above holds; a skipped
restart waits for the next day. With no clock anything has confirmed, it runs once a day
counted from the last start instead of at its set time.

## Reset to defaults

`POST /api/v1/system/reset` with `{"confirm":"<camera name>"}` (the name from
`/api/v1/network/mdns`) answers `200 {"ok":true}` and restarts the camera. A wrong
name is a 400 `confirm_mismatch`, and it refuses with the same `409` reasons as a
restart, except `starting`: someone holding the button just after power-on means it, and
it counts for nothing against that boot. A fixed address goes back to automatic. Holding the reset button for 5 seconds does the same in a native boot;
the button only counts once it has been seen released, so a stuck one does nothing.

The next boot erases, before anything that owns those files is running: both
passwords (back to the initial value), SSH keys, remembered sign-ins, the restart
schedule, presets, and the picture, night, light and audio controls. A power cut
halfway resumes it at the following boot. It keeps Wi-Fi, the local name, allowed
networks, network time, the time zone, certificates and the installed releases, so
the camera stays reachable the same way. The `audio` setting in the media
configuration also stays.

## Local networking and DNS-SD

The runtime removes IPv4 and IPv6 default routes and repeatedly prunes any that
reappear. Connected local-subnet routes remain. `/api/v1/network/routes` accepts
only explicit RFC1918 or ULA routes; it cannot add a public or default route.

The embedded responder advertises the `label.local` A record plus DNS-SD
PTR/SRV/TXT records for `_http._tcp` (80), `_ssh._tcp` (22) and
`_jooan-camera._tcp` (80) on UDP/5353. It never advertises RTSP. The label
defaults to `jooan-w3u`; a change through `/api/v1/network/mdns` takes effect
after the next `joan-daemon` restart. The responder answers only peers on a
directly attached IPv4 subnet, including unicast queries; routed VLAN
discovery needs a trusted mDNS reflector on the camera's subnet, while direct
address access does not.

Wi-Fi replacement is transactional: POST stages/applies a candidate, PUT
commits it after reconnect/health within 90 seconds, and DELETE rolls it back;
a candidate that is not committed in time is rolled back and the camera
reboots. The generic image contains no SSID or passphrase. The committed
network is the only one a native boot uses, so native boot requires one; the
OEM's stored Wi-Fi configuration is left on flash for the fallback modes.

`GET /api/v1/network/wifi` reports a trial as
`{"pending":true,"seconds_left":61,"ssid":"Family Room WiFi","joined":true,"address":"192.0.2.10"}`,
and otherwise `{"pending":false,"last":"committed"}` (`last` is `committed`,
`rolled_back` or `null`). `address` is `null` until the camera has one. `PUT` and
`DELETE` need an `id` string, the one `POST` returned or any other, such as
`"current"` from a device that did not start the trial: there is only one trial,
and they act on it.

`GET /api/v1/network/wifi/scan` asks the Wi-Fi driver for a fresh scan and lists
up to 30 networks, each once at its strongest:

```json
[{"ssid":"Family Room WiFi","signal_dbm":-48,"frequency_mhz":5180,"secure":true,"in_use":true}]
```

Hidden networks are not listed. Names follow the same sanitizing as below.

`GET /api/v1/network/ip` returns
`{"mode":"dhcp","address":"192.0.2.10","prefix":24,"gateway":"192.0.2.1"}`, where
`mode` is `dhcp` or `static` and the rest is what the camera has now. `PUT` with
`{"mode":"dhcp"}` or
`{"mode":"static","address":"192.168.1.50","prefix":"24","gateway":"192.168.1.1"}`
changes it through the same 90-second trial as a new network: commit with `PUT`
on `/api/v1/network/wifi`, or it rolls back and the camera reboots. The address must
be private, the prefix from 8 to 30, and the gateway on the same subnet (400
`invalid_address`). It refuses without touching anything, with `409
{"ok":false,"reason":"..."}`, when something else answers on that address
(`address-in-use`), the gateway does not answer (`gateway-silent`), there is no
committed network (`no-network`), or for a restart's reasons. During such a trial
`GET /api/v1/network/wifi` also has `"static"`, the proposed address, and `"gateway"`.
Joining a new network goes back to an automatic address, and `POST` refuses an address in
its body: that goes through `network/ip`, which checks it. Both, and a restart, answer
`409 starting` while a boot is still proving itself, for up to about 4 minutes, because
a rollback or restart then would otherwise count against it.

Authenticated `GET /api/v1/network/quality` is read-only and sampled on demand
(the Network UI refreshes on entry or on pressing Refresh; there is no polling).
Example response:

```json
{"wifi":{"associated":true,"ssid":"Family Room WiFi","signal_dbm":-48,"rate_mbps":72.2,"frequency_mhz":5180,"band":"5 GHz","address":"192.0.2.10","gateway":"192.0.2.1","rx_bytes":"123456","tx_bytes":"234567","rx_packets":"1400","tx_packets":"1500"},"ethernet":{"link":false,"speed_mbps":null,"rx_bytes":"0","tx_bytes":"0","rx_packets":"0","tx_packets":"0"}}
```

`associated` and `link` are booleans or `null` when status is unavailable.
SSID is emitted only for a completed association, capped at 64 characters and
sanitized to a limited printable subset (`?` substitutes unsupported bytes).
`signal_dbm`, `rate_mbps`, `frequency_mhz` and `speed_mbps` are numbers or `null`
if absent; Wi-Fi signal/rate are null when not associated, Ethernet speed is null
when down. `band` is `"5 GHz"`, `"2.4 GHz"` or `null`. `address` is the camera's
address on Wi-Fi, and `gateway` the one the supervisor recorded when it pruned the
default route; either may be `null`.
Counters are decimal strings (to avoid JavaScript integer precision loss), or
`null` when unreadable; they are cumulative since boot and can wrap/reset.
Only fixed `wlan0` and `eth0` are inspected. No BSSID/MAC, passphrase, PSK, account credentials, or unfiltered tool output is returned.
The endpoint requires an administrator session and responds with 502
`integration_failed` if its integration helper fails.

## Signed updates

Release stages carry a complete SHA-256 inventory signed with deterministic
ECDSA P-256/SHA-256. The device verifier pins the public key and target ID,
checks every staged file, enforces the exact hardware/ABI hashes, and rejects a
release sequence that is not strictly newer. The outer OEM MD5 remains carrier
format/corruption metadata; it is not the authenticity boundary.

The update API separates staging from apply. `POST` stores and verifies the
package; `PUT` with its `id` mounts the verified payload again, re-checks its
signature, metadata and sequence, runs the package's own `upgrade.sh`, and
reboots the camera. The OEM updater (GoAhead) is used only for the very first
install; afterwards GoAhead is killed every second. An uninstall artifact is
separately signed and declares destructive-removal semantics; it is not a
state rollback. This API updates the platform only; the media layer is updated
by its own installer (see [Building and releasing](BUILDING.md#media-layer)).

## Errors and secrets

Errors use an appropriate HTTP status and a JSON body with a stable code and a
human-readable message. Responses and logs must not expose passwords, session
tokens, cookies, Wi-Fi passphrases, private keys, raw OEM replies containing
secrets, or filesystem paths that disclose them.
