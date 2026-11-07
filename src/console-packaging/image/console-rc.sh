#!/bin/sh
# Managed single-card Moss bootstrap. The stock S95done service backgrounds us.
set -eu
PATH=/system/bin:/usr/magicx/bin:/usr/sbin:/usr/bin:/sbin:/bin
LD_LIBRARY_PATH=/system/lib:/usr/magicx/lib:/lib:/usr/lib
export PATH LD_LIBRARY_PATH
. /system/init/console-common.sh
boot_mark() {
    read -r boot_uptime boot_idle < /proc/uptime || return 0
    echo "boot timing: stage=$1 uptime_seconds=$boot_uptime"
}
if [ -f /etc/console/BUILD_ID ]; then
    printf 'console-rc: firmware build='
    cat /etc/console/BUILD_ID
fi
CONF=/etc/console/cards.conf
CTL=/system/bin/console-ctl
die() {
    echo "console-rc: $*" >&2
    # Freeze the causal tail before the diagnostic renderer appends SDL output.
    if [ -d /run/console ] && [ ! -L /run/console/startup-error.log ]; then
        tail -c 8192 /run/console/startup.log > /run/console/startup-error.log || :
    fi
    exit 1
}
console_pin_configuration || die 'cannot pin immutable startup configuration'
console_require_root_config "$CONF" || die 'untrusted card configuration'
. "$CONF"
runtime() {
    if ! console_is_mounted /run; then
        [ -d /run ] && [ -z "$(ls -A /run)" ] || die 'occupied runtime mount'
        mount -t tmpfs -o mode=0755,size=128m tmpfs /run
    fi
    [ ! -L /run/console ] || die 'runtime is a link'
    mkdir -p /run/console
    chmod 0700 /run/console
    "$CTL" runtime-check || die 'runtime protection failed'
}
mount_games() {
    device=$(/system/bin/console-board-probe --find-volume "$PRIMARY_UUID") || die 'primary volume is absent or ambiguous'
    mkdir -p /dev/disk/by-uuid
    alias=/dev/disk/by-uuid/$PRIMARY_UUID
    if [ -e "$alias" ] || [ -L "$alias" ]; then
        [ "$(readlink -f "$alias")" = "$device" ] || die 'UUID alias mismatch'
    else
        ln -s "$device" "$alias"
    fi
    [ ! -L "$PRIMARY_MOUNT" ] || die 'primary mount is a link'
    mkdir -p "$PRIMARY_MOUNT"
    if ! console_is_mounted "$PRIMARY_MOUNT"; then
        mount -t vfat -o rw,nosuid,nodev,noexec,utf8,fmask=0077,dmask=0077 "$device" "$PRIMARY_MOUNT"
        : > /run/console/primary.owned
    fi
    "$CTL" verify-mount "$PRIMARY_MOUNT" "$device" "$PRIMARY_UUID" vfat rw || die 'primary mount mismatch'
    [ "$(readlink /roms)" = /mnt/SDCARD/Roms ] || die 'ROM path mismatch'
    [ -d "$PRIMARY_MOUNT/Roms" ] && [ -d "$PRIMARY_MOUNT/BIOS" ] || die 'data tree missing'
}
stop_console() {
    [ -e /run/console/started ] || return 0
    if ! "$CTL" check-stopped; then
        "$CTL" --socket /run/console/control.sock stop || return 1
    fi
    "$CTL" check-stopped || return 1
    if [ -e /run/console/primary.owned ] && console_is_mounted "$PRIMARY_MOUNT"; then
        umount "$PRIMARY_MOUNT" || return 1
    fi
}
case "${1:-}" in
    mount-games) runtime; mount_games;;
    stop) stop_console;;
    start)
        boot_mark runtime-start
        runtime
        mkdir /run/console/bootstrap.lock || die 'bootstrap already attempted'
        bootstrap_child=
        diagnostic_child=
        log_child=
        stop_progress() {
            for child in $diagnostic_child $log_child; do
                kill -TERM "$child" 2>/dev/null || :
                wait "$child" 2>/dev/null || :
            done
            diagnostic_child=
            log_child=
        }
        save_progress() {
            "$CTL" verify-mount "$PRIMARY_MOUNT" "$device" "$PRIMARY_UUID" vfat rw >/dev/null 2>&1 || return 0
            logdir=$PRIMARY_MOUNT/.tmp_update/logs
            [ ! -L "$PRIMARY_MOUNT/.tmp_update" ] && [ ! -L "$logdir" ] || return 0
            mkdir -p "$logdir" || return 0
            [ ! -L "$logdir/boot-progress.log" ] && [ ! -L "$logdir/boot-progress.log.new" ] || return 0
            if tail -c 131072 /run/console/startup.log > "$logdir/boot-progress.log.new"; then
                mv "$logdir/boot-progress.log.new" "$logdir/boot-progress.log" || :
                sync
            fi
        }
        failed_start() {
            bootstrap_result=$?
            trap - EXIT
            stop_progress
            if [ "$bootstrap_result" -ne 0 ]; then
                if [ -n "$bootstrap_child" ] && kill -0 "$bootstrap_child" 2>/dev/null; then
                    # Normal TERM and actual wait, never a forced writer kill.
                    kill -TERM "$bootstrap_child" 2>/dev/null || :
                    wait "$bootstrap_child" || :
                fi
                rm -f /run/console/started /run/console/supervisor.pid
                rmdir /run/console/bootstrap.lock || :
            fi
            exit "$bootstrap_result"
        }
        trap failed_start EXIT
        asset=/system
        [ ! -d /rom/system ] || asset=/rom/system
        if [ ! -e /run/console/system.bound ]; then
            mount -o bind "$asset" /system
            mount -o remount,bind,ro /system
            : > /run/console/system.bound
        fi
        mount_games
        boot_mark games-mounted
        for module in $INPUT_MODULES; do
            case "$module" in ''|-*|*[!A-Za-z0-9_-]*) die 'invalid module';; esac
            modprobe "$module" || die 'input module failed'
        done
        mkdir -p /run/console/config
        chmod 0700 /run/console/config
        rm -f /run/console/config/base-profile.json /run/console/config/ui.json /run/console/config/supervisor.json
        for profile in /run/console/config/profiles/*.json; do
            [ -e "$profile" ] || continue
            [ -f "$profile" ] && [ ! -L "$profile" ] || die 'unsafe runtime profile'
            rm -f "$profile"
        done
        attempts=0
        while ! /system/bin/console-board-probe --prepare-runtime; do
            attempts=$((attempts + 1))
            [ "$attempts" -lt 20 ] || die 'physical input discovery failed'
            # Retry only after removing known private tmpfs outputs, never card data.
            rm -f /run/console/config/base-profile.json /run/console/config/ui.json /run/console/config/supervisor.json
            for profile in /run/console/config/profiles/*.json; do
                [ -e "$profile" ] || continue
                [ -f "$profile" ] && [ ! -L "$profile" ] || die 'unsafe runtime profile'
                rm -f "$profile"
            done
            sleep 0.5
        done
        boot_mark input-ready
        # Stock display/audio initialization has completed. Restore only
        # verified user levels and the bounded Zero28 audio baseline. An
        # absent/corrupt record must not block boot or restore old DAC gain.
        /system/bin/console-level restore "$PRIMARY_MOUNT" "$PRIMARY_UUID" ||
            echo 'console-rc: levels/audio initialization incomplete; see console-level diagnostics' >&2
        boot_mark levels-restored
        echo preparing-catalog > /run/console/boot.phase
        echo 'console-rc: preparing catalog; progress displayed before launcher'
        /system/bin/console-boot-status --splash --message 'Preparing games. Please wait...' &
        diagnostic_child=$!
        # Keep evidence even if power is lost before the catalog returns.
        (
            pause_child=
            trap 'if [ -n "$pause_child" ]; then kill -TERM "$pause_child" 2>/dev/null || :; wait "$pause_child" 2>/dev/null || :; fi; exit 0' TERM INT
            while :; do
                save_progress
                sleep 5 &
                pause_child=$!
                wait "$pause_child" || :
                pause_child=
            done
        ) &
        log_child=$!
        catalog_result=0
        /system/bin/console-catalog || catalog_result=$?
        save_progress
        stop_progress
        if [ "$catalog_result" -eq 2 ]; then
            # Discovery can fail on bounded library limits or invalid directory
            # entries. Preserve all card files; allow Tools/transfer/settings
            # from a separately validated empty RAM catalog instead of trapping
            # startup in a retry loop. An unsafe/unavailable volume still fails.
            tail -c 8192 /run/console/startup.log > /run/console/startup-error.log
            /system/bin/console-catalog --empty-runtime || die 'empty library recovery failed validation'
            echo 'console-rc: library scan incomplete; starting Home with an empty runtime library; card content retained' >&2
            save_progress
            /system/bin/console-boot-status --message 'Game scan failed. Files are retained. Continue to Home; use Refresh ROM list to retry.' --log /run/console/startup-error.log || die 'library warning renderer failed'
        elif [ "$catalog_result" -ne 0 ]; then
            die 'qualified game catalog could not be prepared'
        fi
        boot_mark catalog-ready
        echo catalog-ready > /run/console/boot.phase
        /system/bin/console-timezone || echo 'console-clock: timezone restore failed' >&2
        /system/init/console-network-init.sh &
        : > /run/console/started
        /system/bin/console-supervisor --config /run/console/config/supervisor.json >/run/console/supervisor.log 2>&1 &
        bootstrap_child=$!
        echo "$bootstrap_child" > /run/console/supervisor.pid
        sleep 1
        if ! kill -0 "$bootstrap_child" 2>/dev/null; then
            wait "$bootstrap_child" || :
            bootstrap_child=
            die 'supervisor exited during startup; see supervisor.log'
        fi
        trap - EXIT
        ;;
    *) die 'usage: console-rc start|mount-games|stop';;
esac
