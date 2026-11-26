#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build doom and a test image with (optionally) default arguments, without touching the
# shared rootfs/: the extra files go only into build-doom/root/, and build-doom/initrd.tar
# is re-created from it. The game data is not put in the image: it reaches Doom the way it
# does in the hosted build, as a remote file fetched on first open (kernel/remote.c), from
# build-doom/files/ (tools/remote-files.sh; the WADs come from $WADS, default bin/doom/wads).
#   bin/doom/tests/mkimage.sh doom1.wad|freedoom1.wad|none [doom arguments...]
# freedoom1.wad adds "-iwad freedoom1.wad" to the arguments; none builds with no game data.
# With WM=1 in the environment, the window manager is built too and its config
# (in build-doom/root only) starts doom: boot that image with --init=/bin/wm.
set -e
cd "$(dirname "$0")/../../.."
B=${B:-build-doom}
WAD=$1; shift
W=${WADS:-bin/doom/wads}
[ "$WAD" = none ] && W=/nonexistent
PROGS=doom
[ "$WM" = 1 ] && PROGS="doom wm"
make B=$B PROGS="$PROGS" WADS="$W" >/dev/null
D=$B/root/usr/share/games/doom
mkdir -p $D
for f in doom1.wad freedoom1.wad doom.args; do [ -f $D/$f ] && rm $D/$f; done
ARGS="$*"
[ "$WAD" = freedoom1.wad ] && ARGS="-iwad freedoom1.wad $ARGS"
[ -n "$ARGS" ] && echo "$ARGS" > $D/doom.args
C=$B/root/home/user/.config/wm
[ -f $C/config ] && rm $C/config
if [ "$WM" = 1 ]; then mkdir -p $C; printf 'gaps 0\nexec doom\n' > $C/config; fi
tar --format=ustar --owner=0 --group=0 -C $B/root -cf $B/initrd.tar .
echo "image: $WAD (remote), args: $ARGS"
