#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Full gate. Needs TOOLCHAIN_ROOT, OEM_ROOTFS and the release signing key;
# ci/check-hosted.sh is the part that runs without them.
set -eu

root=$(git rev-parse --show-toplevel)
cd "$root"

ci/check-hosted.sh
python3 ci/check_persistent_size.py
ci/check_cookie_header.sh
ci/check_ntp.sh
python3 ci/check_reproducible.py
