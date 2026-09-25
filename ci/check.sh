#!/bin/sh
set -eu

root=$(git rev-parse --show-toplevel)
cd "$root"

python3 ci/source_hygiene.py
python3 ci/check_compatibility.py
ci/check_shell_syntax.sh
python3 ci/check_target_applets.py
python3 ci/check_persistent_size.py
python3 -m unittest discover -s tests -p 'test_*.py'
cc -O2 -pthread -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections -o /tmp/jooan-auth-clock-test tests/test_auth_clock.c
/tmp/jooan-auth-clock-test
node web/app-auth-race.test.js
node web/ptz-steps.test.js
node web/video-player.test.js
node web/spatial.test.js
node web/audio-codec.test.js
python3 tests/run_optional_vectors.py network_guard --schema-only
ci/check_guard.sh
python3 tests/run_optional_vectors.py audio --schema-only
ci/check_audio.sh
ci/check_rtsp_proxy.sh
ci/check_cookie_header.sh
python3 ci/check_reproducible.py
