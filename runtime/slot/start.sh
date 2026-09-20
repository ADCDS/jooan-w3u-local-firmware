#!/bin/sh
set -eu
: "${JL_ROOT:=/opt/custom/jooan-local}"
: "${JL_RUN:=/run/jooan-local}"
: "${JL_SLOT_DIR:?slot directory is required}"
PATH=/bin:/sbin:/usr/bin:/usr/sbin:/mnt/mtd/run
export PATH

mkdir -p "$JL_RUN"
chmod 700 "$JL_RUN"
"$JL_SLOT_DIR/hooks/entropy-ready.sh" || exit 1

# The independent boot watcher also suppresses later OEM absolute-path starts.
killall goahead 2>/dev/null || :

if [ -x "$JL_SLOT_DIR/bin/audio-router" ]; then
    "$JL_SLOT_DIR/bin/audio-router" \
        --ws-socket "$JL_RUN/audio-ws.sock" \
        --mic-socket "$JL_RUN/mic.sock" \
        --guard-socket /tmp/jooan-guard-talkback.sock \
        >"$JL_RUN/audio-router.log" 2>&1 &
    echo $! > "$JL_RUN/audio-router.pid"
fi

if [ -f "$JL_ROOT/config/wifi.json" ] &&
   [ -x "$JL_SLOT_DIR/hooks/wifi-apply.sh" ]; then
    "$JL_SLOT_DIR/hooks/wifi-apply.sh" "$JL_ROOT/config/wifi.json" \
        >"$JL_RUN/wifi-boot.log" 2>&1 || :
fi

JOAN_STATE_DIR="$JL_ROOT/config" \
JOAN_RELEASE_SEQUENCE_PATH="$JL_ROOT/state/release-sequence" \
JOAN_STAGING_DIR="$JL_RUN/staging" \
JOAN_WEB_DIR="$JL_SLOT_DIR/web" \
JOAN_INTEGRATION_HELPER="$JL_SLOT_DIR/hooks/integration-helper.sh" \
JOAN_AUDIO_WS_SOCKET="$JL_RUN/audio-ws.sock" \
JOAN_AUDIO_MIC_SOCKET="$JL_RUN/mic.sock" \
JOAN_MQTT_PORT=1883 JOAN_RTSP_PORT=8554 JOAN_RTSP_PROXY_PORT=554 \
JOAN_PUBLIC_HOST="jooan-w3u.local" \
    "$JL_SLOT_DIR/bin/joan-daemon" >"$JL_RUN/daemon.log" 2>&1 &
echo $! > "$JL_RUN/daemon.pid"
exit 0
