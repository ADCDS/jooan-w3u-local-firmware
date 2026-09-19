#!/bin/sh
# Host entrypoint: build only the standalone rollback/uninstall OTA package.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$REPO/packaging/build-release.py" package --kind uninstall "$@"
