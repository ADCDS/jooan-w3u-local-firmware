#!/bin/sh
# Bounded local PTZ adapter for the exact JA-A12 OEM ONVIF service.
set -eu

action=${1:-}
direction=${2:-}
speed=${3:-3}
duration_ms=${4:-250}
run=${JL_RUN:-/run/jooan-local}
port=${JOAN_ONVIF_PORT:-8899}
profile=profile_0
request=$run/onvif-ptz.$$.request
response=$run/onvif-ptz.$$.response
trap 'rm -f "$request" "$response"' EXIT HUP INT TERM

post() {
    soap_action=$1
    expected=$2
    length=$(wc -c < "$request" | tr -d ' ')
    {
        printf 'POST /onvif/Ptz HTTP/1.0\r\n'
        printf 'Host: 127.0.0.1:%s\r\n' "$port"
        printf 'Content-Type: application/soap+xml; charset=utf-8; action="http://www.onvif.org/ver20/ptz/wsdl/%s"\r\n' "$soap_action"
        printf 'Content-Length: %s\r\nConnection: close\r\n\r\n' "$length"
        cat "$request"
    } | nc -w2 127.0.0.1 "$port" > "$response"
    grep -q '^HTTP/1\.[01] 200 ' "$response"
    ! grep -q '<env:Fault>' "$response"
    grep -q "<tptz:${expected}Response" "$response"
}

case "$action" in
    stop)
        cat > "$request" <<EOF
<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl"><s:Body><tptz:Stop><tptz:ProfileToken>$profile</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>true</tptz:Zoom></tptz:Stop></s:Body></s:Envelope>
EOF
        post Stop Stop
        ;;
    move)
        case "$speed" in
            1) velocity=0.2 ;; 2) velocity=0.4 ;; 3) velocity=0.6 ;;
            4) velocity=0.8 ;; 5) velocity=1.0 ;; *) exit 2 ;;
        esac
        case "$duration_ms" in
            ''|*[!0-9]*) exit 2 ;;
        esac
        [ "$duration_ms" -ge 50 ] && [ "$duration_ms" -le 1000 ] || exit 2
        case "$direction" in
            up) x=0.0; y=$velocity ;;
            down) x=0.0; y=-$velocity ;;
            left) x=-$velocity; y=0.0 ;;
            right) x=$velocity; y=0.0 ;;
            *) exit 2 ;;
        esac
        if [ "$duration_ms" = 1000 ]; then
            timeout=PT1S
        else
            # ISO-8601 fractional seconds are decimal fractions: 50 ms must
            # be written .050, not .50 (which would move for ten times longer).
            case "$duration_ms" in
                [0-9][0-9]) milliseconds=0$duration_ms ;;
                [0-9][0-9][0-9]) milliseconds=$duration_ms ;;
                *) exit 2 ;;
            esac
            timeout=PT0.${milliseconds}S
        fi
        cat > "$request" <<EOF
<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:tt="http://www.onvif.org/ver10/schema"><s:Body><tptz:ContinuousMove><tptz:ProfileToken>$profile</tptz:ProfileToken><tptz:Velocity><tt:PanTilt x="$x" y="$y"/></tptz:Velocity><tptz:Timeout>$timeout</tptz:Timeout></tptz:ContinuousMove></s:Body></s:Envelope>
EOF
        post ContinuousMove ContinuousMove
        ;;
    *) exit 2 ;;
esac
