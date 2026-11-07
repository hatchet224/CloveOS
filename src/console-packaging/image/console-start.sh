#!/bin/sh
# S95done invokes this after stock display/audio/nativepower initialization.
# Failure displays diagnostics and waits for a physical-button retry.
set -eu
umask 077
PATH=/system/bin:/usr/magicx/bin:/usr/sbin:/usr/bin:/sbin:/bin
LD_LIBRARY_PATH=/system/lib:/usr/magicx/lib:/lib:/usr/lib
export PATH LD_LIBRARY_PATH
. /system/init/console-common.sh
boot_mark() {
    read -r boot_uptime boot_idle < /proc/uptime || return 0
    echo "boot timing: stage=$1 uptime_seconds=$boot_uptime"
}
[ ! -L /run ] || exit 1
if ! console_is_mounted /run; then
    [ -d /run ] && [ -z "$(ls -A /run)" ] || exit 1
    mount -t tmpfs -o mode=0755,size=128m tmpfs /run || exit 1
fi
[ ! -L /run/console ] || exit 1
mkdir -p /run/console
chmod 0700 /run/console
/system/bin/console-ctl runtime-check || exit 1
exec </dev/null >>/run/console/startup.log 2>&1
persist_log() {
    # No writes below a missing mount point or onto an unrelated volume.
    /system/bin/console-ctl verify-mount /mnt/SDCARD /dev/disk/by-uuid/C028-0002 C028-0002 vfat rw || return 0
    logdir=/mnt/SDCARD/.tmp_update/logs
    [ ! -L /mnt/SDCARD/.tmp_update ] && [ ! -L "$logdir" ] || return 0
    mkdir -p "$logdir" || return 0
    # The supervisor diagnostic worker archives and persists supervisor.log.
    # Writing its current boot header here would destroy the previous boot's
    # gameplay evidence before that worker gets its first archive request.
    for name in startup startup-error; do
        [ -f "/run/console/$name.log" ] || continue
        target=bootstrap.log
        [ "$name" != startup-error ] || target=startup-error.log
        [ ! -L "$logdir/$target" ] && [ ! -L "$logdir/$target.new" ] || continue
        if tail -c 131072 "/run/console/$name.log" > "$logdir/$target.new"; then
            sync
            mv "$logdir/$target.new" "$logdir/$target" || :
            sync
        fi
    done
}
boot_mark stock-hardware-ready
echo 'console-start: stock hardware initialization completed'
while :; do
    rm -f /run/console/startup-error.log
    echo starting-session > /run/console/boot.phase
    if /system/bin/console-rc start; then
        echo supervisor-spawned > /run/console/boot.phase
        echo 'console-start: supervisor started; visible UI diagnostics handle renderer failures'
        boot_mark supervisor-spawned
        persist_log
        exit 0
    else
        result=$?
        echo startup-failed > /run/console/boot.phase
        echo "console-start: bootstrap failed ($result); waiting for physical retry"
        if [ ! -s /run/console/startup-error.log ]; then
            tail -c 8192 /run/console/startup.log > /run/console/startup-error.log || :
        fi
        persist_log
        if /system/bin/console-boot-status --message 'Startup failed. Press a gamepad button to retry.' --log /run/console/startup-error.log; then
            echo 'console-start: explicit physical retry'
        else
            echo 'console-start: diagnostic renderer failed; see saved logs'
            persist_log
            exit 1
        fi
    fi
done
