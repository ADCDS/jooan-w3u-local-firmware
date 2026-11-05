#!/usr/bin/env python3
"""Boot storage qualification accepts only canonical, mounted release roots."""
import subprocess
import sys

script = sys.argv[1]
internal = "/mnt/mtd/jooanipc-open/releases/a507d3cb-904d60a6"
sd = internal.replace("/mnt/mtd/", "/mnt/sd_card/", 1)
appfs = "/dev/mtdblock4 /mnt/mtd squashfs ro,relatime 0 0\n"
sd_mount = "/dev/mmcblk0p1 /mnt/sd_card vfat rw,relatime 0 0\n"
rootfs = "/dev/root / squashfs ro,relatime 0 0\n"


def check(release, mounts, expected, canonical=None):
    result = subprocess.run(
        ["sh", script, "--check-release-storage", release,
         release if canonical is None else canonical],
        input=mounts.encode(), capture_output=True, timeout=3,
    )
    assert result.returncode == expected, (
        result.returncode, result.stderr, release, canonical, mounts,
    )


# Internal releases require their own appfs, even with no SD card installed.
check(internal, rootfs + appfs, 0)
check(internal, rootfs + appfs + sd_mount, 0)
check(internal, rootfs + sd_mount, 1)
check(internal, rootfs, 1)
check(internal, appfs.replace("mtdblock4", "mtdblock3"), 1)
check(internal, appfs.replace("squashfs", "tmpfs"), 1)
check(internal, appfs.replace("ro,relatime", "rw,relatime"), 1)
check(internal, appfs.replace("ro,relatime", "errors=remount-ro,rw"), 1)
check(internal, appfs.replace("/mnt/mtd ", "/mnt/mtd-other "), 1)
check(internal, appfs + "tmpfs /mnt/mtd tmpfs rw 0 0\n", 1)
for mountpoint in (
    "/mnt/mtd/jooanipc-open",
    "/mnt/mtd/jooanipc-open/releases",
    internal,
    internal + "/jooanipc-open",
):
    check(internal, appfs + f"tmpfs {mountpoint} tmpfs rw 0 0\n", 1)
check(internal, appfs + "tmpfs /mnt/mtd/unrelated tmpfs rw 0 0\n", 0)

# Keep existing SD release support; an appfs mount does not qualify missing SD.
check(sd, rootfs + sd_mount, 0)
check(sd, rootfs + appfs + sd_mount, 0)
check(sd, rootfs + appfs, 1)
check(sd, sd_mount.replace("/mnt/sd_card ", "/mnt/sd_card-other "), 1)

# The allowlist covers one named release, not traversal or path aliases.
for invalid in (
    "", "/tmp/jooanipc-open/releases/a507d3cb-904d60a6",
    "/mnt/mtd/jooanipc-open/releases", internal + "/",
    internal + "/../other", internal + "/child", internal + "/../../../../opt",
    internal.rsplit("/", 1)[0] + "/.",
    internal.rsplit("/", 1)[0] + "/..",
    internal.rsplit("/", 1)[0] + "/",
    internal.replace("/releases/", "/releases//"),
    internal.replace("/jooanipc-open/", "/jooanipc-open/../jooanipc-open/"),
    internal + " with-space", internal + "\nother", internal + ";command",
):
    check(invalid, rootfs + appfs + sd_mount, 1)
check(internal, appfs, 1, canonical=sd)
check(sd, sd_mount, 1, canonical=internal)
check(internal, appfs, 1, canonical="/tmp/aliased-release")
check(sd, sd_mount, 1, canonical="/tmp/aliased-release")

print("boot release storage: internal appfs, SD, absent mounts and path rejection passed")
