#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Full gate. Needs TOOLCHAIN_ROOT, OEM_ROOTFS and the release signing key;
# ci/check-hosted.sh is the part that runs without them.
set -eu

root=$(git rev-parse --show-toplevel)
cd "$root"

ci/check-hosted.sh
ci/check_cookie_header.sh
ci/check_ntp.sh
# Media layer for the camera: MIPS build, QEMU replay run, flash budget.
make -C media mips qemu-test
(cd media && python3 tools/check-size.py)
# Builds the release twice and size-checks the fresh persistent layout.
python3 ci/check_reproducible.py
