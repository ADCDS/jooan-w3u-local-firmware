# Compatibility

## Supported hardware

The version 1 target is restricted to one observed bill of materials. This
scope statement is not a release-support claim; no tag is supported until the
physical promotion gates pass.

| Component | Required value |
|---|---|
| OEM product/model | JOOAN W3-U; `/etc/deviceModel` is `JA-A12` |
| Package model token | `A12` |
| PCB | `JA-6621 V1.0` |
| SoC | Ingenic T23N |
| DRAM | 64 MiB physical on the verified unit |
| SPI NOR | 8 MiB; verified part Puya P25Q64HA |
| Primary sensor | `cv2005`, I2C-0 address `0x35` |
| Secondary sensor | `cv2005s1`, I2C-0 address `0x36` |
| Radio | USB SeekWave SKW6316 / `SV6160LITE` |
| OEM kernel family | Linux `3.10.14__isvp_pike_1.0__` |

The target manifest enforces the `JA-A12`/`A12` model identity, but software
model checks cannot detect every component substitution. Open the camera and
verify the PCB and fitted parts before installation.

## Explicitly unsupported

- the Thingino Jooan W3-U target with one SC2336P sensor and an ATBM6132U
  radio;
- Jooan W3-U units whose model name matches but PCB, sensors, flash size, or
  radio differs;
- Lenovo/NVT or other rebrands unless every identifier above is independently
  verified;
- Fullhan, SigmaStar, or other non-Ingenic revisions;
- any unit whose complete flash cannot be backed up and restored.

Do not “try the image” to discover compatibility. A mismatched sensor driver,
radio driver, GPIO assignment, or flash layout can remove video, networking, or
the ability to boot.

## What is preserved

The retrofit intentionally preserves the compatible unit's OEM kernel,
`jooanipc`, vendor media modules, sensor tuning, radio support, factory data,
and existing Wi-Fi configuration. A generic release is not a whole-flash image
and must not replace those per-device areas.

Preservation is not portability. A backup from one camera must never be
restored onto another camera because it can contain unique calibration,
identity, network, and credential data.

## Wi-Fi access-point interoperability

The verified SKW6316/SV6160Lite firmware is not interoperable with every access
point. It completed WPA association against Realtek-based bench APs on channels
1 and 6, but repeatedly failed against a tested OpenWrt/ath11k AP while still
seeing its BSS at a usable signal level. Tests ruled out channel 11, HE versus
HT, legacy-rate policy, WPA2 versus mixed mode, the UTF-8 SSID capability,
secondary-BSSID addressing, nl80211 versus wext, and interference from the OEM
network manager. The camera submitted authentication to its driver, while the
ath11k hostapd instance received no corresponding station event.

This is an AP/firmware compatibility limitation, not a credential error. Keep
Ethernet recovery connected when changing Wi-Fi, let the transactional timeout
roll back a failed association, and qualify the intended AP before relying on
Wi-Fi-only administration. Do not substitute public SDIO SWT6621S firmware for
this unit's USB firmware/driver pair.

## Evidence to record

Before installation, record photographs of the PCB and flash marking, the
output of the supplied compatibility check, current firmware/kernel versions,
the MTD map, and hashes of two identical full-flash reads. Store this evidence
offline; do not open a public issue with an unsanitized dump.
