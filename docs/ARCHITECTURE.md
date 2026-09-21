# Architecture

## Design choice

The retrofit is deliberately incremental. The verified JA-A12 has a customized
T23 dual-sensor pipeline for which there is no complete redistributable open
replacement. Replacing that pipeline would sacrifice the known-working
CV2005/CV2005S1 configuration. Version 1 therefore keeps the OEM media stack
and places a hardened local control plane around it.

```text
Local browser / NVR / administrator
             |
     HTTPS API and RTSP/TCP
             |
  open gateway + policy services
     |                    |
authenticated control   route/process policy
     |                    |
     +---- retained jooanipc ---- vendor IMP/ISP/VPU
                                      |
                          cv2005 + cv2005s1 sensors

Bootloader -> OEM Linux 3.10 -> vendor modules -> retrofit supervisor
```

## Retained layer

The following remain device-supplied and proprietary:

- bootloader and OEM Linux 3.10 kernel;
- ISP, VPU, sensor, audio, motor, and SeekWave kernel modules;
- sensor IQ/calibration and radio firmware;
- `jooanipc`, retained as the media-hardware owner.

These components are not imported into this repository or a generic release.
They remain on the owner's camera. Their behavior is treated as an untrusted
dependency behind a narrow boundary.

## Open retrofit layer

The open layer is responsible for:

- model-gated, transactional installation and uninstall packages;
- local HTTPS termination and authenticated session handling;
- the versioned `/api/v1/` management contract;
- password-synchronized SSH for user `admin`, with optional authorized keys;
- local RTSP reachability;
- no-default-route local networking plus exact-process egress containment;
- embedded mDNS/DNS-SD discovery;
- health checks, update verification, and rollback/fallback handling.

The generic image does not personalize Wi-Fi or embed keys. Per-device HTTPS
material, the administrator credential, optional SSH authorized keys, and
manual Wi-Fi settings live in device-local persistent state and are excluded
from release artifacts. The public initial password remains usable until
changed; warning state, rather than forced setup, records that condition.

## Current implementation boundary

The implementation includes deterministic signed packaging, a compressed
controller core plus one compressed runtime and separate SSH recovery, telnet
suppression, transactional Wi-Fi, password-synchronized Dropbear with optional
keys, the HTTPS daemon and Web UI, main/sub fMP4, a local TLS MQTT sink, microphone
and press-to-talk routing, PTZ jog/stop/home/presets, embedded DNS-SD, and an
exact-binary `LD_PRELOAD` guard. The guard redirects only approved OEM service
names to loopback and denies other `jooanipc` connect/datagram traffic.
It also confines OEM RTSP to loopback TCP/8554, suppresses OEM speaker output,
and grants the amplifier only to bounded local talkback. An authenticated
Digest proxy exposes RTSP to local clients on TCP/554.

In steady state, the compressed controller core, SSH recovery bundle, and one
runtime must total no more than 192512 bytes (188 KiB), with at least 69632 bytes
(68 KiB) free. Updates are maintenance transactions, not A/B coexistence: after
the controller and recovery path are durable, selection becomes `- - 0`, the old
persistent runtime is removed, and one signed candidate is staged while at least
57344 bytes (56 KiB) remain free. This exceeds the OEM 50 KiB cleanup threshold.
Failure leaves controller-owned SSH recovery; successful health promotion selects
the candidate and restores the 68 KiB steady reserve.

No kernel-wide firewall is claimed. The supervisor repeatedly removes IPv4 and
IPv6 default routes and filters public resolvers; explicitly configured
RFC1918/ULA and connected routes remain. DHCP, SSH and the open daemon are
separate processes, while the router/VLAN remains the hard device-wide boundary.

## Boot and network sequencing

The OEM boot sequence can launch GoAhead before `/opt/etc/local.rc` runs. That
early interval is immutable without replacing earlier OEM boot components. The
runtime terminates the public listener and optionally reopens it on loopback
for snapshots, but cannot eliminate that immutable early interval. The
external router or VLAN must block untrusted peers and Internet access before
power is applied.

For a conforming promoted release, the intended listeners are TCP/80 (redirect only), TCP/443,
TCP/554, and—after key enrollment—TCP/22. No cloud or P2P path is part of the
supported architecture.

## Package format

The host tooling assembles the reviewed runtime and wraps it as an OEM IronMan update package.
The exact target manifest records `JA-A12`, package token `A12`, T23N, both
sensors, SKW6316, the `0x200001` payload limit, and the format's `0x60`-byte
header/trailer properties. Install and uninstall packages have separate staging
trees and hashes.

The carrier is only a delivery mechanism. The inner stage has a complete
SHA-256 inventory signed with deterministic ECDSA P-256/SHA-256 and verified
against a pinned public key on the camera. Release sequences reject downgrade
and replay. The included installer also performs exact model/retained-component
preflight, single-runtime maintenance staging, health validation, and SSH recovery.

The `0.1` migration is journaled. It recognizes the earlier manual-admin or
developer-launcher `/opt/open` trees, preserves validated SSH identity material
where available, publishes the compressed replacement, and retires the
recognized predecessor only after activation. Unknown or partial predecessor
trees fail closed. Expanded in-place controller layouts require their explicit
backed-up migration rather than silent deletion.

## Non-goals

Version 1 does not claim:

- a new kernel or bootloader;
- an open CV2005 sensor/ISP implementation;
- removal or auditability of `jooanipc`;
- compatibility with every product sold as W3-U;
- safety on an Internet-facing network.
