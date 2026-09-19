#!/bin/sh
# Assemble deterministic OTA stage directories from deployable target outputs.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
target=$repo/build/target
stage=$repo/build/release-stage
controller=$repo/build/controller-tree
runtime=$repo/build/runtime-tree
budget=$repo/build/persistent-budget
version=$(cat "$repo/packaging/VERSION")

for file in "$target/bin/joan-daemon" "$target/shared/libjooan_guard.so" \
    "$target/shared/jooan-sha256" "$target/shared/dropbear.tar.gz"; do
    [ -f "$file" ] || { echo "missing target output: $file" >&2; exit 1; }
done
[ -x "$target/bin/audio-router" ] || { echo 'missing target audio-router' >&2; exit 1; }
[ ! -e "$stage" ] && [ ! -e "$controller" ] && [ ! -e "$runtime" ] && [ ! -e "$budget" ] || {
    echo 'remove the generated build/release-stage, controller-tree and runtime-tree before rebuilding' >&2
    exit 1
}
mkdir -p "$stage/install" "$stage/uninstall" "$controller/boot" \
    "$controller/admin" "$controller/shared" "$runtime/bin" \
    "$runtime/web" "$runtime/hooks" "$budget"

cp "$repo/runtime/boot/local.rc" "$repo/runtime/boot/boot.sh" \
    "$repo/runtime/boot/common.sh" "$controller/boot/"
cp "$repo/runtime/admin/install-controller.sh" "$repo/runtime/admin/install-runtime.sh" \
    "$repo/runtime/admin/mark-healthy.sh" "$repo/runtime/admin/ssh-start.sh" \
    "$repo/runtime/admin/wifi-transaction.sh" "$controller/admin/"
cp "$repo/runtime/boot/verify-guard.sh" "$controller/shared/verify-guard.sh"
cp "$target/shared/jooan-sha256" "$target/shared/libjooan_guard.so" \
    "$target/shared/guard.sha256" "$target/shared/dropbear.tar.gz" \
    "$target/shared/dropbear.sha256" "$controller/shared/"

cp "$target/bin/joan-daemon" "$target/bin/audio-router" "$runtime/bin/"
cp "$repo/runtime/slot/start.sh" "$repo/runtime/slot/stop.sh" \
    "$repo/runtime/slot/health.sh" "$runtime/"
cp "$repo/runtime/slot/hooks/"*.sh "$runtime/hooks/"
cp "$repo/web/"*.html "$repo/web/"*.css "$repo/web/"*.js "$runtime/web/"
find "$controller" "$runtime" -type f -name '*.sh' -exec chmod 755 {} \;

cp -R "$controller" "$stage/install/controller"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$controller" -cf - . | gzip -9n > "$budget/controller.tar.gz"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$runtime" -cf - . | gzip -9n > "$stage/install/runtime.tar.gz"
sha256sum "$stage/install/runtime.tar.gz" | awk '{print $1}' > "$stage/install/runtime.sha256"
cp "$stage/install/runtime.tar.gz" "$budget/runtime-A.tar.gz"
cp "$stage/install/runtime.tar.gz" "$budget/runtime-B.tar.gz"
cp "$target/shared/jooan-sha256" "$stage/install/"
cp "$repo/packaging/payload/install-upgrade.sh" "$stage/install/upgrade.sh"
chmod 755 "$stage/install/upgrade.sh" "$stage/install/jooan-sha256"

cat > "$stage/install/compatibility.sha256" <<'HASHES'
f2e46d4b897bca54a30941b98a08327b0af37dc96d577de91486adc5ed920ffd  /bin/goahead
ab8ac8328011053b41889b2ef75eefc036a85cef7f8741c12bbb678dd1dce29e  /lib/ko/skw6316.ko
02be360a7ff00409965a3b96a5f47e507c30866f480ae1e5ba667d0763d820c0  /lib/ko/skw_usb_lite.ko
e5080976a0f9d45e6621746a325fa26e13783ecb558d10d160c7848157f310ca  /mnt/mtd/lib/modules/sensor_cv2005.ko
c245eeb74c54c2174d861f449c10e458fddff708f1e3e84d1338f65608bb0eed  /mnt/mtd/lib/modules/sensor_cv2005s1.ko
edd1afa9f89f74f60d23fc56a1347f9b406400d38a23aa46ebcc45983fd09355  /mnt/mtd/run/jooanipc
23480f1449fb9017b49283bbf12ae6a3db6df833ca89a14075df9d6c25f8d8c1  /mnt/mtd/startapp
HASHES
(cd "$stage/install" && find controller -type f -print | LC_ALL=C sort | \
    xargs sha256sum > payload.sha256)
(cd "$stage/install" && sha256sum runtime.tar.gz runtime.sha256 compatibility.sha256 jooan-sha256 >> payload.sha256)

cp "$repo/packaging/payload/uninstall-upgrade.sh" "$stage/uninstall/upgrade.sh"
chmod 755 "$stage/uninstall/upgrade.sh"
printf '%s\n' "$version" > "$stage/install/RELEASE"
printf '%s\n' "$version" > "$stage/uninstall/RELEASE"

du -h "$stage/install/"* "$stage/uninstall/"*
printf '%s\n' "$stage/install" "$stage/uninstall"
