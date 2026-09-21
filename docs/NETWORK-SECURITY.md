# Network security

## Required upstream isolation

Place the camera in a dedicated VLAN or behind a router interface whose policy
denies:

- camera-to-Internet traffic;
- traffic from untrusted LAN, guest, and IoT clients to the camera;
- camera-initiated connections to other LAN devices, except explicitly needed
  local NVR, DNS, and time services;
- IPv6 paths that would bypass equivalent IPv4 restrictions.

Allow administration only from named management hosts. Allow RTSP only from the
NVR/viewers that need it. Keep this policy active during installation, every
reboot, and recovery.

## Target listener policy

| Port | Protocol | Purpose | Policy |
|---:|---|---|---|
| 80/tcp | HTTP | Redirect to HTTPS | No application or credential exchange |
| 443/tcp | HTTPS | UI and `/api/v1/` | Authenticated; unique per-device TLS identity |
| 5353/udp | mDNS/DNS-SD | Host and HTTPS/SSH/RTSP/camera discovery | Link-local multicast only |
| 22/tcp | SSH | Administrative shell | Synchronized `admin` password; optional keys |
| 554/tcp | RTSP | Local video | TCP transport; restrict to approved viewers |

The runtime removes IPv4/IPv6 default routes, filters public resolvers, kills
public GoAhead/telnet after the boot hook runs, and confines the exact
`jooanipc` binary. It does not add a kernel-wide firewall. Connected local
routes and explicitly configured RFC1918/ULA routes remain. The VLAN/router is
the device-wide inbound and outbound boundary.

The runtime supplies continuous `telnetd` suppression, password-synchronized
SSH with optional keys, HTTPS on 443, the port-80 redirect, embedded DNS-SD,
and an exact-hash `jooanipc` containment DSO. The daemon owns public RTSP/TCP
554 and requires Digest authentication; the guard remaps the retained OEM RTSP
service to loopback TCP/8554 for the proxy and fMP4 ingestion.

## Early-boot OEM exposure

The retained OEM startup exposes GoAhead before `/opt/etc/local.rc` runs.
The runtime kills it and optionally reopens it on loopback for snapshots.
Treat the period from
power-on until a qualified release's health status reports hardened policy as
hostile. Do not depend on the camera's own firewall to protect this interval.
Router/VLAN isolation is the controlling security boundary.

If an unexpected OEM listener remains after startup, disconnect the camera,
collect sanitized diagnostics locally, and recover or update it. Do not forward
the port as a workaround.

## First-run credentials and HTTPS

Fresh generic installations begin with `admin` / `admin`, a publicly known
value. It remains valid, setup is not forced, and the Web UI does not warn
about it; the System zone reports whether it is still in use. Rotate it. The same password is synchronized to SSH and RTSP; optional Ed25519
keys supplement SSH password authentication. Never place a fresh unit on a
shared LAN.

HTTPS keys are generated or enrolled per device. Never ship or clone a private
key across cameras.

The generated identity is self-signed, and its only subject alternative name is
the camera's `<hostname>.local`. Two consequences worth knowing before handing
the page to anyone else:

- it cannot be reached without a warning at `https://<ip>/`, because there is no
  IP entry in the certificate, and no amount of trusting changes that;
- it cannot be installed as a trusted authority, because it is `CA:FALSE`.

A browser warning is not permission to click through blindly: verify the
fingerprint over the trusted installation channel, then pin that specific
device.

### Enrolling a certificate an ordinary device already trusts

For a camera that family phones or a television will use, enroll a certificate
from an authority those devices already trust, and the warning disappears with
nothing to install on them.

The camera has no route off the LAN, so it cannot answer an ACME HTTP
challenge. Solve a **DNS-01** challenge elsewhere -- it needs no inbound access
to anything -- and hand the result over:

    certbot certonly --dns-<provider> -d cam.example.com
    cat .../privkey.pem .../fullchain.pem > bundle.pem
    tools/enroll-certificate.sh --host cam.example.com --bundle bundle.pem --restart

Point that name at the camera's LAN address in your own DNS. A public record
holding a private address also works, but some resolvers refuse to return one
(DNS rebinding protection), so a local override is the reliable choice.

`PUT /api/v1/tls/identity` takes the bundle, checks that the key matches the
certificate and that the dates are current, and refuses anything else. An
enrolled identity is never regenerated over -- including by an mDNS rename,
which does regenerate a self-signed one. `DELETE` returns the camera to a
generated identity. The listener reads its certificate once at startup, so a
renewal takes effect on restart, which is what `--restart` asks for.

## Wi-Fi

The generic package contains no SSID or passphrase. It preserves the compatible
camera's existing OEM Wi-Fi configuration. Configure any replacement network
manually over a trusted wired or otherwise isolated management path. Confirm
that the new network has the same router isolation before removing the old
path.

Avoid recovery designs that depend only on Wi-Fi. Keep a verified flash backup
and external programmer path.

## Verification checklist

After installation and after each update:

1. reboot while packet capture runs on the isolated router segment;
2. record the duration and reachability of any early OEM listener;
3. verify only the intended steady-state TCP ports are reachable;
4. verify TCP/80 only redirects and sends no sensitive content;
5. verify HTTPS identity, login throttling, and session expiry;
6. verify SSH `admin` accepts the synchronized password and optional keys;
7. verify DNS-SD records and direct RTSP/fMP4 behavior;
8. verify no IPv4/IPv6 default route or public resolver remains;
9. verify attempted vendor cloud/P2P connections cannot leave the VLAN;
10. test IPv4 and IPv6 separately.
