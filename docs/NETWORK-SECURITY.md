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
| 80/tcp | HTTP | UI, `/api/v1/` and ONVIF `/onvif/` | Authenticated (session, or a WS-Security digest for ONVIF); unencrypted, see [Plain HTTP](#plain-http) |
| 5353/udp | mDNS/DNS-SD | Host and HTTP/SSH/RTSP/camera discovery | Link-local multicast only |
| 22/tcp | SSH | Administrative shell | Synchronized `admin` password; optional keys |
| 554/tcp | RTSP | Local video | TCP transport; restrict to approved viewers |

The runtime removes IPv4/IPv6 default routes, filters public resolvers, kills
public GoAhead/telnet after the boot hook runs, and confines the exact
`jooanipc` binary. It does not add a kernel-wide firewall. Connected local
routes and explicitly configured RFC1918/ULA routes remain. The VLAN/router is
the device-wide inbound and outbound boundary.

The runtime supplies continuous `telnetd` suppression, password-synchronized
SSH with optional keys, the Web UI on TCP/80, embedded DNS-SD, and an
exact-hash `jooanipc` containment DSO. The daemon owns public RTSP/TCP
554 and requires Digest authentication; the guard remaps the retained OEM RTSP
service to loopback TCP/8554 for the proxy. The OEM ONVIF
service accepts any credentials, so the guard keeps it on loopback TCP/8899.
NVRs use the daemon's authenticated ONVIF subset on port 80 instead (see
[API](API.md#onvif-for-nvrs)).

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

## First-run credentials

Fresh generic installations begin with `admin` / `admin`, a publicly known
value. It remains valid, setup is not forced, and the Web UI does not warn
about it; the System zone reports whether it is still in use. Rotate it. The same password is synchronized to SSH and RTSP; optional Ed25519
keys supplement SSH password authentication. Never place a fresh unit on a
shared LAN.

## Plain HTTP

The Web UI, `/api/v1/` and ONVIF are plain HTTP on TCP/80. Releases up to
0.2.17 served them over HTTPS with a per-device certificate. That was dropped:
the camera is only reached from its own isolated network, day-to-day use is an
NVR speaking ONVIF, and the certificate meant either a browser warning or a
public name and a renewal job to keep alive.

What crosses the network in the clear:

- the administrator password, once per sign-in and on a password change. It is
  also the SSH, RTSP and ONVIF password;
- the session cookie, on every request. Whoever reads it acts as the
  administrator until the session ends or the password changes;
- whatever a request changes, such as a new Wi-Fi passphrase or SSH keys.

ONVIF and RTSP never send the password itself, only digests of it. Their
video and commands were never encrypted.

So sign in only from a management host on the camera's trusted segment, or
through a VPN that encrypts the path, never across a network you do not
control. The per-device key the camera still generates only serves TLS on the
loopback MQTT sink that `jooanipc` connects to. Never ship or clone it across
cameras.

A camera upgraded from an HTTPS release discards an enrolled certificate and
its key at the first start. Browsers that opened the HTTPS page by name
(`<hostname>.local` or an enrolled public name) remember that for up to a year
(HSTS) and keep asking for `https://`, which no longer answers. Use the address
instead, which HSTS never applies to, or clear the name's HSTS entry in that
browser.

## Wi-Fi

The generic package contains no SSID or passphrase. It preserves the compatible
camera's existing OEM Wi-Fi configuration. Configure any replacement network
manually over a trusted wired or otherwise isolated management path. Confirm
that the new network has the same router isolation before removing the old
path.

The committed network lives in the retrofit's state, not the OEM's: the
supervisor adds it to jooanipc's wpa_supplicant once the control socket answers.
jooanipc can restart wpa_supplicant from its own stored network (about two
minutes into boot, and when it decides the link has failed), so the supervisor
applies the committed network again to every new wpa_supplicant process. Each
apply also disables every other network there, including jooanipc's stored one
(on the verified unit an open, hidden-SSID bench network), so the camera neither
probes for it by name nor joins an open network that takes that name. The
SKW6316 does not look for any other network by itself; without that re-apply it
hunts for the OEM network until a power cycle.

The SKW6316's own firmware can also crash (the driver logs an assert ending in
`DUMPDONE`, then refuses every transmit), which leaves the camera associated but
silent, and nothing on the board resets the chip. The supervisor therefore
reboots the camera when the gateway stops answering ARP for 5 minutes, or for
30 seconds once the chip has logged a crash. It records the reason and the
kernel log under `jooan-local/netdog-*.log` on the microSD card first. A network
that never worked in the current boot is given 15 minutes of uptime first, and
each watchdog reboot doubles that wait (up to 4 hours) until the network has
stayed up for 30 minutes, so an absent network cannot make the camera reboot in
a loop. It never acts during maintenance or a Wi-Fi trial.
`tools/wifi-deadman.sh` records the Wi-Fi state in detail when this needs
diagnosing again.

Avoid recovery designs that depend only on Wi-Fi. Keep a verified flash backup
and external programmer path.

## Verification checklist

After installation and after each update:

1. reboot while packet capture runs on the isolated router segment;
2. record the duration and reachability of any early OEM listener;
3. verify only the intended steady-state TCP ports are reachable;
4. verify nothing answers on TCP/443;
5. verify login throttling and session expiry;
6. verify SSH `admin` accepts the synchronized password and optional keys;
7. verify DNS-SD records, direct RTSP, and ONVIF authentication;
8. verify no IPv4/IPv6 default route or public resolver remains;
9. verify attempted vendor cloud/P2P connections cannot leave the VLAN;
10. test IPv4 and IPv6 separately.
