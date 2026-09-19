# Compatibility

## Supported hardware

Version 1 supports exactly one observed bill of materials:

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

## Evidence to record

Before installation, record photographs of the PCB and flash marking, the
output of the supplied compatibility check, current firmware/kernel versions,
the MTD map, and hashes of two identical full-flash reads. Store this evidence
offline; do not open a public issue with an unsanitized dump.
