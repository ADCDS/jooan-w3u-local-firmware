# jooanipc containment guard

`libjooan_guard.so` is an `LD_PRELOAD` containment layer for one exact OEM
`jooanipc` binary. It activates only when `/proc/self/exe` has SHA-256
`edd1afa9f89f74f60d23fc56a1347f9b406400d38a23aa46ebcc45983fd09355`.
Executables started by `jooanipc` inherit the preload variable but do not match
the hash, so the library is inert in them. An executable named `jooanipc` with
any other hash exits before `main`; an OEM mismatch cannot silently run without
containment.

While active, the guard:

- resolves only `use1api.jooaniot.com` and `use1mqtt01.jooaniot.com` to
  distinct loopback aliases, routing MQTT to the local TCP/1883 bridge and the
  API to its original local port;
- rewrites OEM IPv4/IPv6 listener binds to loopback except the product RTSP
  contract on TCP/554;
- rejects non-loopback RTSP accepts while the SHA-256 of the configured RTSP
  password does not match `rtsp.synced`; loopback fMP4 ingestion remains
  available during synchronization;
- confines `connect`, `send`, `sendto`, `sendmsg`, `sendmmsg`, `sendfile64`,
  `write`, and `writev`, while permitting Unix sockets and replies from the
  approved local OEM listener ports;
- records an `O_WRONLY` open of `/dev/dsp`;
- receives the local `JAGD` ACQUIRE/PCMA/RELEASE datagram protocol on
  `/tmp/jooan-guard-talkback.sock`, enforces one leased, contiguous-sequence
  talk session, decodes its G.711 A-law audio to little-endian PCM16, submits
  it through the recovered 20-byte AO ioctl ABI, and serializes those calls
  with OEM writes and ioctls to the DSP descriptor;
- drops talkback while no DSP descriptor is open and stops using a descriptor
  immediately after close or a failed write. Talk ownership expires after half
  a second without a valid packet, and malformed or out-of-sequence traffic
  releases ownership.
- owns the active-high speaker amplifier on GPIO 63: OEM direction/value writes
  are confined to output/muted, the amplifier is enabled only for a valid local
  talkback lease, and release, malformed traffic, timeout, or shutdown restores
  mute. This suppresses the OEM motion-detection siren without disabling
  authenticated push-to-talk.
- discards OEM DSP playback writes and the exact AO payload ioctl while
  reporting successful consumption, so muted alarm audio cannot queue and leak
  into the next authenticated talkback window; guard-injected PCM bypasses the
  interposed OEM path.
- records an `O_RDONLY` open of `/dev/dsp`, taps successful `read`/`readv` and
  the exact OEM `AMIC_AI_GET_STREAM` ioctl PCM16 result without blocking the
  OEM reader, converts it to 16 kHz mono A-law, and
  sends fixed 20 ms `JAGM` datagrams to `/run/jooan-local/mic.sock`; missing or
  backpressured listeners cause packet drops, never an OEM read failure.

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
