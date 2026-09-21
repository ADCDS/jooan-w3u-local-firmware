#!/bin/sh
# Host-only tests. No real mounts, routes, processes, or camera are touched.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fixture=$(mktemp -d /tmp/jooan-runtime-shell.XXXXXX)
case "$fixture" in /tmp/jooan-runtime-shell.*) ;; *) exit 1 ;; esac
trap 'rm -rf "$fixture"' EXIT
JL_ROOT=$fixture/root JL_RUN=$fixture/run JL_CONTROL=$repo/runtime
export JL_ROOT JL_RUN JL_CONTROL
. "$repo/runtime/boot/common.sh"
jl_init_run

# Public resolvers disappear; private/loopback/ULA resolvers and metadata stay.
printf '%s\n' 'search bench.local' 'nameserver 8.8.8.8' 'nameserver 10.42.0.1' \
    'nameserver 172.16.1.1' 'nameserver 172.32.1.1' 'nameserver 192.168.1.1' \
    'nameserver 127.0.0.1' 'nameserver ::1' 'nameserver fd00::1' \
    'nameserver 2001:4860:4860::8888' | jl_private_resolvers > "$fixture/dns"
[ "$(wc -l < "$fixture/dns")" = 7 ]
! grep -q '8.8.8.8\|172.32\|2001:4860' "$fixture/dns"

# Mock the route utility, proving only exact defaults are selected for deletion.
route() {
    case "$*" in
        '-n') printf '%s\n' '0.0.0.0 10.42.0.1 0.0.0.0 UG 0 0 0 wlan0' \
            '10.42.0.0 0.0.0.0 255.255.255.0 U 0 0 0 wlan0' ;;
        '-A inet6 -n') printf '%s\n' '::/0 fe80::1 UG 100 0 0 wlan0' \
            'fd00::/64 :: U 100 0 0 wlan0' ;;
        *) printf '%s\n' "$*" >> "$fixture/route-actions" ;;
    esac
}
jl_prune_default_routes
[ "$(wc -l < "$fixture/route-actions")" = 2 ]
grep -q '^del default gw 10.42.0.1 dev wlan0$' "$fixture/route-actions"
grep -q '^-A inet6 del ::/0 gw fe80::1 dev wlan0$' "$fixture/route-actions"
# The pruned gateway is remembered so allowlisted routes can still use it.
[ "$(cat "$JL_RUN/default-gateway")" = '10.42.0.1 wlan0' ]
[ "$(cat "$JL_RUN/default-gateway6")" = 'fe80::1 wlan0' ]

# Fixture hashes are nonfunctional placeholders, never credentials for a device.
printf '%s\n' 'root:x:0:0:root:/:/bin/sh' 'nobody:x:65534:65534::/:/bin/sh' > "$fixture/oem-passwd"
printf '%s\n' 'admin:$6$fixture$abcdefghijklmnopqrstuvwxyz0123456789' > "$fixture/credential"
jl_make_accounts "$fixture/oem-passwd" "$fixture/credential" "$fixture/accounts" "$JL_RUN/admin-home"
awk -F: '$1=="admin" && $2=="x" && $3==0 && $7=="/bin/sh" {ok++} END {exit ok!=1}' "$fixture/accounts/passwd.new"
awk -F: '$1!="admin" && ($2!="!" || $7!="/nonexistent/jooan-disabled-login") {bad=1} END {exit bad}' "$fixture/accounts/passwd.new"
awk -F: '$1=="admin" && $2 ~ /^\$6\$fixture\$/ {ok=1} END {exit !ok}' "$fixture/accounts/shadow.new"
printf '%s\n' 'root:$6$fixture$abcdefghijklmnopqrstuvwxyz0123456789' > "$fixture/invalid"
if jl_make_accounts "$fixture/oem-passwd" "$fixture/invalid" "$fixture/accounts" "$JL_RUN/admin-home"; then exit 1; fi
printf '%s\n' 'admin:!' > "$fixture/locked"
jl_make_accounts "$fixture/oem-passwd" "$fixture/locked" "$fixture/locked-accounts" "$JL_RUN/admin-home"
grep -q '^admin:!:' "$fixture/locked-accounts/shadow.new"
printf '%s\n' 'admin:$6$fixture$abcdefghijklmnopqrstuvwxyz0123456789:extra' > "$fixture/invalid"
if jl_make_accounts "$fixture/oem-passwd" "$fixture/invalid" "$fixture/accounts" "$JL_RUN/admin-home"; then exit 1; fi

