#!/bin/busybox sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
cp /payload/emulator-fixture /fixture/system/bin/retroarch
chmod 755 /fixture/system/bin/retroarch
proof=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
registry=/system/share/console/onion-4.3.1-1/cores.json
config=/etc/console/retroarch.cfg
core=/system/bin/gambatte_libretro.so
root=/fixture/run/console
touch "$root/fail-control-merge"
failure=0
chroot /fixture /system/bin/console-onion-easy-netplay host "$proof" "$config" "$core" /mnt/SDCARD/Roms/GB/player1.gb link || failure=$?
test "$failure" = 29
test ! -e "$root/easy-netplay"
test -e "$root/station"
rm "$root/fail-control-merge"
echo CONTROL_CONFIG_MERGE_FAILURE_REMOVES_PENDING_AND_RETRY_SESSION_PASS
ifconfig lo:3 192.168.100.177 netmask 255.255.255.0 up
for scenario in host join host-fallback host-stock-empty host-term join-term;do
 role=$scenario;case "$role" in host-fallback|host-stock-empty|host-term)role=host;;join-term)role=join;;esac
 peerrole=join;player=1;other=2;peerip=192.168.100.101;localip=192.168.100.100
 if [ "$role" = join ];then peerrole=host;player=2;other=1;peerip=192.168.100.100;localip=192.168.100.101;fi
 if [ "$scenario" = host-fallback ];then peerip=192.168.100.177;fi
 printf '%s 0x1 0x2 02:11:22:33:44:55 * wlan0\n' "$peerip" >/fixture/arp-fixture
 folder=/run/wrapper-peer-$scenario
 mkdir /fixture$folder
 chroot /fixture /native/peer link-init "$registry" "/mnt/SDCARD/Roms/GB/player$other.gb" "$folder" "$peerrole"
 sed -i 's|/mnt/SDCARD/Roms/|/mnt/SDCARD/Emu/GBC/../../Roms/|g' "/fixture$folder/retroarch.cookie"
 if [ "$scenario" = host-stock-empty ];then
  sed -i '/^\[corechksum\]:/d;s|^\[core\]:.*|[core]: |' "/fixture$folder/retroarch.cookie"
 fi
 chroot /fixture /native/peer serve "$peerip:21" "$folder/export.json" & ftp=$!
 chroot /fixture /system/bin/console-onion-easy-netplay "$role" "$proof" "$config" "$core" "/mnt/SDCARD/Roms/GB/player$player.gb" link /system/share/console/themes/Silky 1 'Synthetic Link Game' & wrapper=$!
 chroot /fixture /native/peer link-pair "$localip:21" "$folder" & pairing=$!
 wait "$pairing"
 chroot /fixture /native/peer link-ready "$folder"
 for attempt in $(seq 1 80);do grep -q 'joined as player 2' "$root/easy-netplay/retroarch.log" 2>/dev/null && break;kill -0 "$wrapper";sleep 0.25;done
 grep -q 'joined as player 2' "$root/easy-netplay/retroarch.log"
 test ! -e "$root/easy-netplay/audio-ready"
 touch "$root/release-audio"
 for attempt in $(seq 1 80);do test -e "$root/easy-netplay/audio-ready"&&break;kill -0 "$wrapper";sleep 0.1;done
 test "$(cat "$root/easy-netplay/audio-ready")" = ready
 case "$scenario" in
 *-term)
  kill -TERM "$wrapper"
  for attempt in $(seq 1 100);do
   [ "$(cat "$root/easy-netplay.phase")" = returning-saves ] && break
   kill -0 "$wrapper";sleep 0.1
  done
  test "$(cat "$root/easy-netplay.phase")" = returning-saves
  # TERM while the actual helper waits for the peer must not abort save return.
  kill -TERM "$wrapper";sleep 0.25;kill -0 "$wrapper"
  ;;
 *) printf 'exit\n' >"$root/easy-netplay/exit-request" ;;
 esac
 chroot /fixture /native/peer link-finish "$folder" & finish=$!
 wait "$wrapper";wait "$finish"
 case "$scenario" in *-term)echo POWER_TERM_DURING_GAME_AND_SAVE_RETURN_REAL_HELPER_FLUSH_REAP_RESTORE_PASS=$role;;esac
 test "$(cat "$root/fixture-normal-flush")" = 'ordinary signal exit before save return'
 grep -q "notice-start phase=returning-saves role=$role mode=link" "$root/fixture-events"
 grep -q "notice-start phase=restoring-network role=$role mode=link" "$root/fixture-events" || grep -q "notice-start phase=returning-saves role=$role mode=link" "$root/fixture-events"
 kill -TERM "$ftp";wait "$ftp"||:
 rm "$root/release-audio" "$root/fixture-normal-flush"
 echo PRODUCTION_WRAPPER_AUDIO_READINESS_NORMAL_EMULATOR_EXIT_BEFORE_SAVE_RETURN_PASS=$role
 if [ "$scenario" = host-stock-empty ];then
  grep -q 'stock pre-selection cookie; actual pinned TGB Dual verified read-only' "$root/easy-netplay.log"
  echo STOCK_UNSET_CORE_COOKIE_PRODUCTION_HOST_PAIR_FRONTEND_EXIT_AND_SAVE_RETURN_PASS
 fi
 test ! -e "$root/easy-netplay";test -e "$root/station"
 test "$(cat "$root/wpa.conf")" = 'saved network data'
 echo ONION_POKEMON_PRODUCTION_WRAPPER_PAIR_SAVE_EXIT_WIFI_RESTORE_PASS=$role
 if [ "$scenario" = host-fallback ];then
  grep -q 'associated_peer_address=192.168.100.177' "$root/easy-netplay.log"
  echo ONION_HOST_RANDOM_FALLBACK_ADDRESS_REAL_HELPER_WRAPPER_PASS
 fi
