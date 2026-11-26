#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Put a small Debian root (dash, bash, coreutils, busybox and the libraries they need) at
# /debian in a UC.OS build's image, plus the engine's test scripts under /lx. UC.OS has no
# symbolic links yet, so links are copied as files (cp -L): /lib/ld-linux-riscv64-lp64d.so.1
# and /bin/sh become real files. About 5 MB.
#   LX_SCRATCH=/dir sh bin/lx/tests/mkimage-debian.sh build-NAME
set -eu
B=$1
R=$LX_SCRATCH/root
HERE=$(cd "$(dirname "$0")" && pwd)
L=usr/lib/riscv64-linux-gnu
set --
for f in usr/bin/dash usr/bin/bash usr/bin/ls usr/bin/cat usr/bin/echo usr/bin/sort usr/bin/busybox usr/bin/dpkg usr/bin/perl $L/libmd.so.0 $L/libm.so.6 $L/libcrypt.so.1 \
         $L/ld-linux-riscv64-lp64d.so.1 $L/libc.so.6 $L/libtinfo.so.6 $L/libselinux.so.1 $L/libpcre2-8.so.0 \
         $L/libacl.so.1 $L/libattr.so.1 $L/libcap.so.2; do
  [ -e "$R/$f" ] && set -- "$@" "$R/$f:/debian/$f"
done
set -- "$@" "$R/$L/ld-linux-riscv64-lp64d.so.1:/debian/lib/ld-linux-riscv64-lp64d.so.1" \
  "$R/usr/bin/dash:/debian/bin/sh" "$HERE/guest/m2.sh:/debian/lx/m2.sh" "$HERE/guest/m34.sh:/debian/lx/m34.sh"
mkdir -p "$B/root/debian/tmp" "$B/root/debian/etc"
sh "$HERE/mkimage.sh" "$B" "$@"
