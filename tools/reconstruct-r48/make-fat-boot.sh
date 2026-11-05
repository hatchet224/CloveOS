#!/bin/sh
# Generate a FAT32 boot sector from the user's installed dosfstools 4.2.
set -eu
if test "$#" -ne 1 || test -e "$1" || test -L "$1"; then exit 2; fi
if test -n "${CLOVE_BUILD_TMPDIR:-}"; then
    directory=$CLOVE_BUILD_TMPDIR
    mkdir -- "$directory"
else
    directory=$(mktemp -d)
fi
trap 'rm -rf -- "$directory"' EXIT HUP INT TERM
mkfs.fat -C -F 32 -i C0280002 "$directory/blank.fat" 65536
dd if="$directory/blank.fat" of="$1" bs=512 count=1
