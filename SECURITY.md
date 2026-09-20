# Security policy

## Supported versions

There is currently no supported tag. The `0.1.0` line is an engineering
pre-release until the documented physical-camera promotion gates pass. Once a
supported tag exists, only the most recent supported tag will receive security
fixes. Development snapshots and locally modified images remain unsupported.
Because this project depends on retained OEM components, a fix may mitigate or
isolate a defect rather than repair the closed component itself.

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
- sign in initially as `admin` / `change-me-password`; it remains valid until
  changed, so do not ignore the persistent warning and rotate it promptly;
- use a unique per-device HTTPS key and certificate;
- understand that SSH password authentication uses the same synchronized
  `admin` credential as HTTPS; optional Ed25519 keys supplement it;
- keep the camera local-only: the runtime removes IPv4/IPv6 default routes and
  retains only connected/local routing, but the router/VLAN is still the hard
  perimeter;
- install only artifacts whose signed inner manifest verifies against the
  pinned release key and whose release sequence passes anti-replay policy;
- never reuse a release image containing another camera's secrets;
- verify package hashes and preserve a tested offline recovery path.

At early boot, the OEM GoAhead listener may become reachable before
`/opt/etc/local.rc` can run. This interval is immutable in the retained OEM
boot sequence. It is why upstream router isolation remains mandatory even
though the runtime later kills GoAhead, prunes default routes, and confines the
exact `jooanipc` binary.

Uninstall is destructive removal, not restoration of predecessor state. It
removes HTTPS, local routing enforcement, the process guard, and project SSH,
then returns to OEM/GoAhead behavior on the next boot. It intentionally does not
restore the former unauthenticated telnet hook. Keep the camera isolated during
and after uninstall.

## Out of scope

Reports about unmodified JOOAN firmware, unrelated JOOAN revisions, Thingino,
or third-party clients should be sent to their respective maintainers. Reports
that require intentionally exposing the camera to the public Internet may be
closed with a safer local reproduction request.