jl_lock
jl_write_selection A B 0
jl_read_selection
[ "$JL_STABLE:$JL_PENDING:$JL_ATTEMPTED" = A:B:0 ]
jl_write_selection B - 0
jl_read_selection
[ "$JL_STABLE:$JL_PENDING:$JL_ATTEMPTED" = B:-:0 ]
jl_write_selection A A 0
if jl_read_selection; then exit 1; fi
jl_unlock

# Stored checksums use BusyBox-compatible MD5 without any persistent verifier.
printf '%s\n' 'archive fixture contents' > "$fixture/archive"
md5sum "$fixture/archive" | awk '{print $1}' > "$fixture/archive.md5"
jl_verify_archive "$fixture/archive" "$fixture/archive.md5"
printf '%s\n' 'modified archive' > "$fixture/archive"
if jl_verify_archive "$fixture/archive" "$fixture/archive.md5"; then exit 1; fi
printf '%s\n' 'not-a-valid-md5' > "$fixture/archive.md5"
if jl_verify_archive "$fixture/archive" "$fixture/archive.md5"; then exit 1; fi
# A SHA-256 sidecar cannot accidentally be accepted under the smaller contract.
printf '%064d\n' 0 > "$fixture/archive.md5"
if jl_verify_archive "$fixture/archive" "$fixture/archive.md5"; then exit 1; fi

# Enforce final storage policy using pure test overrides, never a real partition.
jl_wait_free_kb() { [ "$2" = 64 ]; }
jl_tree_bytes() { printf '%s\n' 196608; }
jl_check_storage
jl_tree_bytes() { printf '%s\n' 196609; }
if jl_check_storage 2>/dev/null; then exit 1; fi
jl_wait_free_kb() { [ "$2" = 56 ]; }
jl_tree_bytes() { printf '%s\n' 196608; }
jl_check_maintenance_storage
jl_tree_bytes() { printf '%s\n' 196609; }
if jl_check_maintenance_storage 2>/dev/null; then exit 1; fi

# Allowlisted private routes are installed through the pruned default gateway,
# as specific prefixes only; a default route is never re-created.
mkdir -p "$JL_CONFIG"
[ "$(jl_netmask_of 24)" = 255.255.255.0 ]
[ "$(jl_netmask_of 8)" = 255.0.0.0 ]
[ "$(jl_netmask_of 12)" = 255.240.0.0 ]
[ "$(jl_netmask_of 32)" = 255.255.255.255 ]
: > "$fixture/route-actions"
# Existing routes are reported so an already-present prefix is not re-added.
route() {
    case "$*" in
        '-n') printf '%s\n' '10.42.0.0 0.0.0.0 255.255.255.0 U 0 0 0 wlan0' \
            '172.16.9.0 192.168.20.1 255.255.255.0 UG 0 0 0 wlan0' ;;
        '-A inet6 -n') printf '%s\n' 'fd00:beef::/64 fe80::1 UG 100 0 0 wlan0' ;;
        *) printf '%s\n' "$*" >> "$fixture/route-actions" ;;
    esac
}
# The pruning above already recorded its gateway; with none remembered at all,
# nothing may be installed.
rm -f "$JL_RUN/default-gateway" "$JL_RUN/default-gateway6"
jl_apply_allowed_routes
[ ! -s "$fixture/route-actions" ]
printf '%s %s\n' 192.168.20.1 wlan0 > "$JL_RUN/default-gateway"
printf '%s %s\n' fe80::1 wlan0 > "$JL_RUN/default-gateway6"
printf '%s\n' '192.168.100.0/24' 'fd00:dead::/64' '0.0.0.0/0' \
    '172.16.9.0/24' 'fd00:beef::/64' > "$JL_CONFIG/routes.list"
