# Recovery

## Recovery tiers

Two independent paths are required:

- **Package rollback/uninstall** for a booting camera whose local management
  or OEM update transport still works.
- **External SPI recovery** for a camera that no longer boots or is unreachable.

Do not make persistent changes until the external path has been rehearsed. An
uninstall package stored only on the camera is not an independent recovery
method.

## Recovery assets

Keep these offline and label them for one physical camera:

- two byte-identical full 8 MiB flash reads and their SHA-256 hashes;
- photographs of PCB `JA-6621 V1.0`, flash orientation, clip orientation, and
  wiring;
- programmer model, voltage setting, flash-part selection, and a known-good
  read command;
- the exact install and uninstall packages, manifests, and hashes;
- a copy of the release source and toolchain metadata;
- sanitized notes describing the last successful boot and network state.

Never share one camera's dump as a recovery image for another unit.

## Booting-camera rollback

1. Move the camera and workstation to the isolated recovery network.
2. Block all camera Internet access at the router.
3. Verify the uninstall package hash and `A12` target metadata.
4. Use the authenticated local update path or the documented offline transport.
5. Maintain stable power until the package reports completion and reboot ends.
6. Cold boot, inspect status/listeners, and verify the intended restored state.

Rollback must preserve factory data and the existing Wi-Fi configuration unless
the reviewed uninstall manifest explicitly says otherwise. If the camera cannot
authenticate, cannot validate the package, or repeatedly reboots, stop trying
network updates and use external recovery.

## External SPI recovery

The verified board uses an 8 MiB SPI NOR (Puya P25Q64HA on the observed unit).
Confirm the fitted chip rather than assuming it.

1. Disconnect camera power and Ethernet.
2. Use a 3.3 V-safe programmer and verify pin 1/orientation from the board and
   flash datasheet.
3. Read the chip again before writing and preserve that failed-state image.
4. Confirm the backup length is exactly 8 MiB and its SHA-256 matches one of the
   pre-install reads.
5. Write the complete image using the programmer's verified command.
6. Read the chip back and compare every byte with the backup.
7. Disconnect the programmer before applying normal camera power.
8. Boot only on the isolated recovery network and verify OEM kernel, both
   sensors, radio, and network behavior.

Programmer-specific commands are intentionally not universalized here: clip
wiring, controller voltage, and flash support differ. Follow the programmer and
flash-vendor documentation. Never use 5 V on a 3.3 V SPI NOR.

## After recovery

Assume any credential or private key present during a security failure may be
compromised. Rotate the administrator password, regenerate the per-device HTTPS
identity where appropriate, replace enrolled SSH keys, and replace Wi-Fi
credentials. Revalidate router isolation before returning the camera to normal
use.

Record why recovery was needed and retain sanitized logs. Do not upload raw
flash images, keys, tokens, cookies, or Wi-Fi configuration to an issue.

## Recovery is complete only when

- the device cold-boots repeatedly;
- both CV2005 paths produce expected video;
- the SKW6316 interface is stable;
- the intended management path works;
- unexpected ports and outbound traffic are absent or blocked;
- the new full-flash read is archived with its hash.
