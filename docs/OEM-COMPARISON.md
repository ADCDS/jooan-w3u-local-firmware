# Compared with the official firmware

This page is for someone deciding whether this firmware fits them. It lists
what you give up compared with the camera's **official firmware**, what works
differently, and what this firmware adds.

"Official firmware" here means everything the JOOAN W3-U (`JA-A12`) ships
with: the stock firmware on the camera, whose application `jooanipc` does all
the camera work, together with JOOAN's **Cam720** phone app (the only app the
vendor supports for its cameras[^faq-app]) and the JOOAN cloud behind it.

This firmware has two layers:

- the **platform** (the repository root): the maintenance Web UI and API on
  port 80, SSH, Wi-Fi, clock, signed updates and the network lockdown;
- the **media** daemon `jooanipc-open` ([`media/`](../media/)): video over
  RTSP, ONVIF, pan/tilt, presets, lights and optional local recording.

In the default **native boot** the stock application `jooanipc` never starts.
It stays on flash only as a fallback (see [boot modes](ARCHITECTURE.md#boot-modes)),
and even there it runs contained: no cloud, listeners on loopback only, speaker
muted. So every official feature that lives in `jooanipc` or in the cloud is
gone unless this project reimplemented it. The tables below say which ones it
did. The hardware itself (lenses, field of view, housing, power) is unchanged.

No release of this firmware is supported or hardware-qualified yet; see
[Status and limitations](../README.md#status-and-limitations).

> **Check the hardware before you buy.** JOOAN's own spec table for the W3-U
> lists an SC2336P image sensor[^off]. This firmware supports only the
> revision with two CV2005/CV2005S1 sensors and a SKW6316 radio; the
> SC2336P revision is a different product (see the [README](../README.md#exact-hardware-scope)
> and [Compatibility](COMPATIBILITY.md)). A camera sold as W3-U may not be
> the one this firmware runs on.

## Biggest gaps today

If you are used to the Cam720 app, these are the things you will most likely
miss:

1. **No phone app, no cloud, no viewing from outside your home** (unless you
   bring your own VPN). The camera is meant to be watched and recorded by an
   NVR such as [Frigate](https://frigate.video/) on your own network.
2. **No detection or alerts on the camera.** No motion detection, no push
   notifications, no siren or flashing light, no auto-tracking. Your NVR has to
   do detection and notifications.
3. **No automatic night vision.** Infrared and the white light switch only
   when something calls the API, and the setting is not remembered.
4. **Audio is new and off by default.** The microphone and two-way talk
   (from Frigate's live view) work with `audio=1` and are being qualified on
   the test camera. There is no siren yet.
5. **Lower resolution.** The pan/tilt lens streams 1920×1080 (official:
   2304×1296); the fixed lens streams only 640×360. No JPEG snapshots.
6. **No microSD recording or playback on the camera.** Recording happens on
   the NVR.
7. **Wi-Fi only, and a technical install.** The Ethernet port is not used;
   installing takes a computer, an isolated network and a full flash backup,
   not a QR code.

## How to read the tables

| Status | Meaning |
| --- | --- |
| **Supported** | Works, roughly as in the official firmware or better. |
| **Partial** | Some of it works; the note says what is missing. |
| **Missing** | Not available today. Could be added. |
| **Not planned** | Left out on purpose, usually because it needs the vendor cloud. |

"(media)" or "(platform)" names the layer that provides a feature. Footnotes
are the vendor's public pages and store listings. Three more sources come from
the verified unit and the vendor's app, tagged in the tables and described
under [Sources](#sources):

- **[cfg]** the vendor configuration on the verified unit, including its
  `FunctionList` capability flags (a flag at `1` means the stock firmware
  turns the feature on for this model);
- **[app]** text inside the stock application `jooanipc`;
- **[phone]** text inside the vendor's Android app. One app serves many JOOAN
  models, so this proves the app has a feature, not that the W3-U offers it.

"Unverified" means no source settled the question.

## Video and image

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Live view of both lenses | Both lenses side by side in the app ("dual screen")[^off][^amz-us] | **Supported** (media) | Over RTSP, for an NVR or a player such as VLC: `/live/ch0` is the pan/tilt lens, `/live/ch2` the fixed lens. Neither the platform Web UI nor the media daemon's small web page shows video. |
| Pan/tilt lens resolution | 3MP, 2304×1296, with a 640×360 substream[^off]. On the verified unit: 15 fps, 1024 kbit/s [cfg] | **Partial** (media) | 1920×1080 at 15 fps, 3072 kbit/s. The 2304×1296 output does not fit the reserved video memory with the current encoder setup. The project's [1080p test](../media/docs/camera-1080-test-20261018.json) notes that both sensor drivers report a 1080p input and that the stock 2304×1296 mode looks upscaled from it; the sensor's real maximum is not established. |
| Fixed lens resolution | Marketed as two 3MP lenses[^amz-us]; the verified unit's settings give it the same 2304×1296 plus 640×360 [cfg] | **Partial** (media) | 640×360 only. The shipped profile (`main_only=1`) refuses anything larger. |
| Separate low-resolution substreams | One 640×360 substream per lens[^off] [cfg] | **Partial** (media) | In the shipped profile `ch1` and `ch3` carry the same feed as `ch0` and `ch2`. A four-encoder mode has real substreams, but limits both main streams to 1280×720. |
| Video codec | H.264[^off] | **Supported** (media) | H.264. |
| Frame rate | 15 fps on the verified unit [cfg]; not stated publicly | **Supported** (media) | 15 fps. With the shipped video library the daemon refuses any other rate. |
| Digital zoom | 5× in the app[^off] | **Missing** | Zoom in the NVR or player. ONVIF zoom requests are rejected. |
| Still snapshots (JPEG) | App screenshots (`support_screenshot_3_million_pixels` = 1) [cfg] | **Missing** (media) | `GET /api/v1/snapshot` and ONVIF `GetSnapshotUri` are refused in the shipped profile. NVRs take stills from the RTSP video instead. Snapshots work only in the four-encoder mode. |
| Flip and mirror | Yes (`image_rollovers` = 1) [cfg] | **Supported** (media) | `flip`, `hflip` and `vflip` controls. Saved, and pan/tilt directions follow the image. |
| Brightness, contrast, saturation, sharpness | Stored settings `Brightness`, `Contrast`, `Hue`, `Saturation`, `Sharpness`, `Denoise` [cfg]; whether the app shows them is unverified | **Partial** (media), unverified | The REST API has `brightness`, `contrast`, `saturation` and `sharpness` controls (0–255). They are undocumented, not saved across restarts and not tested on the camera. No hue or noise-reduction control. |
| Anti-flicker (50/60 Hz) | Stored setting `PowerFreq` [cfg] | **Missing** | No control. |
| Dual-lens linkage (tap the wide view to aim the pan/tilt lens) | Not advertised; a linkage setting exists (`GunBallLinkageEnable`) [cfg] and the app has lens-linkage screens [phone]. Offered on this model: unverified | **Missing** | |
| Timestamp and logo on the picture | Timestamp on by default (`OSDTimeEnable` = 1), optional logo (`OSDLogoEnable`) [cfg] | **Missing** | The media daemon draws no overlay. NVR recordings carry the NVR's own timestamps. |

## Night vision and lights

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Automatic day/night switching | Yes: the lights come on by themselves in the dark[^amz-us]; light thresholds `ToNightEv`/`ToDayEv` [cfg] | **Missing** | Day/night is manual only. The automatic mode (`night=2`) is rejected until the sensor's exposure reading is understood. |
| Infrared night vision | Black-and-white IR night vision, up to 20 m[^off][^faq-night] | **Partial** (media) | `night=1` moves the IR-cut filter, turns on the infrared LEDs and puts the image processor in night mode; `night=0` goes back to day. Only through the REST API. The choice is not saved: when the media daemon stops it switches the lights off, and nothing switches them back on at start, so an NVR or home-automation script has to do it. |
| Colour night vision (white light) | Colour night vision with the white light, on a schedule ("timed full colour") or only when a person or animal moves ("intelligent full colour")[^off][^faq-night] | **Partial** (media) | `white_light` on or off by hand. No schedule and no motion-triggered mode. |
| Flashing light on alarm | A strobe light when motion is detected[^amz-us]; alarm-light settings (`AlarmLightParam`) [cfg] | **Missing** | There is no detection to trigger it. |
| Status LED | No LED switch on this model (`indicator_light_switch` = 0) [cfg] | **Supported** (media) | `pilot_led` on or off through the REST API. |

## Pan and tilt

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Manual pan and tilt | From the app; 330° pan and 90° tilt[^off] (the Amazon US listing says 355°[^amz-us]) | **Supported** (media) | ONVIF `ContinuousMove`/`Stop` (by default each command is one steady 350 ms nudge, which suits Frigate's arrows), the REST `ptz` control, and buttons on the media daemon's web page. Travel is the motor driver's calibrated range; it is not measured in degrees here. |
| Presets | Yes (`preset_positions` = 1) [cfg] | **Supported** (media) | 16 named presets over ONVIF and REST, saved on the camera. An explicit migration step can copy the stock presets. |
| Move to an exact position | The stock application's ONVIF server has absolute and relative moves [app]; not advertised | **Missing** | No ONVIF `AbsoluteMove` or `RelativeMove`. The position is reported (`GetStatus`) but can only be set through presets. |
| Calibration | Calibrated at boot; recalibration from the app (`support_ptz_calibrate` = 1) [cfg] | **Partial** (media) | Homes once per boot (`motor_home=1`). No recalibration on demand. |
| Patrol (cruise) | Cruise switch, interval and schedule settings [cfg]. The flag `cruise_setting` is `1` in the factory settings but `0` on the verified unit | **Missing** | |
| Auto-tracking | Yes[^off] (`automatic_tracking` = 1) [cfg] | **Missing** | |
| Privacy mode (lens turns away, on a schedule) | "The camera will enter privacy mode and the device will not be able to watch live, record or upload to cloud storage" [phone]; schedule `PtzHideSchedTime` [cfg] | **Missing** | Moving to a preset turns only the pan/tilt lens; the fixed lens keeps streaming. |

## Audio

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Listen through the microphone | Built-in microphone[^off] | **Partial** (media) | With `audio=1` both main RTSP URLs carry the microphone as 8 kHz PCMA, which Frigate can record as AAC. Off by default; being qualified on the test camera. |
| Two-way talk | Yes[^off] | **Partial** (media) | ONVIF RTSP backchannel, which go2rtc and Frigate's live-view microphone use (the page must be opened over HTTPS), and `POST /api/v1/talk`. One talker at a time, half-duplex. Being qualified on the test camera. |
| Siren and alarm sounds | A loud alarm on motion[^amz-us]; custom alarm sounds[^off]; alarm schedule (`SoundAlarmSchedule`) and self-recorded sounds (`the_user_records_alarm_information` = 1) [cfg] | **Missing** | The platform mutes the speaker amplifier during boot; afterwards the media daemon switches it on only for a talk session. |
| Volume | Microphone and speaker volume (`support_volume_setting`, `support_speaker_volume_setting`) [cfg] | **Partial** (media) | The `volume` control sets either one with `audio=1`; it is not remembered after a restart. |
| Voice prompts | Spoken setup and status prompts [app] | **Missing** | The media installer's rebuilt app partition leaves the prompt files out, so they are gone in the fallback modes too. |
| Sound detection | Yes[^off]; crying detection is claimed only by the Amazon UK listing[^amz-uk] | **Missing** | Frigate can detect sounds on the microphone track once audio is on. |

## Detection and alerts

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Motion detection | Yes[^off], with area, sensitivity and time settings[^amz-uk]; on by default (`MoveDetectEnable` = 1) [cfg] | **Missing** | Left to the NVR: Frigate, for example, detects motion and objects in the RTSP streams. |
| Person detection | **Conflicting:** "Human Detection: Not Supported" in JOOAN's spec table[^off], but "AI Human Detection" in the Amazon US listing[^amz-us]; the stock settings have a `PersonDetect` switch [cfg] | **Missing** | Left to the NVR. |
| Push notifications | App push notifications[^off], with a schedule (`MessagePushSchedTime`) [cfg] | **Not planned** | Needs the cloud. Use the NVR's notifications. |
| Event list | Alarm messages in the app [phone] | **Missing** | Left to the NVR. |
| Detection events sent to an NVR (ONVIF events) | The stock application's ONVIF server contains motion events [app]; whether they reach an NVR on this model is unverified | **Missing** | No ONVIF event service, so the NVR always runs its own detection. |

## Recording and storage

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| microSD recording | 8–256 GB card; record all day, only on detection, or on a schedule[^off][^faq-rec][^amz-uk] | **Missing** | A native boot does not mount the card, and nothing formats it, shows its capacity or manages overwriting. |
| Playback and clip download | Remote playback and download in the app[^ios][^pp] | **Missing** | Use the NVR's timeline. |
| Cloud storage | Paid, event-based, with a free trial[^faq-paid][^faq-rec][^amz-uk] | **Not planned** | Needs the cloud. |
| NAS storage | Not advertised; NAS settings (`SambaParam`) [cfg] and "NAS network storage" in the app, which needs an SD card [phone]. Offered on this model: unverified | **Missing** | |
| Recording on the camera itself | See microSD | **Partial** (media) | The media daemon can write rotating raw H.264 files (5-minute segments, with a size cap) and serve them for download over HTTPS. It is off by default, and in the default layout the folder is in RAM (`/run`), so files are lost at reboot and limited by the camera's 38 MB of memory. |
| Recording by an NVR | ONVIF supported[^off]; the app sets a "security password" for RTSP and suggests changing it "if connected to an NVR" [phone] | **Supported** (media) | The intended use: Frigate or another RTSP NVR records both lenses. |

## Apps and remote access

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Phone app | Cam720 for Android and iOS[^off][^faq-app] | **Not planned** | No phone app. Watch and steer through an NVR (Frigate, also inside Home Assistant), a player such as VLC, or any ONVIF client. |
| Computer | Cam720 PC client for Windows[^faq-app][^dl] | **Partial** | Any RTSP player or ONVIF client on your network. The browser pages are for maintenance only (see below). |
| Viewing away from home | Through the cloud with P2P[^ios] | **Not planned** | The camera has no Internet route. Use your own VPN into the camera's network; a VPN client on another private subnet needs its prefix added to the camera's route allowlist (`/api/v1/network/routes`). |
| Sharing with family | Device sharing by QR code[^pp] | **Not planned** | Use the NVR's user accounts. |
| Voice assistants | Alexa[^off]; Tmall Genie[^ios]; the app also links to Google Home [phone] | **Missing** | |
| Web browser | Unverified | **Partial** | The platform's maintenance Web UI on port 80 (no video) and the media daemon's minimal page on port 18443 (pan/tilt buttons, recording start/stop and download). |
| RTSP | Not on the product page; the app sets an RTSP "security password" for NVRs [phone] | **Supported** (media) | TCP/18554, Digest authentication, RTP over TCP or UDP, H.264, up to 8 clients in the default configuration. |
| ONVIF | Supported[^off]. The stock application's ONVIF server also has events, imaging, absolute and relative moves, snapshots and recording search [app] | **Partial** (media) | An ONVIF subset on HTTPS port 18443: device information, profiles, stream addresses, pan/tilt and presets. No events, imaging settings, snapshots (in the shipped profile), recording search or encoder changes. WS-Discovery is implemented but off in the default configuration, so add the camera by address. |

## Network and setup

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| First setup | In the app: Bluetooth pairing[^off], the camera's own Wi-Fi hotspot or a QR code [phone], or a network cable[^off] | **Missing** (different process) | Installing is technical: an isolated network, a full flash backup, the first package uploaded from a computer to the stock updater, then the media installer, which rewrites the app partition with an image built from your own camera. See [Installation](INSTALL.md). The install guide assumes the stock camera is already on your network. |
| Wi-Fi bands | 2.4 GHz and 5 GHz[^off] | **Supported** (platform) | 2.4 GHz and 5 GHz. The camera joins the network's 5 GHz access point when it is strong enough and falls back to 2.4 GHz. The radio did not authenticate to one tested OpenWrt/ath11k access point; test yours first. |
| Changing the Wi-Fi network | In the app [phone] | **Supported** (platform) | Stage, then commit in the Web UI. A network that is not committed within 90 seconds is rolled back and the camera reboots on the old one. |
| List of nearby networks | Yes (`support_get_wifilist` = 1) [cfg] | **Missing** | Type the network name. |
| Signal strength | Unverified | **Supported** (platform) | Signal, link rate and counters at `/api/v1/network/quality` and in the Web UI. |
| Wired Ethernet | Ethernet port[^off] | **Missing** | A native boot configures only Wi-Fi and does not finish without a Wi-Fi gateway. The Ethernet link is only reported. |
| Static IP address | Stored Ethernet IP settings (`Eth0NetworkParam`) [cfg]; whether the app offers them is unverified | **Missing** | DHCP on Wi-Fi only. |
| Clock and time zone | Clock set from the phone (`phone_utctime_sync` = 1), time zone stored (`TimeZomeCfg`) [cfg] | **Partial** (platform) | Set by hand, or by optional network time (SNTP) from a server on your network, off by default. The board has no battery-backed clock, so without network time the clock lags after every power cut. ONVIF always reports UTC. The stored time zone is used only by the stock application's overlay in the fallback modes. |

## Maintenance and updates

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Firmware updates | Not described on the product page. The stored update address (`UpgradeFilePath`) points to a JOOAN server [cfg] | **Partial** | Platform: signed packages uploaded through the Web UI or API; the camera checks the signature and refuses older releases. No automatic check. Media: updated from a computer by the media installer, with a bundle built from your own camera's app partition. |
| Reboot and scheduled reboot | Reboot from the app [phone]; scheduled reboot (`FixedTimeReboot`, `support_timing_reboot` = 1) [cfg] | **Missing** | No reboot button or API, except as part of an update. Possible from the SSH shell. No schedule. |
| Factory reset | Reset button (`ResetGpio`) [cfg]; the app suggests a reset to restore the initial password [phone] | **Missing** | Nothing in this firmware reads the reset button; what it does after install is unverified. To go back to stock: the signed uninstall package for the platform, plus the app partition backup you took before installing the media layer (see [Recovery](RECOVERY.md)). |
| Vendor remote support and log upload | Diagnostic tunnel and log upload settings (`DiagMode`, `LogUploadInfo`) [cfg] | **Not planned** | |

## Security and privacy

| Feature | Official firmware | This firmware | How it's done here, or what's missing |
| --- | --- | --- | --- |
| Cloud account and vendor servers | The app works through the vendor's cloud[^ios]; the privacy policy says user information is stored in the People's Republic of China[^pp] | **Not planned** (removed) | In a native boot the stock application never runs. Default routes are removed and kept removed, so the camera has no Internet route. In the fallback modes the stock application is contained: no cloud or P2P, listeners on loopback only. Router or VLAN isolation is still required. |
| Accounts and passwords | An app account, plus a device "security password" for RTSP [phone] | **Supported** | One local `admin` account for the Web UI and SSH (initial password `admin`), and separate credentials for the media daemon's RTSP, ONVIF and HTTPS. Repeated failed logins from one address are throttled. |
| Encryption | The Google Play data-safety section for Cam720 says "Data isn't encrypted"[^gp] | **Partial** | The media daemon's REST API and ONVIF use HTTPS. RTSP uses Digest authentication but is not encrypted, and the maintenance Web UI is plain HTTP. |
| Privacy mode | See [Pan and tilt](#pan-and-tilt) | **Missing** | |

## What this firmware adds

- **No cloud, no account, no Internet.** The camera works entirely on your
  network and never contacts JOOAN.
- **Standard NVR integration.** Both lenses over RTSP, and ONVIF pan/tilt and
  16 named presets, tested with Frigate.
- **A higher video bitrate** for the pan/tilt lens: 3072 kbit/s against the
  1024 kbit/s of the verified unit's stock settings, at a lower resolution.
- **Safe Wi-Fi changes** that roll back by themselves if the new network does
  not work.
- **Signed updates.** The platform installs only packages signed with the
  project's release key and refuses older releases.
- **SSH** with a root shell, password or Ed25519 keys.
- **A local name** (`jooan-w3u.local` by default) over mDNS.
- **Network time from your own server** instead of a vendor service.
- **Self-recovery.** A hardware watchdog, fallback boot modes and a separate
  SSH recovery path when an update fails.
- **Open source.** The platform is GPL-2.0-only and the media daemon MIT;
  the vendor kernel, drivers and sensor tuning stay as they are.
- **Status and health endpoints** (`/api/v1/status`, `/health`) for
  monitoring.

## How this was checked

Claims about this firmware were checked against the source at the commit that
adds this page, and against the deployed configuration of the verified unit:

| Area | Where |
| --- | --- |
| Video, resolutions, audio restriction, snapshots | `media/src/config.c`, `media/src/imp_backend.c`, `media/config/ja-a12.example.ini` |
| Controls (lights, night mode, image, pan/tilt, presets) | `media/src/media.c`, `media/src/platform.c`, [platform controls](../media/docs/platform.md) |
| ONVIF operations | `media/src/onvif.c` |
| REST routes and the media web page | `media/src/http.c` |
| RTSP | `media/src/rtsp.c` |
| Recording | `media/src/record.c` |
| Platform API routes | `src/daemon/http.c`, [API](API.md) |
| Boot, Wi-Fi and Ethernet | `runtime/boot/local.rc`, `runtime/slot/hooks/wifi-apply.sh`, `media/scripts/native-boot.sh` |

## Sources

Web pages were read on 2026-11-05. Listings change, and they do not always
agree with each other or with the vendor's spec table; where they disagree,
the tables say so.

- **[cfg]** Vendor configuration on the verified unit: the factory template
  (`/etc/config.org` in the app partition) and the settings the stock firmware
  and app wrote (`/opt/conf/config.json`), stock firmware version
  `05.02.31.114`. Only key names and non-identifying values are quoted.
- **[app]** Text inside the stock application `jooanipc` of that firmware.
- **[phone]** Text inside the vendor's Android app, which calls itself Cam720.

[^off]: JOOAN, W3-U product page and its Specifications tab: <https://www.jooancam.com/products/1949bbeb-b9ee-491b-83cf-3e38b3921f54>
[^faq-app]: JOOAN FAQ, "Which app is used to control the camera?": <https://www.jooancam.com/support/faqs/64eba12d-3a38-4b31-8999-6140620110a2>
[^faq-rec]: JOOAN FAQ on recording modes and SD cards: <https://www.jooancam.com/support/faqs/23594f2a-b163-4856-9d74-15434ed3fd3f>
[^faq-night]: JOOAN FAQ, "Does the camera have a night vision mode": <https://www.jooancam.com/support/faqs/7da9fea8-9489-455c-9e82-bb91a28543f9>
[^faq-paid]: JOOAN FAQ on paid features (cloud storage, AI-assisted monitoring): <https://www.jooancam.com/support/faqs/f651ede6-fc63-4f3f-b4c6-5e0427c2196d>
[^dl]: JOOAN download center (Cam720 PC client): <https://www.jooancam.com/support/download-center>
[^amz-us]: Amazon US listing, W3-U01A: <https://www.amazon.com/dp/B0H6PLWWL2>
[^amz-uk]: Amazon UK listing, W3-U: <https://www.amazon.co.uk/dp/B0F9F4BPZ3>
[^ios]: Cam720 on the App Store: <https://apps.apple.com/us/app/cam720/id6444693400>
[^pp]: Cam720 privacy policy: <https://use1policy.jooaniot.com/cam720_privacy_policy_en.html>
[^gp]: Cam720 data safety on Google Play: <https://play.google.com/store/apps/datasafety?id=com.jooan.qiaoanzhilian.fmr.gp>
