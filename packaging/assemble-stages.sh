#!/bin/sh
# Assemble deterministic, signed install/uninstall stages and logical budget.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${BUILD_ROOT:-$repo/build}
target=$build/target
stage=$build/release-stage
controller=$build/controller-tree
runtime=$build/runtime-tree
budget=$build/persistent-layout
contract=$repo/packaging/targets/ja-a12.json
version=$(cat "$repo/packaging/VERSION")
sequence=$(cat "$repo/packaging/RELEASE_SEQUENCE")

case "$build" in /|"$repo") echo "unsafe BUILD_ROOT: $build" >&2; exit 1 ;; esac
for file in "$target/bin/joan-daemon" "$target/bin/audio-router" \
    "$target/shared/libjooan_guard.so" "$target/shared/jooan-sha256" \
    "$target/shared/jooan-auth-verify" "$target/shared/jooan-ironman-inspect" \
    "$target/shared/dropbear.tar.gz" "$build/build-provenance.json"; do
    [ -f "$file" ] || { echo "missing target output: $file" >&2; exit 1; }
done

rm -rf "$stage" "$controller" "$runtime" "$budget"
mkdir -p "$stage/install" "$stage/uninstall" "$controller/boot" \
    "$controller/admin" "$controller/shared" "$runtime/bin" \
    "$runtime/web" "$runtime/hooks" "$budget/slots/stable" \
    "$budget/recovery" "$budget/state" "$budget/config"

# local.rc is the only persistent executable. Everything else in the controller
# is authenticated, compressed at rest, and expanded to tmpfs at boot.
cp "$repo/runtime/boot/boot.sh" "$repo/runtime/boot/common.sh" "$controller/boot/"
cp "$repo/runtime/admin/install-controller.sh" "$repo/runtime/admin/install-runtime.sh" \
    "$repo/runtime/admin/mark-healthy.sh" "$repo/runtime/admin/ssh-start.sh" \
    "$repo/runtime/admin/wifi-transaction.sh" "$controller/admin/"
cp "$repo/runtime/boot/verify-guard.sh" "$controller/shared/verify-guard.sh"
cp "$repo/runtime/slot/hooks/entropy-ready.sh" "$controller/shared/entropy-ready.sh"
cp "$target/shared/libjooan_guard.so" \
    "$target/shared/jooan-auth-verify" "$target/shared/jooan-ironman-inspect" \
    "$controller/shared/"
(cd "$controller/shared" && md5sum libjooan_guard.so | awk '{print $1}' > guard.md5)

cp "$target/bin/joan-daemon" "$target/bin/audio-router" "$runtime/bin/"
cp "$repo/runtime/slot/start.sh" "$repo/runtime/slot/stop.sh" \
    "$repo/runtime/slot/health.sh" "$runtime/"
cp "$repo/runtime/slot/hooks/"*.sh "$runtime/hooks/"
find "$repo/web" -maxdepth 1 -type f \( -name '*.html' -o -name '*.css' -o \
    -name '*.js' -o -name '*.svg' -o -name '*.webmanifest' \) \
    ! -name '*.test.js' -print | LC_ALL=C sort | while IFS= read -r file; do
        cp "$file" "$runtime/web/"
    done

find "$controller" "$runtime" -type f -name '*.sh' -print | while IFS= read -r script; do
    awk 'NR == 1 { print; next } /^[[:space:]]*#/ { next } NF { print }' \
        "$script" > "$script.min"
    mv "$script.min" "$script"
done
find "$controller" "$runtime" -type f -name '*.sh' -exec chmod 755 {} \;

tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$controller" -cf - . | gzip -9n > "$stage/install/controller.tar.gz"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C "$runtime" -cf - . | gzip -9n > "$stage/install/runtime.tar.gz"
(cd "$stage/install" && md5sum runtime.tar.gz | awk '{print $1}' > runtime.md5)
cp "$target/shared/dropbear.tar.gz" "$stage/install/recovery.tar.gz"
(cd "$stage/install" && md5sum recovery.tar.gz | awk '{print $1}' > recovery.md5)

awk 'NR == 1 { print; next } /^[[:space:]]*#/ { next } NF { print }' \
    "$repo/runtime/boot/local.rc" > "$stage/install/local.rc"
cp "$target/shared/jooan-sha256" "$target/shared/jooan-auth-verify" \
    "$target/shared/jooan-ironman-inspect" "$stage/install/"
cp "$target/shared/jooan-sha256" "$target/shared/jooan-auth-verify" \
    "$stage/uninstall/"
cp "$build/build-provenance.json" "$stage/install/build-provenance.json"

python3 "$repo/packaging/generate-target-contract.py" compatibility \
    --target "$contract" --output "$stage/install/compatibility.sha256"
python3 "$repo/packaging/generate-target-contract.py" persistent \
    --target "$contract" --output "$stage/install/persistent.contract"
python3 "$repo/packaging/generate-target-contract.py" migration \
    --target "$contract" --output "$stage/install/migration.contract"

awk 'NR == 1 { print; next } /^[[:space:]]*#/ { next } NF { print }' \
    "$repo/packaging/payload/install-upgrade.sh" > "$stage/install/upgrade.sh"
awk 'NR == 1 { print; next } /^[[:space:]]*#/ { next } NF { print }' \
    "$repo/packaging/payload/uninstall-upgrade.sh" > "$stage/uninstall/upgrade.sh"
chmod 755 "$stage/install/upgrade.sh" "$stage/install/local.rc" \
    "$stage/install/jooan-sha256" "$stage/install/jooan-auth-verify" \
    "$stage/install/jooan-ironman-inspect" "$stage/uninstall/upgrade.sh" \
    "$stage/uninstall/jooan-sha256" "$stage/uninstall/jooan-auth-verify"
printf '%s\n' "$version" > "$stage/install/RELEASE"
printf '%s\n' "$version" > "$stage/uninstall/RELEASE"

# SHA-256 remains useful for file diagnostics. release.manifest signs this file.
(cd "$stage/install" && find . -type f ! -name payload.sha256 \
    ! -name release.manifest ! -name release.manifest.sig -print | \
    sed 's|^./||' | LC_ALL=C sort | while IFS= read -r file; do
        sha256sum "$file"
    done > payload.sha256)

sign_one() {
    sign_kind=$1 sign_directory=$2
    if [ -n "${JOOAN_SIGNER_COMMAND:-}" ]; then
        python3 "$repo/packaging/sign-stage.py" sign --stage "$sign_directory" \
            --target "$contract" --kind "$sign_kind" --release-version "$version" \
            --release-sequence "$sequence" --signer-command "$JOOAN_SIGNER_COMMAND"
    else
        python3 "$repo/packaging/sign-stage.py" sign --stage "$sign_directory" \
            --target "$contract" --kind "$sign_kind" --release-version "$version" \
            --release-sequence "$sequence"
    fi
}
sign_one install "$stage/install"
sign_one uninstall "$stage/uninstall"

# Exact steady persistent layout: controller plus one promoted stable runtime.
cp "$stage/install/local.rc" "$budget/local.rc"
cp "$stage/install/controller.tar.gz" "$budget/"
cp "$stage/install/runtime.tar.gz" "$stage/install/runtime.md5" "$budget/slots/stable/"
cp "$stage/install/recovery.tar.gz" "$budget/recovery/dropbear.tar.gz"
cp "$stage/install/recovery.md5" "$budget/recovery/dropbear.md5"
python3 "$repo/ci/check_persistent_size.py" "$budget" --target "$contract" --require

du -h "$stage/install/"* "$stage/uninstall/"*
printf '%s\n' "$stage/install" "$stage/uninstall"
