# Compatibility

## Supported hardware

The target is restricted to one observed bill of materials. This
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

The target manifest enforces the `JA-A12`/`A12` model identity, and the
installer refuses a camera whose pinned OEM files differ, before it writes
anything. Software checks cannot detect every component substitution, though:
only opening the camera shows the PCB marking and the fitted parts. That is
the complete check, and it is recommended before installation.

The platform installer also pins the exact retained components it depends
on: the OEM `jooanipc` executable, `startapp` and GoAhead, the sensor and radio
kernel modules, and the userspace libraries it links. It does not pin the
application partition as a whole, so an appfs rebuilt to carry the media
daemon (with those files unchanged) still accepts platform updates.

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

The retrofit intentionally preserves the compatible unit's bootloader, OEM
kernel, `jooanipc`, vendor media modules, sensor tuning, radio support and
factory data. Neither layer is a whole-flash image:

- the platform package writes only `/opt`;
- the media layer's installer rewrites the application partition
  (`/dev/mtd4`) with an image rebuilt from this camera's own appfs. The OEM
  files above stay byte-identical (the containment guard and the platform's
  pins check them); the image adds one jooanipc-open release, and the
  qualified image drops files this board does not use, such as voice prompts
  and other sensors' drivers.

The OEM's stored Wi-Fi configuration is left untouched on flash. A native
boot does not use it: it joins only the network committed through the
platform. The OEM application keeps using its own configuration in the
fallback modes.

Preservation is not portability. A backup from one camera must never be
restored onto another camera because it can contain unique calibration,
identity, network, and credential data.

## Wi-Fi access-point interoperability

No access point is known to be incompatible with the verified
SKW6316/SV6160Lite firmware. In one session on 2026-09-19 the camera did not
associate with an OpenWrt access point with an IPQ5018 (ath11k) radio on
2.4 GHz channels 11 and 6. With the same driver and firmware, it later
associated with that access point on both bands: 5 GHz channel 149 on
2026-10-16 and 2.4 GHz channel 11 on 2026-10-18. The cause of the first
failure was not identified.

Keep Ethernet recovery connected when changing Wi-Fi, and let the
transactional timeout roll back a failed association. Do not substitute public
SDIO SWT6621S firmware for this unit's USB firmware/driver pair.

## Evidence to record

Before installation, record photographs of the PCB and flash marking, the
current firmware/kernel versions and the camera's model as the vendor app
reports it. After the platform is installed, record the MTD map
(`cat /proc/mtd`) and hashes of two identical full-flash reads, taken over SSH
([Installation, step 7](INSTALL.md#7-back-up-the-camera-over-ssh)) or with a
programmer. Store this evidence offline; do not open a public issue with an
unsanitized dump.
