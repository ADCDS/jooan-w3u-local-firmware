#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Every check that needs no private input: no Ingenic toolchain, no OEM
# rootfs, no release signing key. GitHub Actions runs this on every push;
# ci/check.sh runs it plus the build-dependent gates on the maintainer host.
set -eu

root=$(git rev-parse --show-toplevel)
cd "$root"

python3 ci/source_hygiene.py
python3 ci/check_compatibility.py
ci/check_shell_syntax.sh
python3 ci/check_target_applets.py
python3 -m unittest discover -s tests -p 'test_*.py'
cc -O2 -pthread -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections -o /tmp/jooan-auth-clock-test tests/test_auth_clock.c
/tmp/jooan-auth-clock-test
node web/app-auth-race.test.js
python3 tests/run_optional_vectors.py network_guard --schema-only
ci/check_guard.sh
python3 tests/run_optional_vectors.py audio --schema-only
ci/check_audio.sh
# Media layer: unit tests, then the RTSP/ONVIF/HTTPS integration runs
# against the replay backend (plain and TLS).
make -C media test integration integration-tls
