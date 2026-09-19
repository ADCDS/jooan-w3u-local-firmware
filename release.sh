#!/bin/sh
# Full maintainer build: target binaries, staged runtime, install + uninstall OTA.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version=$(cat "$repo/packaging/VERSION")
"$repo/tools/build-target.sh"
"$repo/packaging/assemble-stages.sh"
exec "$repo/build.sh" --release-version "$version" \
    --install-stage "$repo/build/release-stage/install" \
    --uninstall-stage "$repo/build/release-stage/uninstall"
