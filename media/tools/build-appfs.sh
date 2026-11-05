#!/bin/sh
# SPDX-License-Identifier: MIT
# Host tool (runs on the workstation, not the camera). Rebuild a JA-A12 appfs
# image with only the jooanipc-open release directory swapped, for
# install-camera.py. BASE_IMAGE is a copy of the camera's own appfs
# (/dev/mtdblock4, 3211264 bytes); it contains vendor files, so keep the
# input and output private. Needs squashfs-tools (unsquashfs, mksquashfs).
# Usage: build-appfs.sh BASE_IMAGE OLD_RELEASE_ID STRIPPED_BINARY OUT_DIR
# Prints the new release ID and the md5 sums bundle.json needs.
set -eu
base=$1 old=$2 bin=$3 out=$4
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
unsquashfs -q -d "$work/root" "$base" >/dev/null
rel=$work/root/jooanipc-open/releases
[ -d "$rel/$old" ] || { echo "old release $old missing" >&2; exit 1; }
app=$(sha256sum "$bin" | cut -c1-8)
imp=$(sha256sum "$rel/$old/libimp.so" | cut -c1-8)
new=$app-$imp
mkdir "$rel/$new"
cp "$bin" "$rel/$new/jooanipc-open"; chmod 755 "$rel/$new/jooanipc-open"
cp -p "$rel/$old/libimp.so" "$rel/$old/libatomic.so.1" "$rel/$new/"
chmod 644 "$rel/$new/libimp.so" "$rel/$new/libatomic.so.1"; chmod 775 "$rel/$new"
rm -rf "${rel:?}/$old"
mkdir -p "$out"
mksquashfs "$work/root" "$out/appfs.raw" -comp xz -b 1048576 -all-root -no-xattrs -noappend -quiet >/dev/null
size=$(wc -c < "$out/appfs.raw")
[ "$size" -le 3211264 ] || { echo "image too large: $size" >&2; exit 1; }
{ cat "$out/appfs.raw"; head -c $((3211264 - size)) /dev/zero | tr '\0' '\377'; } > "$out/appfs.img"
rm "$out/appfs.raw"
echo "release=$new"
echo "squashfs_bytes=$size free_bytes=$((3211264 - size))"
echo "image_md5=$(md5sum "$out/appfs.img" | cut -c1-32)"
echo "app_md5=$(md5sum "$bin" | cut -c1-32)"
