# Hardware round-trip proof — stock to open firmware (2026-11-08)

This records the first end-to-end run of the whole public install path on a real
JOOAN W3-U (JA-A12), and the findings it produced. Private test-rig details
(addresses, network names, the cloud-redirect host) are omitted; what matters
here is what the camera did and what it proves.

## What was proven, in order

1. **Revert to genuine stock.** The test camera, which had been on the open
   media build, was flashed back to the stock application partition with a
   watchdog-fed flash and a RAM rollback, then factory-reset. It booted genuine,
   unprovisioned stock and raised its own Wi-Fi SoftAP (SSID prefix `JAA-`,
   open), reachable at `10.68.68.22`.

2. **App-free provisioning.** A nearby PC joined the camera's SoftAP and sent one
   request:

       GET http://10.68.68.22/goform/setupWifiInfo?ssid_hex=...&key_hex=...&token=...&...

   with the parameters recovered from the vendor app (`ssid_hex`, `key_hex`,
   `token = SHA256(hex(bssid)+hex(rand3)+hex(pwd))[:32]`, `userid=admin`,
   `userkey=md5("admin123")`). The camera answered `{"result":"success",...}`,
   left its SoftAP, joined the target Wi-Fi on its own, and began contacting its
   cloud — a genuine stock camera, provisioned with no app. This is the channel
   `provision.py` implements (GoAhead HTTP, not the Alibaba CoAP variant; this
   camera does not answer CoAP 5683).

3. **Over-the-air platform install.** With `use1api.jooaniot.com` redirected to a
   local server (DNS override plus a port-53 intercept, because the camera
   ignores DHCP DNS) and the camera's cloud answered by the project's MITM-cloud
   helper, the helper pushed the firmware-update command over MQTT
   (`cmd 66400`, pointing at `/fw/JOOAN_FW_PKG`). The camera replied
   `cmd=66400 status=0`, downloaded the **signed platform package built from this
   repository with the Phase A and B fixes**, applied it, and rebooted into the
   platform (`release_version 0.2.39`), announcing itself over mDNS as
   `jooan-w3u`.

4. **Both fixes live.** On the running platform, `jl_finalize_sequence` (Phase A)
   and `jl_drop_dead_eth0` (Phase B) are present in the active controller, and
   Phase B is visibly working: `eth0` is up with **no IPv4 address** (the stale
   `172.16.1.18` that stock sets was flushed) and the route table holds a single
   clean `wlan0` subnet route — the collision that stranded the camera before is
   gone, with no hand-patch.

5. **Media layer built and installed from the camera's own partition.** The
   platform's Wi-Fi was committed through its API (WPA2), the camera's appfs was
   dumped, `build-appfs.sh --first` repacked it with the open `jooanipc-open`
   binaries (no compiler), and `install-camera.py --first` flashed it with a RAM
   rollback. The open daemon came up serving **RTSP on :18554**
   (`Server: jooanipc-open/0.1.0`) and **HTTPS/ONVIF on :18443**, took ownership
   of the hardware watchdog (native-boot qualification), and **survived a full
   cold power cycle** still on the open image.

## Findings fixed or recorded

1. **Fixed (code).** `install-camera.py` rejected a fresh OTA platform because it
   reports `release_sequence 0` until a sequence is published. Added
   `platform_can_boot_media()`: accept when sequence ≥ 73 **or**
   `release_version ≥ 0.2.39`. A genuinely old platform at sequence 0 is still
   refused. Covered by `tests/test_install_camera_gate.py`.

2. **Fixed (platform runtime).** `wifi-apply.sh` (and the ip-stage re-apply in
   `integration-helper.sh`) required an 8–63 character password and rejected open
   networks. They now treat an empty password as an open network and join it with
   `key_mgmt NONE`. Covered by `tests/test_runtime_hooks.py`
   (`test_wifi_hook_joins_an_open_network_with_key_mgmt_none`).

3. **Fixed (space).** A fresh platform plus the full stock `/opt/conf` can leave
   the 384 KiB `/opt` below the ~96 KiB the first media install needs.
   `install-camera.py` now frees the regenerable `/opt/conf/config.org`
   automatically when `/opt` is short and the stock copy is present in
   `/mnt/mtd/etc` to restore it. Covered by `tests/test_install_camera_gate.py`
   (`OptSpaceReclaim`).

## Bottom line

The complete public chain — stock → app-free provisioning → cloud-triggered OTA
→ platform (with the A/B fixes) → open media built from the user's own camera →
serving video, surviving reboot — works on real hardware. The remaining work is
the GUI wrapper and the two small platform rough edges above.
