#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Add guest test files to a UC.OS build's image (B/initrd.tar), the same way the Makefile
# builds it: files are copied under B/root/ and the archive is rebuilt from B/root.
# Everything in the image lives in RAM, so keep it to a few MB.
#
#   sh bin/lx/tests/mkimage.sh build-NAME SRC:DEST [SRC:DEST ...]
#   e.g.  sh bin/lx/tests/mkimage.sh build-lx /scratch/hello:/lx/hello /scratch/root/usr/bin/busybox:/lx/bin/busybox
#
# A DEST ending in / copies SRC (a directory) recursively into it.
set -eu
B=$1; shift
[ -d "$B/root" ] || { echo "run make B=$B first" >&2; exit 1; }
for m in "$@"; do
  src=${m%%:*}; dst=${m#*:}
  case "$dst" in
    */) mkdir -p "$B/root$dst"; cp -a "$src/." "$B/root$dst" ;;
    *) mkdir -p "$B/root$(dirname "$dst")"; cp -L "$src" "$B/root$dst" ;;
  esac
done
tar --format=ustar --owner=0 --group=0 -C "$B/root" -cf "$B/initrd.tar" .
echo "image: $B/initrd.tar ($(du -h "$B/initrd.tar" | cut -f1))"
