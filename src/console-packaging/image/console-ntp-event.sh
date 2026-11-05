#!/bin/sh
# BusyBox ntpd 1.27.2 exports stratum and invokes this after clock changes.
# Retain UTC in the battery-backed RTC; display time zones never change it.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
case "${1:-}" in step|stratum|periodic) ;; *) exit 0;; esac
case "${stratum:-}" in ''|*[!0-9]*) exit 0;; esac
[ "$stratum" -ge 1 ] && [ "$stratum" -lt 16 ] || exit 0
year=$(date -u +%Y)
case "$year" in [2][0][2-9][0-9]|2100) ;; *) exit 0;; esac
[ -c /dev/rtc0 ] || exit 0
if /sbin/hwclock -w -u -f /dev/rtc0; then
    # Volatile status only: repeated synchronizations do not write the SD card.
    date -u '+%Y-%m-%d %H:%M UTC' > /run/console/ntp.synced.tmp
    mv -f /run/console/ntp.synced.tmp /run/console/ntp.synced
    echo "console-clock: synchronized UTC hardware RTC after NTP $1"
else
    echo 'console-clock: UTC hardware RTC write failed' >&2
    exit 1
fi