jl_apply_allowed_routes
grep -q '^add -net 192.168.100.0 netmask 255.255.255.0 gw 192.168.20.1 dev wlan0$' \
    "$fixture/route-actions"
grep -q '^-A inet6 add fd00:dead::/64 gw fe80::1 dev wlan0$' "$fixture/route-actions"
! grep -q '0\.0\.0\.0/0\|default' "$fixture/route-actions"   # never a default route
! grep -q '172\.16\.9\.0\|fd00:beef' "$fixture/route-actions" # already present
[ "$(wc -l < "$fixture/route-actions")" = 2 ]
unset -f route
rm -f "$JL_CONFIG/routes.list" "$JL_RUN/default-gateway" "$JL_RUN/default-gateway6"

# The supervisor applies the committed network only once wpa_supplicant
# answers, exactly once per boot, and gives up rather than thrashing.
mkdir -p "$JL_RUN/slot-A/hooks"
cat > "$JL_RUN/slot-A/hooks/wifi-apply.sh" <<'HOOK'
#!/bin/sh
printf '%s\n' "$1" >> "$JL_WIFI_CALLS"
[ ! -f "$JL_WIFI_FAIL" ] || exit 1
HOOK
chmod +x "$JL_RUN/slot-A/hooks/wifi-apply.sh"
JL_WIFI_CALLS=$fixture/wifi-calls; export JL_WIFI_CALLS
: > "$JL_WIFI_CALLS"
printf '{"ssid":"x","password":"y"}\n' > "$JL_CONFIG/wifi.json"
wpa_cli() { return 1; }                       # control socket not up yet
jl_ensure_wifi A
[ ! -s "$JL_WIFI_CALLS" ]
wpa_cli() { printf 'PONG\n'; }                # wpa_supplicant now answering
jl_ensure_wifi A
[ "$(wc -l < "$JL_WIFI_CALLS")" = 1 ]
jl_ensure_wifi A                              # already applied: not repeated
[ "$(wc -l < "$JL_WIFI_CALLS")" = 1 ]
# A hook that keeps failing is retried, but only up to the cap.
rm -f "$JL_RUN/wifi-applied" "$JL_RUN/wifi-attempts"
: > "$JL_WIFI_CALLS"
JL_WIFI_FAIL=$fixture/wifi-fail; export JL_WIFI_FAIL; : > "$JL_WIFI_FAIL"
i=0; while [ "$i" -lt 9 ]; do jl_ensure_wifi A 2>/dev/null || :; i=$((i + 1)); done
[ "$(wc -l < "$JL_WIFI_CALLS")" = 5 ]
[ ! -f "$JL_RUN/wifi-applied" ]
unset -f wpa_cli
rm -f "$JL_WIFI_FAIL" "$JL_CONFIG/wifi.json" "$JL_RUN/wifi-applied" "$JL_RUN/wifi-attempts"

# Promotion/rollback cleanup is idempotent and retains exactly the selected slot.
mkdir -p "$JL_ROOT/slots/A" "$JL_ROOT/slots/B"
printf stable > "$JL_ROOT/slots/A/runtime.tar.gz"
printf candidate > "$JL_ROOT/slots/B/runtime.tar.gz"
jl_tree_bytes() { find "$JL_ROOT" -type f -printf '%s\n' | awk '{n+=$1} END {print n+0}'; }
jl_wait_free_kb() { return 0; }
jl_prune_other_slots A
[ -d "$JL_ROOT/slots/A" ] && [ ! -e "$JL_ROOT/slots/B" ]
jl_prune_other_slots A

for script in "$repo"/runtime/boot/*.sh "$repo"/runtime/boot/local.rc \
    "$repo"/runtime/admin/*.sh "$repo"/runtime/slot/*.sh "$repo"/runtime/slot/hooks/*.sh; do
    sh -n "$script"
done
printf '%s\n' 'PASS: DNS filtering, routes, locked recovery account, single-runtime maintenance/pruning, MD5 corruption checks, storage boundaries, shell syntax'
