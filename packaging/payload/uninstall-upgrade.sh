#!/bin/sh
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
root=/opt/custom/jooan-local
hook=/opt/etc/local.rc

killall telnetd 2>/dev/null || :
if [ -x "$root/slots/A/stop.sh" ]; then "$root/slots/A/stop.sh" || :; fi
if [ -x "$root/slots/B/stop.sh" ]; then "$root/slots/B/stop.sh" || :; fi
if [ -f "$hook" ] && grep -q '/opt/custom/jooan-local' "$hook"; then
    rm -f "$hook"
fi
# The pre-install hook is deliberately not restored: it may contain the old
# unauthenticated telnet bootstrap. Uninstall returns to clean OEM startup.
if [ -d "$root" ]; then
    rm -rf "$root"
fi
sync
echo 'jooan-local removed; OEM media will return without the exploit telnet hook'
exit 0
