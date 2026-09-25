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
node web/ptz-steps.test.js
node web/video-player.test.js
node web/spatial.test.js
node web/audio-codec.test.js
python3 tests/run_optional_vectors.py network_guard --schema-only
ci/check_guard.sh
python3 tests/run_optional_vectors.py audio --schema-only
ci/check_audio.sh
ci/check_rtsp_proxy.sh
python3 ci/check_reproducible.py
