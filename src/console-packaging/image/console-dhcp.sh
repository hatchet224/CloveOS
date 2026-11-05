#!/bin/sh
# Fixed udhcpc callback: no eval, executable card paths or lease-provided hooks.
set -eu
umask 077
PATH=/system/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
[ "${interface:-}" = wlan0 ] || exit 1
ipv4() {
    case "$1" in ''|*[!0-9.]*|.*|*.|*..*) return 1;; esac
    local saved_ifs="$IFS" octet
    IFS=.; set -- $1; IFS="$saved_ifs"
    [ "$#" -eq 4 ] || return 1
    for octet in "$@"; do
        [ "${#octet}" -le 3 ] && [ "$octet" -le 255 ] || return 1
    done
}
case "${1:-}" in
    deconfig) ifconfig wlan0 0.0.0.0;;
    bound|renew)
        ipv4 "${ip:-}" && ipv4 "${subnet:-255.255.255.0}" || exit 1
        ifconfig wlan0 "$ip" netmask "${subnet:-255.255.255.0}"
        attempts=0
        while route del default dev wlan0 2>/dev/null; do
            attempts=$((attempts + 1)); [ "$attempts" -lt 8 ] || break
        done
        for gateway in ${router:-}; do
            ipv4 "$gateway" || continue
            route add default gw "$gateway" dev wlan0 && break
        done
        [ ! -L /tmp/resolv.console ] || exit 1
        : > /tmp/resolv.console
        count=0
        for server in ${dns:-}; do
            ipv4 "$server" || continue
            printf 'nameserver %s\n' "$server" >> /tmp/resolv.console
            count=$((count + 1)); [ "$count" -lt 3 ] || break
        done
        mv -f /tmp/resolv.console /tmp/resolv.conf
        # A valid clock is required by HTTPS. This runs independently of the UI.
        /system/bin/console-time-sync --request || echo 'console-clock: NTP startup failed' >&2
        ;;
    *) exit 0;;
esac
