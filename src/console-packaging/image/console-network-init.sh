#!/bin/sh
# Optional radio service. Runs in the background after the qualified card mount.
# Never owns checkpoint files, writes on a missing card, or blocks MainUI startup.
set -eu
umask 077
PATH=/system/bin:/usr/sbin:/usr/bin:/sbin:/bin
LD_LIBRARY_PATH=/system/lib:/usr/magicx/lib:/lib:/usr/lib
export PATH LD_LIBRARY_PATH
exec </dev/null >>/run/console/wifi.log 2>&1
[ -d /run/console ] && [ ! -L /run/console ] || exit 1
[ ! -L /run/console/wpa ] || exit 1
mkdir -p /run/console/wpa
chmod 0700 /run/console/wpa
if [ ! -e /sys/class/net/wlan0 ]; then
    # The original Moss config selects CONFIG_RTL8189ES=m. No forced loading
    # or module from an unrelated kernel is accepted here.
    modprobe 8189es || echo 'console-network: matching 8189es kernel module unavailable'
fi
attempt=0
while [ ! -e /sys/class/net/wlan0 ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 20 ] || { echo 'console-network: no wlan0; UI remains usable'; exit 1; }
    sleep 0.5
done
# Supplicant owns a private tmpfs config. The supervisor alone publishes its
# bounded SAVE_CONFIG result to FAT with file/directory fsync and atomic rename.
[ ! -L /run/console/wpa.conf ] || exit 1
config=/mnt/SDCARD/.console/wifi.conf
[ ! -L /mnt/SDCARD/.console ] && [ -d /mnt/SDCARD/.console ] || exit 1
if [ "${1:-}" = --restore ]; then
    # Easy Netplay uses a different tmpfs profile. Preserve the live station
    # profile including settings changed since boot and the last chosen SSID.
    [ -f /run/console/wpa.conf ] && [ -s /run/console/wpa.conf ] || exit 1
elif [ -e "$config" ] || [ -L "$config" ]; then
    [ -f "$config" ] && [ ! -L "$config" ] || exit 1
    bytes=$(wc -c < "$config")
    [ "$bytes" -gt 0 ] && [ "$bytes" -lt 65536 ] || exit 1
    cat "$config" > /run/console/wpa.conf
else
    printf 'ctrl_interface=/run/console/wpa\nupdate_config=1\n' > /run/console/wpa.conf
fi
chmod 0600 /run/console/wpa.conf
/system/bin/wpa_supplicant -B -P /run/console/wpa.pid -D nl80211 -i wlan0 \
    -c /run/console/wpa.conf -O /run/console/wpa || exit 1
# DHCP retries independently while association is asynchronous.
/sbin/udhcpc -b -i wlan0 -p /run/console/dhcp.pid \
    -s /system/init/console-dhcp.sh -t 3 -T 3 -A 10
