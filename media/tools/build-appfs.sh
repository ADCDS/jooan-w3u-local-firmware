#!/bin/sh
# SPDX-License-Identifier: MIT
# Host tool (runs on the workstation, not the camera). Rebuild a JA-A12 appfs
# image with only the jooanipc-open release directory swapped, for
# install-camera.py. BASE_IMAGE is a copy of the camera's own appfs
# (/dev/mtdblock4, 3211264 bytes); it contains vendor files, so keep the
# input and output private. Needs squashfs-tools (unsquashfs, mksquashfs).
# Usage: build-appfs.sh BASE_IMAGE OLD_RELEASE_ID STRIPPED_BINARY OUT_DIR [LIBIMP]
# LIBIMP replaces the library the old release carries.
# Prints the new release ID and the sums bundle.json needs.
#
# The first release, from a camera's stock partition (cat /dev/mtdblock4 over SSH):
#   build-appfs.sh --first STOCK_IMAGE STRIPPED_BINARY LIBIMP LIBATOMIC OUT_DIR
# keeps what config/ja-a12-appfs-keep.txt lists, checks the two files the platform pins,
# and writes a whole bundle for install-camera.py --first.
set -eu
if [ "${1:-}" = --first ]; then
    [ "$#" = 6 ] || { echo "usage: build-appfs.sh --first STOCK_IMAGE STRIPPED_BINARY LIBIMP LIBATOMIC OUT_DIR" >&2; exit 2; }
    stock=$2 bin=$3 lib=$4 atomic=$5 out=$6
    here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    keep=${JIPC_APPFS_KEEP:-$here/../config/ja-a12-appfs-keep.txt}
    target=${JIPC_TARGET_JSON:-$here/../../packaging/targets/ja-a12.json}
    [ "$(wc -c < "$stock")" = 3211264 ] || { echo "the stock image must be the whole 3211264-byte partition" >&2; exit 1; }
    work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
    tree=$work/root
    unsquashfs -q -d "$tree" "$stock" >/dev/null
    list=$work/keep
    grep -v -e '^#' -e '^$' "$keep" > "$list"
    while IFS= read -r path; do
        [ -e "$tree/$path" ] || [ -L "$tree/$path" ] || { echo "the stock image lacks $path: this tool was qualified on another stock build" >&2; exit 1; }
    done < "$list"
    (cd "$tree" && find . -depth -mindepth 1) | sed 's#^\./##' | while IFS= read -r path; do
        grep -Fxq -- "$path" "$list" || rm -rf "${tree:?}/$path"
    done
    # The platform's updates depend on these two staying byte-identical.
    for file in run/jooanipc startapp; do
        want=$(sed -n "s#^ *\"/mnt/mtd/$file\": *\"\([0-9a-f]*\)\".*#\1#p" "$target")
        have=$(sha256sum "$tree/$file" | cut -c1-64)
        [ -n "$want" ] && [ "$want" = "$have" ] || { echo "$file is not the version the platform has pinned" >&2; exit 1; }
    done
    imp=$(sha256sum "$lib" | cut -c1-64)
    new=$(sha256sum "$bin" | cut -c1-8)-$(printf %s "$imp" | cut -c1-8)
    rel=$tree/jooanipc-open/releases/$new
    mkdir -p "$rel"
    cp "$bin" "$rel/jooanipc-open"; cp "$lib" "$rel/libimp.so"; cp "$atomic" "$rel/libatomic.so.1"
    chmod 755 "$rel/jooanipc-open"; chmod 644 "$rel/libimp.so" "$rel/libatomic.so.1"; chmod 775 "$rel"
    mkdir -p "$out"
    mksquashfs "$tree" "$out/appfs.raw" -comp xz -b 1048576 -all-root -no-xattrs -noappend -quiet >/dev/null
    size=$(wc -c < "$out/appfs.raw")
    [ "$size" -le $((3211264 - 49152)) ] || { rm -f "$out/appfs.raw"; echo "image too large: $size, with 48 KiB to stay free" >&2; exit 1; }
    { cat "$out/appfs.raw"; head -c $((3211264 - size)) /dev/zero | tr '\0' '\377'; } > "$out/appfs.img"
    rm "$out/appfs.raw"
    scripts=$here/../scripts
    cp "$scripts/first-appfs.sh" "$scripts/native-boot.sh" "$out/"
    cp "$scripts/oem-boot-handoff.sh" "$out/boot-handoff.sh"
    python3 -I - "$out" "$new" "$stock" "$bin" "$lib" "$atomic" <<'EOF'
import hashlib, json, sys
from pathlib import Path
out, release, stock, app, imp, atomic = sys.argv[1:7]
out = Path(out)
md5 = lambda path: hashlib.md5(Path(path).read_bytes()).hexdigest()
spec = {"schema": 1, "name": out.name, "board": "JA-A12", "jooanipc_open": {
    "first": True, "release": release, "image": "appfs.img", "image_md5": md5(out / "appfs.img"),
    "expected_current_appfs_md5": md5(stock), "app_md5": md5(app), "imp_md5": md5(imp),
    "imp_sha256": hashlib.sha256(Path(imp).read_bytes()).hexdigest(), "atomic_md5": md5(atomic),
    "installer": "first-appfs.sh", "installer_md5": md5(out / "first-appfs.sh")}}
(out / "bundle.json").write_text(json.dumps(spec, indent=1) + "\n")
EOF
    echo "release=$new"
    echo "squashfs_bytes=$size free_bytes=$((3211264 - size))"
    echo "bundle=$out"
    exit 0
fi
base=$1 old=$2 bin=$3 out=$4 lib=${5:-}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
unsquashfs -q -d "$work/root" "$base" >/dev/null
rel=$work/root/jooanipc-open/releases
[ -d "$rel/$old" ] || { echo "old release $old missing" >&2; exit 1; }
app=$(sha256sum "$bin" | cut -c1-8)
[ -n "$lib" ] || lib=$rel/$old/libimp.so
imp_sha256=$(sha256sum "$lib" | cut -c1-64)
imp_md5=$(md5sum "$lib" | cut -c1-32)
imp=$(printf %s "$imp_sha256" | cut -c1-8)
new=$app-$imp
mkdir "$rel/$new"
cp "$bin" "$rel/$new/jooanipc-open"; chmod 755 "$rel/$new/jooanipc-open"
cp -p "$lib" "$rel/$old/libatomic.so.1" "$rel/$new/"
chmod 644 "$rel/$new/libimp.so" "$rel/$new/libatomic.so.1"; chmod 775 "$rel/$new"
rm -rf "${rel:?}/$old"
mkdir -p "$out"
mksquashfs "$work/root" "$out/appfs.raw" -comp xz -b 1048576 -all-root -no-xattrs -noappend -quiet >/dev/null
size=$(wc -c < "$out/appfs.raw")
[ "$size" -le $((3211264 - 49152)) ] || { rm -f "$out/appfs.raw"; echo "image too large: $size, with 48 KiB to stay free" >&2; exit 1; }
{ cat "$out/appfs.raw"; head -c $((3211264 - size)) /dev/zero | tr '\0' '\377'; } > "$out/appfs.img"
rm "$out/appfs.raw"
echo "release=$new"
echo "squashfs_bytes=$size free_bytes=$((3211264 - size))"
echo "image_md5=$(md5sum "$out/appfs.img" | cut -c1-32)"
echo "app_md5=$(md5sum "$bin" | cut -c1-32)"
echo "imp_md5=$imp_md5"
echo "imp_sha256=$imp_sha256"
