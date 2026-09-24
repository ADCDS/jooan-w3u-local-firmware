# Off-camera RTSP → browser playback investigation

The camera has a 38 MiB Linux RAM budget and a single CPU core. **Do not
attempt to run a WebRTC server, transcoder, or another always-on media service on
it.** The retained OEM `jooanipc` owns both encoders; `joan-daemon` is a local
security/control layer and browser fMP4 remuxer, not a replacement encoder.

## Observed on camera1, 2026-09-24

| State (13.8 s samples, single core) | OEM `jooanipc` | `joan-daemon` | fMP4 ingest threads (main + sub) | CPU idle |
|---|---:|---:|---:|---:|
| Browser fMP4 viewer absent | 63.4% | 3.9% | 1.6% | 0% |
| Off-host RTSP relay + WebRTC viewer | 57.8% | 7.4% | 2.0% | 0% |

The OEM audio callback alone accounted for ~34–41% of the core. Do not kill or
patch it without independently proving the effect on video, microphone,
talkback, and process stability. 38,324 KiB RAM, roughly 1.5–3 MiB free and
7–8 MiB cached, zero swap. Wi-Fi was connected to `BRAVO-IOT` at about -62 dBm;
RX drops grew by six packets in each ~14 s sample. The daemon fMP4 threads
**were not the primary source of CPU saturation**. Browser fMP4 delivery can
still add camera TLS/network/worker load, and Chrome may struggle to decode two
simultaneous 2304×1296 and 640×360 videos on a particular client.

## Browser options

Ordinary `<video>` elements and browser JavaScript cannot open `rtsp://` as
HTML video. RTSP must first be presented as a browser protocol by a trusted
**separate LAN host**: e.g. MediaMTX (RTSP → WebRTC/WHEP) or go2rtc. WHEP needs
an RTCPeerConnection client, not `<video src=".../whep">`. A relay can copy
H.264 packets without decoding/re-encoding them, provided browser H.264
negotiation and source timestamps are compatible.

The camera actually emits H.264 High profile, main Level 5.0, sub Level 2.2.
Browser WebRTC only guarantees Constrained Baseline; test all target browsers,
particularly mobile. A source codec/profile change requires encoder settings
or off-camera transcoding, not merely a relay.

## Tested local-only feasibility (no public or persistent service)

MediaMTX v1.21.1 on a LAN desktop successfully pulled the authenticated camera
RTSP streams, but direct RTSP→WebRTC repeatedly closed with `WebRTC doesn't
support H264 streams with B-frames`. Independent `ffprobe` samples decoded only
I/P frames; occasional OEM RTP presentation timestamp regressions, not proven
B-frames, caused the relay's `u.PTS < lastPTS` test to trip. **Do not deploy a
naive direct proxy** without this compatibility check.

An **off-camera** FFmpeg `-c:v copy` pipeline with
`-use_wallclock_as_timestamps 1` and `-fps_mode passthrough` republished each
RTSP video into a loopback MediaMTX RTSP input. Both sub (640×360) and main
(2304×1296) then played through MediaMTX's WebRTC reader in headless Chromium
with no video transcoding: main advanced 8 s/8 s (120 decoded frames, two
additional dropped) and 6 s/6 s (90 decoded, two dropped). The FFmpeg copy jobs
and relay each used roughly 0.5–1.5% of the desktop CPU, but this **short local
experiment** is not a 24/7 reliability, other-browser, authentication, or
exposure qualification. Initial FFmpeg log noted unset startup timestamps and
a duplicate DTS adjusted by one tick; long-run discontinuity behavior still
needs monitoring. The actual six-second camera keyframe interval also delays a
new WebRTC viewer until an IDR arrives; shortening that in the OEM is not a
qualified operation.

`tools/mediamtx-evaluation.yml` is an intentionally loopback-only, placeholder
configuration for reproducing this test. Never commit a rendered config with a
camera credential. Source passwords belong in a protected secret manager or
0600 service configuration on the chosen offload host; a public Web page must
not include them.

## Production deployment gate

1. Choose a **specific always-on LAN host** reachable from the camera and all
   browser clients; the developer desktop used in this test is not proof of
   always-on availability. Run a pinned MediaMTX + one FFmpeg *video-copy
   restamper per sensor* under restartable supervised services. Keep source
   credentials server-side. Constrain FFmpeg inputs, output loopback only,
   service privileges, logs, and resource limits. Measure 24 h camera and relay
   CPU, memory, restarts, RTSP stability, WebRTC frame drops and start delay.
2. Expose signaling through **trusted HTTPS**, same-origin proxy or a dedicated
   browser-trusted LAN name. The camera page's current CSP has no `frame-src`
   for a remote player, and `X-Frame-Options: DENY`; a separate relay page or a
   revised camera UI/CSP is necessary. The WebRTC ICE media path is **UDP**
   (MediaMTX default 8189) in addition to HTTPS signaling (default 8889).
   Advertise only reachable LAN IPs in ICE; firewall both signaling and UDP
   to intended private clients. No public Internet route should be required.
3. Authenticate browser access; CORS is not authentication. If embedding, use
   MediaMTX `reader.js`/WHEP or an authenticated iframe with a suitable policy.
   Prefer video-only initially; audio conversion needs a separate compatibility
   check. Keep the existing camera Web UI as a fallback for control and PTZ.
4. Verify Chromium, Firefox, Safari/iOS, and TV clients with the *actual*
   High-profile bitstream. If a client fails, only then evaluate off-host
   hardware/software re-encoding, or a documented camera Baseline option.

MediaMTX reference: [RTSP sources](https://mediamtx.org/docs/publish/rtsp-cameras-and-servers),
[browser WHEP client](https://mediamtx.org/docs/read/web-browsers),
[WebRTC/ICE and codec caveats](https://mediamtx.org/docs/features/webrtc-specific-features).
Browser H.264 guarantees: [MDN WebRTC codecs](https://developer.mozilla.org/en-US/docs/Web/Media/Guides/Formats/WebRTC_codecs).
