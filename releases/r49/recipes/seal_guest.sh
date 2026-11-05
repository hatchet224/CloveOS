#!/bin/busybox sh
set -eu
/bin/busybox --install -s /bin
finish(){ result=$?;trap - EXIT;sync;echo R49_SEAL_VM_EXIT=$result;poweroff -f; }
trap finish EXIT
mount -t proc proc /proc;mount -t sysfs sys /sys;mount -t devtmpfs dev /dev
modprobe loop;mkdir /modloop /payload /export /actual
mount -t squashfs -o loop,ro /modloop.img /modloop;mount --bind /modloop/modules /lib/modules
modprobe squashfs;modprobe virtio_pci;modprobe virtio_blk;sleep 1
echo R49_SEAL_DEVICES_READY
tar -xf /dev/vdb -C /payload
unsquashfs -no-progress -processors 2 -d /tree /dev/vda >/dev/null
echo R49_BASELINE_EXTRACTED
test "$(sha256sum /tree/etc/console/BINARY_MANIFEST.json | cut -d' ' -f1)" = "$(cat /payload/expected-base-manifest.sha256)"
(cd /tree;find . -type f -print0|xargs -0 sha256sum|sort) >/export/baseline-files.sha256
rm /payload/expected-base-manifest.sha256
if [ -f /payload/removed-files.txt ]; then
 test ! -L /tree/system/share/console/commander
 test ! -L /tree/system/share/console/themes/Silky
 test ! -L /tree/system/share/console/themes/TMNT
 while IFS= read -r file; do
  case "$file" in system/share/console/commander/*|system/share/console/themes/Silky/*|system/share/console/themes/TMNT/*) ;; *) exit 71 ;; esac
  rm -f "/tree/$file"
 done </payload/removed-files.txt
 rm /payload/removed-files.txt
fi
cp -r /payload/. /tree/
test "$(sha256sum /tree/system/bin/retroarch|cut -d' ' -f1)" = "$(cat /tree/etc/console/retroarch-compatible.sha256)"
test "$(stat -c %a /tree/etc/console/retroarch-compatible.sha256)" = 444
test "$(cat /tree/etc/console/BUILD_ID)" = 20261105-cloveos-r49
for file in console-supervisor console-launcher GameSwitcher console-file-explorer retroarch console-onion-peer console-onion-easy-netplay console-standard-netplay console-startup-app;do test -x /tree/system/bin/$file;done
find /tree -xdev -type f -exec touch -t 202611050000 {} +
echo R49_COMPRESSING_ROOTFS
mksquashfs /tree /export/rootfs.sqfs -comp xz -b 65536 -noappend -all-root -mkfs-time 1793836800 -processors 2 -no-progress >/export/mksquashfs.txt
echo R49_COMPRESSION_FINISHED
mount -t squashfs -o loop,ro /export/rootfs.sqfs /actual
(cd /actual;find . -type f -print0|xargs -0 sha256sum|sort) >/export/root-files.sha256
tar -cf /dev/vdd -C /actual .
tar -cf /dev/vdc -C /export .
echo R49_SEALED_ROOTFS_MOUNT_AND_INVENTORY_PASS
