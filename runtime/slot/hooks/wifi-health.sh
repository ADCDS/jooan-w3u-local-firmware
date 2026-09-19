#!/bin/sh
set -eu
wpa_cli -iwlan0 status 2>/dev/null | grep -q '^wpa_state=COMPLETED$'
ifconfig wlan0 2>/dev/null | grep -q 'inet addr:'