done
printf '192.168.100.101 0x1 0x2 02:11:22:33:44:55 * wlan0\n' >/fixture/arp-fixture
# Cancel while waiting for the other device: no stale session or changed save.
before=$(sha256sum /fixture/mnt/SDCARD/Saves/LinkCable/*.link)
chroot /fixture /system/bin/console-onion-easy-netplay host "$proof" "$config" "$core" /mnt/SDCARD/Roms/GB/player1.gb link & wrapper=$!
for attempt in $(seq 1 40);do netstat -lnt|grep -q '192.168.100.100:21 '&&break;kill -0 "$wrapper";sleep 0.25;done
kill -TERM "$wrapper";wait "$wrapper"
test ! -e "$root/easy-netplay";test -e "$root/station"
test "$before" = "$(sha256sum /fixture/mnt/SDCARD/Saves/LinkCable/*.link)"
echo ONION_POKEMON_CANCEL_DURING_PAIRING_NO_SAVE_MUTATION_PASS
echo ONION_PRODUCTION_WRAPPER_PROGRESS_REOPENED_AFTER_EMULATOR_BEFORE_SAVE_RETURN_PASS

cp /fixture/system/bin/console-onion-peer /fixture/system/bin/console-onion-peer-original
for failure in 21 28;do
 for role in host join;do
  cat >/fixture/system/bin/console-onion-peer <<EOF
#!/bin/sh
if [ "\$1" = link-pair ];then echo 'fixture exact pairing error' >&2;exit $failure;fi
exec /system/bin/console-onion-peer-original "\$@"
EOF
  chmod 755 /fixture/system/bin/console-onion-peer
  wanted=26;[ "$failure" != 28 ] || wanted=28
  status=0
  chroot /fixture /system/bin/console-onion-easy-netplay "$role" "$proof" "$config" "$core" /mnt/SDCARD/Roms/GB/player1.gb link || status=$?
  test "$status" = "$wanted"
  grep -q 'fixture exact pairing error' "$root/easy-netplay.log"
  grep -q "helper=link-pair exit=$failure" "$root/easy-netplay.log"
  test "$(cat "$root/easy-netplay.phase")" = pairing-cartridges-and-saves
  test "$(cat "$root/easy-netplay.failed-stage")" = pairing-cartridges-and-saves
  test ! -e "$root/easy-netplay";test -e "$root/station"
  test "$(cat "$root/wpa.conf")" = 'saved network data'
  test "$before" = "$(sha256sum /fixture/mnt/SDCARD/Saves/LinkCable/*.link)"
  echo R55_PAIR_FAILURE_TIMEOUT_CLEANUP_LOG_SAVE_PRESERVATION_PASS=$role:$failure
 done
done
mv /fixture/system/bin/console-onion-peer-original /fixture/system/bin/console-onion-peer
touch "$root/no-peer"
status=0
chroot /fixture /system/bin/console-onion-easy-netplay host "$proof" "$config" "$core" /mnt/SDCARD/Roms/GB/player1.gb link || status=$?
test "$status" = 28
test "$(cat "$root/easy-netplay.phase")" = finding-associated-peer-address
test "$(cat "$root/easy-netplay.failed-stage")" = finding-associated-peer-address
test ! -e "$root/easy-netplay";test -e "$root/station"
test "$before" = "$(sha256sum /fixture/mnt/SDCARD/Saves/LinkCable/*.link)"
test "$(cat "$root/wpa.conf")" = 'saved network data'
rm "$root/no-peer"
echo ONION_HOST_NO_ASSOCIATED_PEER_BOUNDED_TIMEOUT_CLEANUP_SAVE_PRESERVATION_PASS
