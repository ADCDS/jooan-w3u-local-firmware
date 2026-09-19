# JOOAN W3-U local firmware

This project is building a hardened, local-first retrofit for one verified JOOAN
W3-U hardware revision. It removes the camera's dependency on JOOAN cloud and
P2P services while preserving the vendor media path needed to operate its two
sensors.

This is **not** a complete open-source replacement firmware. The retrofit keeps
the OEM Linux 3.10 kernel, vendor kernel modules, and `jooanipc` as the hardware
media owner. The open layer confines that legacy process, exposes authenticated
local services, and blocks unwanted network access. See
[Architecture](docs/ARCHITECTURE.md) for the trust boundary.

## Exact hardware scope

Use this project only with a unit that matches all of the following:

- product/model reported by the OEM firmware: `JA-A12` / package token `A12`;
- PCB: `JA-6621 V1.0`;
- SoC: Ingenic T23N;
- sensors: dual `cv2005` and `cv2005s1`;
- radio: USB SeekWave SKW6316 (`SV6160LITE` on the verified unit);
- flash: 8 MiB SPI NOR (verified unit: Puya P25Q64HA).

The Thingino image named for “Jooan W3-U” targets a different
SC2336P/ATBM6132U revision and is not interchangeable. Product name and case
shape are not sufficient compatibility evidence. Read
[Compatibility](docs/COMPATIBILITY.md) before building or installing anything.

## Security model

The target release surface is local and authenticated:

- HTTP on TCP/80 redirects to per-device HTTPS on TCP/443;
- the initial administrator is `admin` with temporary password
  `change-me-now`, which must be changed during first-run setup;
- each camera receives its own HTTPS identity; release artifacts never contain
  a shared private key;
- SSH on TCP/22 is key-only and is enabled only after key enrollment;
- RTSP over TCP/554 remains available for local video clients;
- a configurable `.local` hostname follows DHCP address changes through mDNS;
- outbound cloud/P2P traffic is denied by the hardened policy.

The generic image does not contain Wi-Fi credentials. It preserves the
compatible unit's existing OEM Wi-Fi configuration, and replacement Wi-Fi
credentials must be entered manually over a trusted local connection.

There is a known early-boot interval in which the retained OEM GoAhead service
may listen before the hardening policy is active. Keep the camera on a router
VLAN or physically isolated network that blocks untrusted clients and Internet
access. Host firewalling is defense in depth, not a substitute for router
isolation. See [Network security](docs/NETWORK-SECURITY.md).

## Repository contents

- `build.sh` and `build-uninstall.sh`: reproducible release entry points;
- `packaging/`: exact-target manifest and IronMan package assembly;
- `src/`: open source included in the retrofit;
- `tests/`: host-side validation;
- `docs/`: installation, recovery, architecture, API, and security guidance.

`release.sh` builds the deployable MIPS/uClibc runtime, assembles the audited
install/uninstall stages, and emits both packages under `dist/`. It requires
`TOOLCHAIN_ROOT` and a private compatible `OEM_ROOTFS` extraction for link-time
ABI libraries; neither is redistributed. The package is model- and hash-gated
and remains subject to the OEM IronMan limit of `0x200001` bytes.

The repository and its published releases intentionally contain no factory
secrets, Wi-Fi credentials, device keys, flash dumps, OEM firmware, vendor
kernel modules, sensor IQ files, radio firmware, or other vendor binaries.
Where a retained OEM component is required, the build/install workflow must use
files already present on the owner's compatible camera or inputs supplied
privately by that owner.

## Start here

1. Confirm every hardware identifier in [Compatibility](docs/COMPATIBILITY.md).
2. Put the camera on an isolated recovery network.
3. Preserve a complete flash backup before modifying the unit.
4. Read [Recovery](docs/RECOVERY.md); this release writes only `/opt`, and a
   failed first runtime falls back to the OEM updater on the next boot.
5. Follow [Installation](docs/INSTALL.md).
6. Change `admin` / `change-me-now` immediately and verify the port and outbound
   traffic policy.

Do not expose an unprovisioned or freshly rebooted camera directly to the
Internet or to an untrusted LAN.

## Status and limitations

The repository now builds a complete generic IronMan image with per-device
HTTPS, a local web UI/API, transactional Wi-Fi, key-only SSH, `jooanipc`
egress containment, configurable mDNS discovery, snapshots/PTZ integration contracts, and browser
microphone/push-to-talk plumbing. Host, native, MIPS/uClibc and QEMU TLS/MQTT
tests pass. The current `0.1.0` artifact remains a pre-release until install,
Wi-Fi, snapshot, PTZ and audio are qualified on the physical unit.

This is a hardware-specific retrofit for experienced users. Keeping
`jooanipc` preserves proven dual-sensor video operation, but also retains a
large, proprietary, unaudited process. Confinement reduces its network reach;
it does not make the process trustworthy. Power-loss-safe installation,
rollback, first-boot credential rotation, and the documented network policy are
release requirements, not optional hardening suggestions.

See [Third-party components](docs/THIRD_PARTY.md) for licensing boundaries and
[Security policy](SECURITY.md) for reporting vulnerabilities.

## License

Original code and documentation in this repository are licensed under
[GPL-2.0-only](LICENSE). No license is granted here for OEM or third-party
artifacts that are not included in the repository.
