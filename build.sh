#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Host entrypoint: build deterministic install and uninstall OTA packages.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$REPO/packaging/build-release.py" release "$@"
