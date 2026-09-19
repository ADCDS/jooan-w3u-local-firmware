# Local API v1

## Status of this document

This is the version 1 public contract implemented by `joan-daemon` and the
bundled web UI. A hardware-backed route is usable only
when the running
image advertises the corresponding feature in `GET /api/v1/status`. Builds must
fail closed: an unavailable or not-yet-qualified hardware operation returns an
error and is not silently proxied to an OEM network endpoint.

Snapshot, PTZ, Wi-Fi, audio and update operations remain feature-gated until
their device helper reports healthy; an unavailable operation returns an HTTP
error rather than being silently proxied to an external service.

In a conforming release, all API and WebSocket traffic uses HTTPS on TCP/443.
TCP/80 only redirects and
must not accept credentials or API bodies. Except for the limited first-run
setup namespace, routes require an authenticated administrator session.

## Route summary

| Route | Method/transport | Purpose |
|---|---|---|
| `/api/v1/setup/*` | HTTPS | First-run status, credential rotation, and per-device enrollment |
| `/api/v1/session` | HTTPS | Create, inspect, or end an authenticated session |
| `/api/v1/status` | `GET` | Health, version, hardening state, and feature gates |
| `/api/v1/network/wifi` | HTTPS | Inspect or manually replace Wi-Fi configuration |
| `/api/v1/streams` | `GET` | Enumerate locally available streams and capabilities |
| `/api/v1/snapshot` | `GET` | Return a still image from an available stream |
| `/api/v1/ptz/move` | `POST` | Start a bounded pan/tilt movement |
| `/api/v1/ptz/stop` | `POST` | Stop movement immediately |
| `/api/v1/ssh/authorized-keys` | HTTPS | Enroll, list, or revoke administrator SSH keys |
| `/api/v1/update` | HTTPS | Validate and apply a signed/hashed local update |
| `/api/v1/audio/mic` | WebSocket | Authenticated microphone audio channel |
| `/api/v1/audio/talk` | WebSocket | Authenticated talkback audio channel |

Exact setup subroutes, media encodings, and request schemas are discoverable
from the image version and must remain backward compatible within API v1.
Clients must ignore unknown response fields.

## Authentication and setup

A generic image starts with user `admin` and temporary password
`change-me-now`. Only the minimal `/api/v1/setup/*` and session-creation surface
is reachable in the unprovisioned state. Setup requires a new administrator
secret and establishes a unique per-device HTTPS identity. Normal API, RTSP,
and optional SSH use are gated on completed setup.

`/api/v1/session` is the sole password-handling endpoint outside setup. Clients
must use HTTPS, must not place credentials in URLs, and must honor session
expiry. Browser clients must use the server's CSRF protection for state-changing
requests. Logging must redact passwords, session tokens, cookies, Wi-Fi
passphrases, private keys, and update secrets.

## Feature behavior

### Status

`GET /api/v1/status` is the authoritative readiness endpoint. At minimum it
distinguishes setup-required, starting, hardened/ready, degraded, updating, and
rollback states. It reports whether HTTPS identity, firewall policy, OEM media,
RTSP, PTZ, audio, SSH enrollment, and each sensor-backed stream passed their
health checks. It never returns secrets.

### Wi-Fi

`/api/v1/network/wifi` is a manual configuration interface. Generic images
preserve the existing Wi-Fi setup and do not contain credentials. A network
change must be staged and validated before the working path is discarded.

### Streams, snapshots, PTZ, and audio

Media/control routes operate through the local retrofit boundary while
`jooanipc` remains the hardware owner. They are available only when the runtime
has qualified the corresponding operation on the exact dual-CV2005 unit.
Movement requests must be bounded; clients should always send
`/api/v1/ptz/stop` when releasing a control. Audio WebSockets require an active
authenticated session and explicit user action.

RTSP itself remains on TCP/554 and is not tunneled through this API.

### SSH keys

`/api/v1/ssh/authorized-keys` accepts public keys only. Private keys are never
uploaded. SSH stays disabled until at least one valid key is enrolled, then
listens on TCP/22 with password authentication disabled.

### Updates

`/api/v1/update` accepts only locally supplied, target-compatible update
artifacts whose integrity metadata verifies before any persistent change. The
endpoint must reject the wrong model, oversized payloads, downgrade or replay
violations, and artifacts without a viable rollback path. Update progress and
the terminal health/rollback result are exposed through status, not through an
unauthenticated callback.

## Errors

API errors use an appropriate HTTP status and a JSON object with a stable
machine-readable code plus a human-readable message. Messages must not expose
filesystem paths containing secrets, credentials, tokens, private keys, or raw
OEM responses. Hardware timeouts and unavailable feature gates are errors, not
successful empty responses.
