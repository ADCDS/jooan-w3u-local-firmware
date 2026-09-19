# Local-firmware verification plan

This plan separates host-only gates from hardware acceptance. Host tests must
pass before anything is copied to the camera. Hardware tests use volatile files,
bounded execution, captured logs, and the established cold-recovery procedure.

## Always-on host gates

- Reject credentials, private keys, opaque firmware images, kernel modules,
  object archives, and vendor executables from source control.
- Verify every compatibility-critical source/schema named in
  `ci/compatibility-hashes.sha256` byte-for-byte.
- Parse every shell script with its declared interpreter.
- Keep the installed persistent payload at or below **184320 bytes (180 KiB)**.
- Build release artifacts twice with `SOURCE_DATE_EPOCH=0`, `TZ=UTC`, `LC_ALL=C`,
  a fresh `BUILD_OUT`, and compare hashes plus executable modes. Build scripts
  must not embed the build path, hostname, current time, uid/gid, directory
  enumeration order, or random archive metadata.

`ci/check.sh` runs the light gates. A release CI job supplies
`PERSISTENT_ROOT`, `REPRO_BUILD_CMD`, and `REPRO_ARTIFACT` after its build.

## API contract

The API suite should run against a disposable host instance first, then against
the camera over its isolated LAN. Generate endpoint cases from the checked-in
OpenAPI/schema when one exists; pin that schema in the compatibility manifest.

Required cases:

1. A health/version endpoint is bounded, contains no secrets, and reports the
   exact build identity.
2. Every state-changing endpoint rejects missing, malformed, expired, and
   replayed credentials. Read endpoints explicitly declare whether anonymous
   LAN access is intended.
3. Unsupported methods, invalid content types, oversized bodies, truncated
   JSON, duplicate keys, invalid UTF-8, unknown fields, and numeric boundaries
   return stable 4xx errors without a crash or partial mutation.
4. Concurrent mutations are serialized or conflict explicitly; retries are
   idempotent where documented.
5. Browser-facing authentication uses SameSite cookies and CSRF protection, or
   bearer tokens without ambient-cookie authority. No credential appears in a
   URL, log, process argument, or error response.
6. Authentication failures are rate-limited without letting an unauthenticated
   client exhaust memory, file descriptors, or worker slots.
7. Binding is limited to the intended management interfaces. WAN/cloud egress
   is not needed for API operation.
8. Restart and power-loss tests prove atomic persistence: the old or new valid
   configuration returns, never a half-written credential or network state.

## Authentication state machine

Model states explicitly rather than inferring them from nullable fields:

```text
UNPROVISIONED -> PROVISIONED -> SESSION_AUTHENTICATED
       |              |                 |
       +-> LOCKED <- failed-limit ------+
                      |
               PHYSICAL_RECOVERY
```

Exercise every allowed transition and every forbidden edge. In particular:

- bootstrap credentials are one-use and unavailable after provisioning;
- provisioning races have exactly one winner;
- password/token rotation revokes old sessions;
- logout, expiry, reboot, and clock rollback cannot resurrect a session;
- failed-attempt counters survive service restart when intended but cannot be
  used for permanent remote denial of service;
- recovery requires the documented local/physical condition and never silently
  restores a vendor/default password;
- comparison behavior and response sizes do not disclose which credential
  field was wrong.

## Wi-Fi state machine

Use a fake netlink/process adapter on the host, then repeat critical transitions
with RF isolation on the camera:

```text
BOOT -> LOAD_CONFIG
LOAD_CONFIG -> STA_JOINING | RECOVERY_AP | NETWORK_DISABLED
STA_JOINING -> STA_ONLINE | RETRY_BACKOFF
RETRY_BACKOFF -> STA_JOINING | RECOVERY_AP
STA_ONLINE -> RETRY_BACKOFF | NETWORK_DISABLED
RECOVERY_AP -> STA_JOINING | NETWORK_DISABLED
```

For each transition assert emitted commands, deadlines, bounded exponential
backoff, cancellation of stale timers, and exactly one active DHCP/client/AP
owner. Cover wrong password, absent SSID, deauthentication storms, DHCP timeout,
address conflict, interface disappearance/reappearance, rapid config changes,
service crash, reboot, and power loss. Credentials must be mode 0600, absent
from logs/process arguments, and atomically replaced. Recovery AP must be
time-bounded or explicitly user-controlled, authenticated, and must not add a
WAN route. The Ethernet maintenance address must not silently become a default
gateway or DNS source.

## Network guard vectors

`tests/vectors/network_guard.json` defines fail-closed decisions for loopback,
the wired bench, DHCP, public IPv4/DNS, global IPv6, and unknown interfaces.
When a matching source module appears, `tests/run_optional_vectors.py` requires
an executable JSON-lines adapter (`NETWORK_GUARD_VECTOR_CMD` or the documented
build path). Add vectors for every newly allowed protocol before implementation.

Hardware acceptance also inspects routes, IPv4 and IPv6 rules, listeners, DNS,
and packet capture during boot, link changes, API use, RTSP use, and 30 minutes
idle. Any unsolicited public destination, vendor P2P/cloud packet, fail-open
window, or rule loss after process death is a failure.

## Audio vectors

`tests/vectors/audio.json` covers full-scale mute, enabled identity, empty
frames, range rejection, and silent-boot speaker GPIO. When audio/speaker source
appears, an `AUDIO_VECTOR_CMD` JSON-lines adapter becomes mandatory.

Extend it with the exact device format and golden vectors for endian/channel
mapping, frame fragmentation, gain rounding, saturation (never wraparound),
underrun/overrun recovery, mute transitions without pops, and malformed sizes.
On hardware, bound playback duration and amplitude, begin muted, verify GPIO 63
stays low throughout the historical startup-alarm window, and abort on repeated
unexpected sound, heat, or current anomalies.

## Hardware promotion gates

No host pass alone authorizes camera execution. A hardware trial additionally
requires exact board identity, known-rooted-OEM pre-baseline, physical power
control, verified watchdog custody where needed, volatile payloads, no unknown
media holders, explicit time/byte limits, reverse-order cleanup, captured kernel
logs, and a cold post-test recovery proving both OEM streams and management
access. Persistent installation requires its separate rollback and pre-boot
recovery gates.
