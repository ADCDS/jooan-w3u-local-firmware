# Network security

## Required upstream isolation

Place the camera in a dedicated VLAN or behind a router interface whose policy
denies:

- camera-to-Internet traffic;
- traffic from untrusted LAN, guest, and IoT clients to the camera;
- camera-initiated connections to other LAN devices, except explicitly needed
  local NVR, DNS, and time services (UDP/123 to the configured NTP server);
- IPv6 paths that would bypass equivalent IPv4 restrictions.

Allow administration only from named management hosts. Allow the media
daemon's video and ONVIF ports only from the NVR/viewers that need them. Keep
this policy active during installation, every reboot, and recovery.

## Listeners by boot mode

The [boot modes](ARCHITECTURE.md#boot-modes) are described in the
architecture. In a native boot, as observed on the verified unit:

| Listener | Process | Purpose and policy |
|---|---|---|
| `0.0.0.0:80/tcp` | `joan-daemon` | `/api/v1/`; authenticated session, unencrypted (see [Plain HTTP](#plain-http)), [Host check](#host-and-origin-checks). With the media layer installed its pages redirect to `https://<host>/`; without it, the platform's own Web UI |
| `0.0.0.0:443/tcp` | `jooanipc-open` | The admin UI: public page files, the platform session relayed to `127.0.0.1:80` (cookie, CSRF token and same-origin checks), and the media API |
| `0.0.0.0:22/tcp`, `[::]:22/tcp` | `dropbear` | SSH; `admin` password from the start, optional Ed25519 keys |
| `0.0.0.0:5353/udp` | `joan-daemon` | mDNS/DNS-SD; answers only peers on a directly attached IPv4 subnet |
| `127.0.0.2:1883/tcp` | `joan-daemon` | Loopback TLS MQTT sink for the OEM application; idle in native mode |
| `127.0.0.4:9443/tcp` | `joan-daemon` | Loopback stub that accepts and closes the OEM application's port-443 connections; idle in native mode |
| `0.0.0.0:18443/tcp` | `jooanipc-open` | HTTPS REST and ONVIF/PTZ; HTTP Basic or WS-Security |
| `0.0.0.0:18554/tcp` | `jooanipc-open` | RTSP; Digest authentication |

The media daemon binds the address set by `bind` in its `camera.ini`. The
deployed unit uses `0.0.0.0`, so the admin UI stays reachable while a Wi-Fi or
address change is on trial; the camera has no route to anything but its own
network and the allowed ones.
RTSP clients that choose RTP over UDP get per-session UDP ports. Nothing
listens on TCP/554, 8554, 8899 or 443 in a native boot.

The fallback modes change this table:

- **Handoff:** until the handoff stops it, the OEM application adds
  loopback-only listeners, among them its RTSP (which it asks for on TCP/554;
  the guard moves it to `127.0.0.1:8554`) and its ONVIF on `127.0.0.1:8899`.
  After the handoff the table is the native one.
- **OEM rescue and platform-only:** the OEM application's loopback-only
  listeners stay for the whole uptime, and the media daemon's two listeners are
  absent.

The guard rewrites every IPv4/IPv6 bind of the OEM application to loopback,
so none of its services is reachable from the network. The OEM RTSP is
authenticated with a random secret (`config/rtsp.password`) that the platform
regenerates at every `joan-daemon` start and every password change and pushes
to the OEM application over the MQTT bridge; the guard also refuses
non-loopback RTSP connections while that secret is not confirmed. The OEM
ONVIF service accepts any credentials, which is why it must stay on loopback.

The runtime removes IPv4/IPv6 default routes, filters public resolvers and
kills `telnetd` and GoAhead every second, in every boot mode. It does not add a
kernel-wide firewall. Connected local routes and explicitly configured
RFC1918/ULA routes remain. The VLAN/router is the device-wide inbound and
outbound boundary.

## Host and origin checks

Every request to TCP/80, sign-in and static files included, must carry a
`Host` header that is one of:

- an IPv4 literal in 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 or 127.0.0.0/8,
  with or without a port;
- a bracketed IPv6 loopback or ULA (`fc00::/7`) literal;
- the configured mDNS label, bare or under one of the private-use suffixes
  `.local` (mDNS), `.lan` (the usual router DHCP domain), `.home.arpa`
  (RFC 8375) or `.internal`, with or without a port or a trailing dot. For the
  label `camera1` that is `camera1`, `camera1.local`, `camera1.lan`,
  `camera1.home.arpa` and `camera1.internal`.

Anything else gets `403 host_rejected` before any handler runs. This blocks DNS
rebinding: a page on an attacker's domain cannot address the camera by a name
that resolves to it, and none of the accepted suffixes resolves in public DNS.
A router name works only when its first label is the camera's mDNS label, so
keep the DHCP reservation name and the mDNS label the same (set the label
under Network → Local name). A renamed mDNS label
takes effect for this check after the next `joan-daemon`
restart, together with the responder. On the authenticated API routes, a
request that carries an `Origin` header must also match `http://<Host>`, or it
gets `403 origin_rejected`.

## Early-boot OEM exposure

The retained OEM startup exposes GoAhead before `/opt/etc/local.rc` runs.
The runtime kills it every second from then on and never reopens it.
Treat the period from
power-on until a qualified release's health status reports hardened policy as
hostile. Do not depend on the camera's own firewall to protect this interval.
Router/VLAN isolation is the controlling security boundary.

If an unexpected OEM listener remains after startup, disconnect the camera,
collect sanitized diagnostics locally, and recover or update it. Do not forward
the port as a workaround.

## First-run credentials

Fresh generic installations begin with `admin` / `admin`. It remains valid
and setup is not forced; nothing is locked until it is changed. The Web UI
shows no banner: the System page shows the password state ("initial value" or
"changed"), and `/api/v1/setup/status`, sign-in and `/api/v1/status` report
`default_password_warning`. The same password is synchronized to SSH;
optional Ed25519 keys supplement SSH password authentication. Never place a
fresh unit on a shared LAN.

## Plain HTTP

The Web UI and `/api/v1/` are plain HTTP on TCP/80. Releases up to
0.2.17 served them over HTTPS with a per-device certificate. That was dropped:
the camera is only reached from its own isolated network, day-to-day use is an
NVR speaking ONVIF, and the certificate meant either a browser warning or a
public name and a renewal job to keep alive.

What crosses the network in the clear:

- the administrator password, once per sign-in and on a password change. It is
  also the SSH password;
- the session cookie, on every request. Whoever reads it acts as the
  administrator until the session ends or the password changes;
- whatever a request changes, such as a new Wi-Fi passphrase or SSH keys.

So sign in only from a management host on the camera's trusted segment, or
through a VPN that encrypts the path, never across a network you do not
control. The per-device key the platform still generates only serves TLS on
the loopback MQTT sink that the OEM application connects to in the fallback
modes. Never ship or clone it across cameras. The media daemon's HTTPS on
TCP/18443 uses its own certificate and key in `/opt/jooanipc-open/`.

A camera upgraded from an HTTPS release discards an enrolled certificate and
its key at the first start. Browsers that opened the HTTPS page by name
(`<hostname>.local` or an enrolled public name) remember that for up to a year
(HSTS) and keep asking for `https://`, which no longer answers. Use the address
instead, which HSTS never applies to, or clear the name's HSTS entry in that
browser. A public name is refused by the [Host check](#host-and-origin-checks)
in any case.

## Network time

Network time is off by default. When set, `joan-daemon` sends SNTP requests on
UDP/123 to the gateway or to the configured IPv4 address. Replies must echo a
random origin cookie and pass header, NTP-era and timing checks, but SNTP is
otherwise unauthenticated: a host on the path can still move the camera's
clock. Use a server on the camera's own network, and let the router/VLAN allow
UDP/123 from the camera to that server only.

## Wi-Fi

The generic package contains no SSID or passphrase. Configure the camera's
network manually over a trusted wired or otherwise isolated management path,
as a transactional stage and commit (`/api/v1/network/wifi`). The committed
network lives in the platform's state
(`/opt/custom/jooan-local/config/wifi.json`), not the OEM's. Confirm that the
new network has the same router isolation before removing the old path.

In a native boot the OEM's stored Wi-Fi configuration is not used at all:
`local.rc` starts `wpa_supplicant` with an empty network list, and the
supervisor adds the committed network once the control socket answers. A
native boot without a committed network cannot reach its gateway, fails after
300 s and latches `native-failed`; the handoff needs the committed network as
well, so the camera then stays in OEM rescue mode. With a committed network, a
router that is down only makes it wait, and restart now and then, in native boot. The OEM configuration stays
on flash, untouched, for the fallback modes.

When the OEM application runs, it owns `wpa_supplicant` and can restart it
from its own stored network (about two minutes into boot, and when it decides
the link has failed), so the supervisor applies the committed network again to
every new `wpa_supplicant` process. Each apply also disables every other
network there, including the OEM's stored one (on the verified unit an open,
hidden-SSID bench network), so the camera neither probes for it by name nor
joins an open network that takes that name. The SKW6316 does not look for any
other network by itself; without that re-apply it hunts for the OEM network
until a power cycle.

After each apply a persistent `udhcpc -b` keeps renewing the DHCP lease.

The SKW6316's own firmware can also crash (the driver logs an assert ending in
`DUMPDONE`, then refuses every transmit), which leaves the camera associated but
silent, and nothing on the board resets the chip. The supervisor therefore
reboots the camera when the gateway stops answering ARP for 5 minutes, or for
30 seconds once the chip has logged a crash. It records the reason and the
kernel log under `jooan-local/netdog-*.log` on the microSD card first, when
one is mounted. A network that never worked in the current boot is given 15
minutes of uptime first, and each watchdog reboot doubles that wait (up to 4
hours) until the network has stayed up for 30 minutes, so an absent network
cannot make the camera reboot in a tight loop. It never acts during maintenance or a
Wi-Fi trial. `tools/wifi-deadman.sh` records the Wi-Fi state in detail when
this needs diagnosing again.

Avoid recovery designs that depend only on Wi-Fi. Keep a verified flash backup
and external programmer path.

## Verification checklist

After installation and after each update:

1. reboot while packet capture runs on the isolated router segment;
2. record the duration and reachability of any early OEM listener;
3. verify only the listeners in [the table above](#listeners-by-boot-mode)
   for the current boot mode are present and reachable;
4. verify nothing answers on TCP/443, 554, 8554 or 8899 from the network;
5. verify login throttling, session expiry and that a request with a foreign
   `Host` gets `403 host_rejected`;
6. verify SSH `admin` accepts the synchronized password and optional keys;
7. verify the DNS-SD records (HTTP, SSH and `_jooan-camera._tcp`, no RTSP);
8. verify no IPv4/IPv6 default route or public resolver remains;
9. verify attempted vendor cloud/P2P connections cannot leave the VLAN;
10. test IPv4 and IPv6 separately.
