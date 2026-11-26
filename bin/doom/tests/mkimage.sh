#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build doom and a test image that contains one IWAD and (optionally) default
# arguments, without touching the shared rootfs/: the extra files go only into
# build-doom/root/, and build-doom/initrd.tar is re-created from it.
#   bin/doom/tests/mkimage.sh doom1.wad|freedoom1.wad|none [doom arguments...]
# With WM=1 in the environment, the window manager is built too and its config
# (in build-doom/root only) starts doom: boot that image with --init=/bin/wm.
set -e
cd "$(dirname "$0")/../../.."
B=build-doom
WAD=$1; shift
PROGS=doom
[ "$WM" = 1 ] && PROGS="doom wm"
make B=$B PROGS="$PROGS" >/dev/null
D=$B/root/usr/share/games/doom
mkdir -p $D
for f in doom1.wad freedoom1.wad doom.args; do [ -f $D/$f ] && rm $D/$f; done
[ "$WAD" = none ] || cp bin/doom/wads/$WAD $D/
[ $# -gt 0 ] && echo "$@" > $D/doom.args
C=$B/root/home/user/.config/wm
[ -f $C/config ] && rm $C/config
if [ "$WM" = 1 ]; then mkdir -p $C; printf 'gaps 0\nexec doom\n' > $C/config; fi
tar --format=ustar --owner=0 --group=0 -C $B/root -cf $B/initrd.tar .
echo "image: $WAD, args: $*"
