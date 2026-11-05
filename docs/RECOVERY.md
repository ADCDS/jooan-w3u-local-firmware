# Recovery

## Recovery tiers

Two independent paths are required:

- **Package rollback/uninstall** for a booting camera whose local management
  or OEM update transport still works.
- **External SPI recovery** for a camera that no longer boots or is unreachable.

The media layer's appfs reflash adds a third, manual path for the one case
where it keeps the camera running on a damaged partition; see
[Appfs reflash recovery](#appfs-reflash-recovery).

An uninstall package stored only on the camera is not an independent recovery
method. Keep it off-device. External SPI restore remains the strongest path for
a camera that cannot boot or accept an update.

## Recovery assets

Keep these offline and label them for one physical camera:

- two byte-identical full 8 MiB flash reads and their SHA-256 hashes;
- photographs of PCB `JA-6621 V1.0`, flash orientation, clip orientation, and
  wiring;
- programmer model, voltage setting, flash-part selection, and a known-good
  read command;
- the exact install and uninstall packages, manifests, and hashes;
- a read of the appfs partition (`/dev/mtd4`) taken before each media-layer
  update; the bundle's `backups/` directory holds only configuration files;
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

Uninstall preserves factory data and the OEM's stored Wi-Fi configuration,
but deletes the platform's state, including the committed Wi-Fi network
(`wifi.json`); afterwards the OEM application uses its own stored network,
which may not be the one the camera was using. If the camera cannot
authenticate, cannot validate the package, or repeatedly reboots, stop trying
network updates and use external recovery.

Runtime replacement is a maintenance transaction: the signed controller and SSH
recovery bundle remain available while the sole persistent runtime is replaced.
During the two-minute health trial the supervisor restarts a candidate whose
start failed: `start.sh` is bounded to 15 s while `entropy-ready.sh` waits up to
30 s, and an early native boot gathers entropy slowly. If the candidate still
fails, or the trial is interrupted, it stays selected as the only runtime,
because the update already deleted the previous one, and the supervisor keeps
restarting it; recover through controller-owned SSH and retry or uninstall.
(Until this was fixed, a failed trial removed the candidate and left no
runtime; the paragraph below is that state.) The old runtime may continue from tmpfs until the update reboot, but it is not an
on-flash rollback slot. A failed platform update does not fall back to the OEM
updater: GoAhead stays killed while the controller runs.

Without a runtime, nothing applies the committed Wi-Fi network. A native boot
then cannot reach its gateway, fails after 300 s and latches `native-failed`,
and later boots run the guarded OEM application, which joins its own stored
network. On the verified unit that is the vendor bench network `JOOANTEST`
(open, probed as a hidden SSID). To reach the camera in that state:

1. Publish a temporary hidden, open `JOOANTEST` network on an access point the
   camera can hear, restricted to the camera's MAC and on an isolated subnet.
   The camera joins within seconds. It may not take an IPv4 address, but its
   IPv6 link-local address answers SSH (dropbear listens on `[::]:22`), for
   example through `ssh -L 2222:[fe80::…%<bridge>]:22` on that access point.
2. Install the runtime as the stable slot: copy `runtime.tar.gz` and
   `runtime.md5` from the release stage to `/opt/custom/jooan-local/slots/A/`,
   then, with `common.sh` sourced, `jl_lock; jl_write_selection A - 0; jl_unlock`.
3. Delete `/opt/jooanipc-open/native-failed`, `sync` and reboot. The supervisor
   starts the runtime (and restarts it if its first start fails), its hook
   applies the committed network, and the native boot qualifies.
4. Remove the temporary network.

Signed uninstall is destructive removal, not restoration of predecessor state.
It removes the Web UI, route pruning, the process guard, project SSH, and the
compressed controller/runtime; the next boot returns to OEM/GoAhead behavior,
with the OEM application running unconfined. The media layer's files in
`/opt/jooanipc-open` and its release in the appfs stay on flash, but nothing
starts them once `local.rc` is gone. Uninstall does not restore the old
unauthenticated telnet hook or an archived predecessor `/opt/etc/local.rc`.
Keep router isolation active before, during, and after uninstall.

## Appfs reflash recovery

`media/scripts/upgrade-appfs.sh` reflashes `/dev/mtd4`, the appfs: it holds the
OEM `startapp`, `jooanipc`, kernel modules, sensor IQ and the media release.
Before it stops the media daemon it takes the platform maintenance lock
(`/run/jooan-local/state.lock`), which pauses the supervisor's runtime restarts
and network-watchdog reboot, and a rootfs `watchdog` feeder takes over the
armed hardware watchdog.

- **Before the commit point** (the configuration switch just before
  `flashcp`) every failure reboots into the untouched appfs.
- **After it**, a flash that fails verification is rolled back from a RAM copy
  of the original appfs, up to three attempts, then the three configuration
  files are restored and the camera reboots.
- **If the rollback fails as well**, the installer does not reboot. The camera
  stays up with the rootfs feeder running, the lock held, and both images in
  `/tmp`: `/tmp/jipc-appfs-rollback.bin` (the original appfs) and
  `/tmp/jipc-flash-candidate.squashfs` (the new image). SSH keeps working,
  because the platform runs from `/run` and the rootfs.

Recover that state over SSH, without rebooting:

```sh
flashcp -v /tmp/jipc-appfs-rollback.bin /dev/mtd4
md5sum /dev/mtd4      # must equal the original appfs MD5
cp /tmp/jipc-install/boot-handoff.sh.before /opt/jooanipc-open/boot-handoff.sh
cp /tmp/jipc-install/camera.ini.before /opt/jooanipc-open/camera.ini
cp /tmp/jipc-install/deployment.env.before /opt/jooanipc-open/deployment.env
chmod 700 /opt/jooanipc-open/boot-handoff.sh
chmod 600 /opt/jooanipc-open/camera.ini /opt/jooanipc-open/deployment.env
sync
reboot
```

The original appfs MD5 is the second argument the installer was given: the
bundle's `expected_current_appfs_md5`, which `install-camera.py` also prints at
preflight. Retry `flashcp` until `md5sum` matches; do not reboot before it
does. `/tmp` is RAM: the images are lost on any reboot.

If power is lost, or the camera reboots, while the appfs is damaged, `startapp`
is missing: nothing sources `local.rc`, so neither the platform nor SSH
starts. Recovery then needs an SPI flash programmer (8 MiB SPI NOR) and a
backup of the appfs partition or of the whole flash, as below.

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
compromised. Rotate the administrator password, replace enrolled SSH keys, and
replace Wi-Fi credentials. Revalidate router isolation before returning the
camera to normal use.

Record why recovery was needed and retain sanitized logs. Do not upload raw
flash images, keys, tokens, cookies, or Wi-Fi configuration to an issue.

## Recovery is complete only when

- the device cold-boots repeatedly;
- both CV2005 paths produce expected video;
- the SKW6316 interface is stable;
- the intended management path works;
- unexpected ports and outbound traffic are absent or blocked;
- the new full-flash read is archived with its hash.
