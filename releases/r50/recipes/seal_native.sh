#!/bin/sh
# Local candidate packaging only. One private temporary RAM mount; no devices,
# services, system configuration or power control.
set -eu
scratch=$1 baseline=$2 overlay=$3 output=$4 tools=$5
case "$scratch" in /tmp/cloveos-r50-seal-*) ;; *) exit 70;; esac
test ! -e "$scratch"
mkdir "$scratch"
mounted=0
cleanup(){
 if [ "$mounted" = 1 ]; then umount "$scratch" || return 1; fi
 rmdir "$scratch"
}
trap cleanup EXIT
mount -t tmpfs -o size=3G,mode=0700 tmpfs "$scratch"
mounted=1
loader="$tools/lib/ld-musl-x86_64.so.1"
libraries="$tools/lib:$tools/usr/lib"
unpack(){ "$loader" --library-path "$libraries" "$tools/usr/bin/unsquashfs" "$@"; }
mkdir "$scratch/payload"
tar -xf "$overlay" -C "$scratch/payload"
unpack -no-progress -processors 2 -d "$scratch/tree" "$baseline" >/dev/null
tree="$scratch/tree" payload="$scratch/payload"
test "$(sha256sum "$tree/etc/console/BINARY_MANIFEST.json" | cut -d' ' -f1)" = "$(cat "$payload/expected-base-manifest.sha256")"
(cd "$tree"; find . -type f -print0 | xargs -0 sha256sum | sort) >"$output/baseline-files.sha256"
rm "$payload/expected-base-manifest.sha256"
test ! -L "$tree/system/share/console/commander"
test ! -L "$tree/system/share/console/themes/Silky"
test ! -L "$tree/system/share/console/themes/TMNT"
while IFS= read -r file; do
 case "$file" in system/share/console/commander/*|system/share/console/themes/Silky/*|system/share/console/themes/TMNT/*) ;; *) exit 71;; esac
 case "$file" in *..*) exit 72;; esac
 rm -f -- "$tree/$file"
done <"$payload/removed-files.txt"
rm "$payload/removed-files.txt"
cp -a "$payload/." "$tree/"
test "$(sha256sum "$tree/system/bin/retroarch" | cut -d' ' -f1)" = "$(cat "$tree/etc/console/retroarch-compatible.sha256")"
test "$(stat -c %a "$tree/etc/console/retroarch-compatible.sha256")" = 444
test "$(cat "$tree/etc/console/BUILD_ID")" = 20261105-cloveos-r50
for file in console-supervisor console-launcher GameSwitcher console-file-explorer retroarch console-onion-peer console-onion-easy-netplay console-standard-netplay console-startup-app; do test -x "$tree/system/bin/$file"; done
find "$tree" -xdev -type f -exec touch -t 202611050000 {} +
rm -f -- "$output/rootfs.sqfs"
"$loader" --library-path "$libraries" "$tools/usr/bin/mksquashfs" "$tree" "$output/rootfs.sqfs" -comp xz -b 65536 -noappend -all-root -mkfs-time 1793836800 -processors 2 -no-progress >"$output/mksquashfs.txt"
unpack -no-progress -processors 2 -d "$scratch/actual" "$output/rootfs.sqfs" >/dev/null
(cd "$scratch/actual"; find . -type f -print0 | xargs -0 sha256sum | sort) >"$output/root-files.sha256"
tar -cf "$output/rootfs-audit.tar" -C "$scratch/actual" .
echo R50_NATIVE_SEAL_EXIT=0
