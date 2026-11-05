#!/bin/sh
# Run inside Linux as root so the rootfs device node is recreated from the manifest.
set -eu
umask 0
if test "$#" -ne 2 || test "$(id -u)" -ne 0; then
    echo 'Usage as Linux root: seal-rootfs.sh STAGED.tar NEW.sqfs' >&2
    exit 2
fi
if test -e "$2" || test -L "$2"; then exit 2; fi
if test -n "${CLOVE_BUILD_TMPDIR:-}"; then
    directory=$CLOVE_BUILD_TMPDIR
    mkdir -- "$directory"
else
    directory=$(mktemp -d)
fi
trap 'rm -rf -- "$directory"' EXIT HUP INT TERM
tar -xpf "$1" -C "$directory"
mksquashfs "$directory" "$2" -comp xz -b 65536 -noappend -all-root \
    -mkfs-time 1793836800 -processors 8 -no-progress
