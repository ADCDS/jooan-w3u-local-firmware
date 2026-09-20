#!/bin/sh
# Full maintainer build: target binaries, staged runtime, install + uninstall OTA.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version=$(cat "$repo/packaging/VERSION")
build_root=${BUILD_ROOT:-$repo/build/release}
build_out=${BUILD_OUT:-$repo/dist}
signing_key=${JOOAN_RELEASE_SIGNING_KEY:-$HOME/.config/jooan-w3u-local-firmware/release-signing-key.pem}
[ -z "$(git -C "$repo" status --porcelain --untracked-files=normal)" ] || {
    echo 'release builds require a clean, committed source tree for provenance' >&2
    exit 1
}
case "$build_root:$build_out" in /*:/*) ;; *) echo 'BUILD_ROOT and BUILD_OUT must be absolute' >&2; exit 1 ;; esac
case "$build_root" in /|"$repo"|"$HOME") echo "unsafe BUILD_ROOT: $build_root" >&2; exit 1 ;; esac
case "$build_out" in /|"$repo"|"$HOME") echo "unsafe BUILD_OUT: $build_out" >&2; exit 1 ;; esac
[ -f "$signing_key" ] || {
    echo "release signing key unavailable; unsigned artifacts are not release-ready" >&2
    echo "back up the external release key securely; never add it to Git" >&2
    exit 1
}
echo "release signing key is external to Git; verify that its encrypted offline backup is current" >&2
rm -rf "$build_root" "$build_out"
mkdir -p "$build_root" "$build_out"
export BUILD_ROOT="$build_root"
"$repo/tools/build-target.sh"
"$repo/packaging/assemble-stages.sh"
exec "$repo/build.sh" --release-version "$version" \
    --install-stage "$build_root/release-stage/install" \
    --uninstall-stage "$build_root/release-stage/uninstall" \
    --out-dir "$build_out" --signing-key "$signing_key"
