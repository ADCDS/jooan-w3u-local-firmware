# Security policy

## Supported versions

Only the most recent tagged release receives security fixes. Development
snapshots and locally modified images are unsupported. Because this project
depends on retained OEM components, a fix may mitigate or isolate a defect
rather than repair the closed component itself.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting feature in the repository's
**Security** tab. If private reporting is unavailable, contact the repository
owner through the private contact method listed on their GitHub profile and ask
for a disclosure channel. Do not publish an exploit, camera address, serial
number, factory credential, private key, Wi-Fi credential, or flash image in a
public issue.

Include, where safe:

- affected release and package SHA-256;
- the exact hardware identity from `docs/COMPATIBILITY.md`;
- whether the camera had completed first-run setup;
- reachable ports and the network isolation in place;
- minimal reproduction steps and expected impact;
- sanitized logs with tokens, cookies, keys, addresses, and personal data
  removed.

You should receive an acknowledgement within seven days. Please allow time for
hardware reproduction and coordinated release of a fix.

## Known security boundary

This retrofit retains the OEM kernel, vendor modules, and `jooanipc`. Treat
them as untrusted legacy components. The open layer's job is to constrain their
network access and expose a smaller authenticated local interface; it does not
claim to make retained OEM code memory-safe or auditable.

The following conditions are security requirements:

- isolate the camera at the router/VLAN and deny Internet access;
- change the initial `admin` / `change-me-now` credential before normal use;
- use a unique per-device HTTPS key and certificate;
- enroll an SSH public key before enabling key-only SSH;
- never reuse a release image containing another camera's secrets;
- verify package hashes and preserve a tested offline recovery path.

At early boot, the OEM GoAhead listener may become reachable before the
hardening rules are installed. This known exposure is why upstream router
isolation remains mandatory even after installation.

## Out of scope

Reports about unmodified JOOAN firmware, unrelated JOOAN revisions, Thingino,
or third-party clients should be sent to their respective maintainers. Reports
that require intentionally exposing the camera to the public Internet may be
closed with a safer local reproduction request.
