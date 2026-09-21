#!/bin/sh
# Put an externally issued certificate on a camera.
#
# The camera has no route off the LAN, so it cannot answer an ACME challenge
# itself. Solve the challenge here -- DNS-01, which needs no inbound access to
# anything -- and hand the result over with this.
#
# It is deliberately issuer-agnostic: it takes a PEM bundle, which is what
# certbot, lego, acme.sh and dehydrated all already produce. Example with
# certbot and any DNS plugin:
#
#   certbot certonly --dns-<provider> -d cam.example.com
#   cat /etc/letsencrypt/live/cam.example.com/privkey.pem \
#       /etc/letsencrypt/live/cam.example.com/fullchain.pem > bundle.pem
#   tools/enroll-certificate.sh --host cam.example.com --bundle bundle.pem --restart
#
# Point the name at the camera's LAN address in your own DNS. A public record
# holding a private address also works, but some resolvers refuse to return
# one (DNS rebinding protection), so a local override is the reliable choice.
#
# The listener parses its certificate once at startup, so a renewal only takes
# effect after a restart: --restart asks the camera to do that over SSH, whose
# password is the same synchronized admin credential.
set -eu

host='' bundle='' user=admin password='' restart=no insecure=''

usage() {
    cat >&2 <<'USAGE'
usage: enroll-certificate.sh --host <camera> --bundle <pem> [options]

  --host <name>       camera address; also the name checked after enrolling
  --bundle <file>     PEM with the private key and the certificate chain
  --user <name>       administrator user (default: admin)
  --password <pw>     administrator password; else $JOAN_PASSWORD, else prompt
  --restart           restart the camera daemon over SSH so it takes effect
  --insecure          skip TLS verification while enrolling (first run, when
                      the camera still carries its self-signed identity)
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case $1 in
        --host) host=${2:-}; shift 2 ;;
        --bundle) bundle=${2:-}; shift 2 ;;
        --user) user=${2:-}; shift 2 ;;
        --password) password=${2:-}; shift 2 ;;
        --restart) restart=yes; shift ;;
        --insecure) insecure=-k; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$host" ] && [ -n "$bundle" ] || usage
[ -f "$bundle" ] || { echo "no such bundle: $bundle" >&2; exit 1; }
grep -q 'BEGIN .*PRIVATE KEY' "$bundle" || { echo "bundle has no private key" >&2; exit 1; }
grep -q 'BEGIN CERTIFICATE' "$bundle" || { echo "bundle has no certificate" >&2; exit 1; }

if [ -z "$password" ]; then
    password=${JOAN_PASSWORD:-}
fi
if [ -z "$password" ]; then
    printf 'password for %s@%s: ' "$user" "$host" >&2
    stty -echo 2>/dev/null || :
    read -r password
    stty echo 2>/dev/null || :
    printf '\n' >&2
fi

jar=$(mktemp) || exit 1
trap 'rm -f "$jar"' EXIT HUP INT TERM

origin=https://$host
login=$(curl -sS $insecure --max-time 20 -c "$jar" -X POST "$origin/api/v1/session" \
    -H 'Content-Type: application/json' -H "Origin: $origin" \
    -d "{\"username\":\"$user\",\"password\":\"$password\"}") || {
        echo "could not reach $host" >&2; exit 1; }
case $login in *'"ok":true'*) ;; *) echo "sign-in refused: $login" >&2; exit 1 ;; esac
csrf=$(printf '%s' "$login" | sed -n 's/.*"csrf":"\([0-9a-f]*\)".*/\1/p')
[ -n "$csrf" ] || { echo "no csrf token in the sign-in reply" >&2; exit 1; }

reply=$(curl -sS $insecure --max-time 30 -b "$jar" -X PUT "$origin/api/v1/tls/identity" \
    -H "X-CSRF-Token: $csrf" -H "Origin: $origin" \
    -H 'Content-Type: application/x-pem-file' --data-binary @"$bundle")
case $reply in
    *'"ok":true'*) echo "enrolled: $(curl -sS $insecure --max-time 15 -b "$jar" "$origin/api/v1/tls/identity")" ;;
    *) echo "rejected: $reply" >&2; exit 1 ;;
esac

if [ "$restart" = yes ]; then
    # The daemon is supervised, so ending it is how it is restarted.
    if command -v sshpass >/dev/null 2>&1; then
        sshpass -p "$password" ssh -o StrictHostKeyChecking=accept-new \
            "$user@$host" 'kill $(cat /var/run/jooan-local/daemon.pid)' 2>/dev/null || :
    else
        ssh -o StrictHostKeyChecking=accept-new "$user@$host" \
            'kill $(cat /var/run/jooan-local/daemon.pid)' || :
    fi
    echo 'restart requested; the supervisor brings the daemon back in a few seconds'
    # Verify the new identity is served AND trusted, with no -k: that is the
    # whole point of the exercise, so do not claim it without checking.
    n=0
    while [ "$n" -lt 30 ]; do
        if curl -sS --max-time 5 "$origin/api/v1/setup/status" >/dev/null 2>&1; then
            echo "verified: $host now presents a certificate your system trusts"
            exit 0
        fi
        n=$((n + 1))
        sleep 2
    done
    echo "warning: $host did not verify cleanly yet; check the name and your DNS" >&2
    exit 1
fi

echo 'restart the camera (or re-run with --restart) for the new certificate to be served'
