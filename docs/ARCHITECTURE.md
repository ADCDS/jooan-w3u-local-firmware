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
authenticated control   firewall/confinement
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
- key-only SSH after administrator enrollment;
- local RTSP reachability;
- deny-by-default outbound and inbound network policy;
- health checks, update verification, and rollback/fallback handling.

The generic image does not personalize Wi-Fi or embed keys. Per-device HTTPS
material, the changed administrator secret, SSH authorized keys, and manual
Wi-Fi settings live in device-local persistent state and are excluded from
release artifacts.

## Current implementation boundary

The implementation includes deterministic packaging, compressed A/B slots,
trial rollback, telnet suppression, transactional Wi-Fi, key-only Dropbear,
the HTTPS daemon and UI, a local TLS MQTT sink, browser audio routing, and an
exact-binary `LD_PRELOAD` guard. The guard redirects only approved OEM service
names to loopback and denies other `jooanipc` connect/datagram traffic.

No kernel-wide firewall is claimed. DHCP, SSH and the open daemon are separate
processes, while the router/VLAN remains the hard device-wide boundary.

## Boot and network sequencing

The OEM boot sequence can launch GoAhead before `/opt/etc/local.rc` runs. The
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

The transport is only a delivery mechanism. The included installer performs
hash/model preflight, compressed slot staging, health validation and rollback.

## Non-goals

Version 1 does not claim:

- a new kernel or bootloader;
- an open CV2005 sensor/ISP implementation;
- removal or auditability of `jooanipc`;
- compatibility with every product sold as W3-U;
- safety on an Internet-facing network.
