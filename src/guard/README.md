# jooanipc containment guard

`libjooan_guard.so` is an `LD_PRELOAD` containment layer for one exact OEM
`jooanipc` binary. It activates only when `/proc/self/exe` has SHA-256
`edd1afa9f89f74f60d23fc56a1347f9b406400d38a23aa46ebcc45983fd09355`.
Executables started by `jooanipc` inherit the preload variable but do not match
the hash, so the library is inert in them. An executable named `jooanipc` with
any other hash exits before `main`; an OEM mismatch cannot silently run without
containment.

The guard is loaded only where the OEM application runs: the handoff, OEM
rescue and platform-only boot modes, through the `jooanipc` wrapper in
`runtime/boot/local.rc`. A native boot never starts `jooanipc`, so the guard
is installed but unused (see [boot modes](../../docs/ARCHITECTURE.md#boot-modes)).

While active, the guard:

- resolves only loopback names and the two approved cloud names:
  `use1mqtt01.jooaniot.com` to `127.0.0.2`, whose connections it redirects to
  the platform's MQTT sink on TCP/1883, and `use1api.jooaniot.com` to
  `127.0.0.3`, whose connections it sends to `127.0.0.1` on the same port.
  Every other name fails to resolve;
- redirects a connection to port 443 on any non-loopback IPv4 address to
  `127.0.0.4:9443`, a platform stub that accepts and closes it, and denies
  every other non-loopback `connect`;
- rewrites every OEM IPv4/IPv6 listener bind to loopback, with no exception;
  the RTSP listener the OEM requests on TCP/554 becomes `127.0.0.1:8554` (or
  `[::1]:8554`), and ONVIF stays on its port, now `127.0.0.1:8899`.
  `test_guard.c` asserts that no external bind is allowed;
- closes accepted RTSP connections on TCP/8554 from non-loopback peers while
  the SHA-256 of `config/rtsp.password` does not match `config/rtsp.synced`.
  Loopback peers are always accepted; with every listener bound to loopback
  this gate is a second line behind the bind rewrite;
- confines `send`, `sendto`, `sendmsg`, `sendmmsg` (also through the exported
  `syscall` entry point), `sendfile64`, `write`, and `writev`, while permitting
  Unix, netlink and loopback sockets and replies from the approved local OEM
  listener ports;
- owns the board's active-low speaker amplifier on GPIO 63: OEM direction and
  value writes are confined to output/muted, and the guard publishes
  `speaker-guard-ready` so the early-boot mute loop in `local.rc` can stop;
- discards OEM DSP playback writes (`O_WRONLY` opens of `/dev/dsp`) and the
  exact AO payload ioctl while reporting successful consumption, so muted
  alarm audio cannot queue and play later. This suppresses the OEM
  motion-detection siren.

Two audio hooks remain from retired features, and no shipped component uses
them:

- the talkback socket `/tmp/jooan-guard-talkback.sock` (0600) still accepts
  the local `JAGD` ACQUIRE/SPEAKER_LEASE/RELEASE protocol and would unmute the
  amplifier only for a valid, contiguous-sequence lease that expires after half
  a second without a packet. Its client, the push-to-talk audio router in
  `src/audio/`, is no longer built for the camera or shipped, so nothing sends
  to it. The platform's runtime health check still requires the socket to
  exist;
- the OEM audio-pause hook on `usleep` and `pthread_cond_timedwait` waits only
  while a lease file exists under `/run/jooan-local/`; since 0.2.22 nothing
  writes one.

The microphone tap and read-only DSP tracking are compiled only into the
test-only library.

Build and run host tests with `make test`. The test-only shared object accepts
fake executable, DSP, and socket paths through environment variables. Those
overrides are excluded from the production shared object.

The host compiler validates behavior, but it does not produce a deployable
camera library. Deployment requires the OEM-compatible MIPS32 little-endian
uClibc 0.9.33.2 toolchain and must verify that `libdl`, pthreads, and the target
loader accept the resulting DSO before enabling `LD_PRELOAD` at boot.

This process shim is defense in depth. The promoted image still needs the
documented persistent no-default-route kernel/router policy. The pinned OEM
libuv contains a direct MIPS `sendmmsg` syscall path, and inline/raw syscalls or
future statically bound code cannot be reliably confined by `LD_PRELOAD` even
when the shim also wraps the exported libc `syscall` entry point.
