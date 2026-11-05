# Security policy

## Supported versions

There is currently no supported tag. The `0.2` line (currently 0.2.24,
release sequence 58) is an engineering pre-release until the documented
physical-camera promotion gates pass. Once a supported tag exists, only the
most recent supported tag will receive security fixes. Development snapshots
and locally modified images remain unsupported. Because this project depends
on retained OEM components, a fix may mitigate or isolate a defect rather than
repair the closed component itself.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting feature in the repository's
**Security** tab. If private reporting is unavailable, contact the repository
owner through the private contact method listed on their GitHub profile and ask
for a disclosure channel. Do not publish an exploit, camera address, serial
number, factory credential, private key, Wi-Fi credential, or flash image in a
public issue.

Include, where safe:

- affected release and package SHA-256, and the media release ID;
- the exact hardware identity from `docs/COMPATIBILITY.md`;
- the boot mode (native, handoff, rescue or platform-only);
- whether the camera had completed first-run setup;
- reachable ports and the network isolation in place;
- minimal reproduction steps and expected impact;
- sanitized logs with tokens, cookies, keys, addresses, and personal data
  removed.

You should receive an acknowledgement within seven days. Please allow time for
hardware reproduction and coordinated release of a fix.

## Known security boundary

This retrofit retains the OEM kernel and vendor modules, and keeps the OEM
application `jooanipc` on flash. Treat them as untrusted legacy components;
the project does not claim to make retained OEM code memory-safe or auditable.

A normal (native) boot never starts `jooanipc`. The exposed surface is then
the two open daemons and SSH:

- **`joan-daemon`** (platform): the Web UI and `/api/v1/` over plain HTTP on
  TCP/80, the mDNS responder on UDP/5353, and two loopback listeners that serve
  only the OEM application in the fallback modes. Every HTTP request must name
  the camera in its `Host` header (private or loopback IP literal, or the
  camera's mDNS label, bare or with `.local`, `.lan`, `.home.arpa` or
  `.internal`), which blocks DNS rebinding.
- **`jooanipc-open`** (media): LAN-facing TLS, HTTP and ONVIF SOAP parsers on
  TCP/18443 and an RTSP parser on TCP/18554, with their own credentials. Failed
  logins over HTTP Basic, ONVIF WS-Security and RTSP Digest share a per-address
  throttle: after 10 failures within 60 s of the first, that address is refused
  for the rest of the window (HTTP `429`, RTSP `503` with `Retry-After: 60`)
  without checking credentials. Requests without credentials never count, and
  `/health` is never refused. WS-Discovery on UDP/3702 adds another parser when
  `discovery=1`; the deployed configuration leaves it off.
- **Dropbear** on TCP/22, with the synchronized `admin` password and optional
  Ed25519 keys.

Neither daemon drops root privileges. Both need device access (the media daemon
the ISP, VPU, motor and watchdog devices; the platform network and system
configuration), so a memory-safety bug in either parser is a root compromise.
This is a known limitation, not fixed in this release.

In the handoff, rescue and platform-only modes the OEM application also runs.
There the `LD_PRELOAD` containment guard confines the exact `jooanipc` binary:
it resolves only loopback names and two approved cloud names (to loopback),
denies other non-loopback traffic, and rewrites every listener to loopback.
The guard is defense in depth: raw or inline syscalls cannot be confined by
`LD_PRELOAD`.

There is no packet filter. The platform prunes default routes and non-private
resolvers continuously and kills `telnetd` and GoAhead every second in every
mode; router/VLAN isolation is the real boundary.

The following conditions are security requirements:

- isolate the camera at the router/VLAN and deny Internet access;
- know that the initial `admin` / `admin` stays valid until changed; setup is
  not forced, and the System page and `/api/v1/setup/status` report whether it
  is still in use;
- understand that SSH password authentication uses the same synchronized
  `admin` credential as the Web UI; optional Ed25519 keys supplement it. The
  platform stores it only as PBKDF2-SHA256 and MD5-crypt `$1$`;
- keep the media daemon's credentials and its per-camera TLS key in
  `/opt/jooanipc-open/` private to that camera;
- keep the camera local-only: the runtime removes IPv4/IPv6 default routes and
  retains only connected/local routing, but the router/VLAN is still the hard
  perimeter;
- install only artifacts whose signed inner manifest verifies against the
  pinned release key and whose release sequence passes anti-replay policy;
- never reuse a release image or media bundle containing another camera's
  secrets;
- verify package hashes and preserve a tested offline recovery path.

At early boot, the OEM GoAhead listener may become reachable before
`/opt/etc/local.rc` can run. This interval is immutable in the retained OEM
boot sequence. It is why upstream router isolation remains mandatory even
though the runtime later kills GoAhead and prunes default routes.

Uninstall is destructive removal, not restoration of predecessor state. It
removes the Web UI and API, local routing enforcement, the process guard, and
project SSH, then returns to OEM/GoAhead behavior on the next boot, with the
OEM application running unconfined. The media layer stays on flash but nothing
starts it. Uninstall intentionally does not restore the former unauthenticated
telnet hook. Keep the camera isolated during and after uninstall.

## Out of scope

Reports about unmodified JOOAN firmware, unrelated JOOAN revisions, Thingino,
or third-party clients should be sent to their respective maintainers. Reports
that require intentionally exposing the camera to the public Internet may be
closed with a safer local reproduction request.
