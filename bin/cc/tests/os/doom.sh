#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Doom built by clang and by cc, both running "-timedemo demo1": the number of game
# tics is deterministic, so equal counts mean the demo replayed identically (a
# miscompiled game desynchronises); realtics show speed (noise on a loaded machine).
# usage: sh bin/cc/tests/os/doom.sh BUILD-DIR WAD-PATH
set -u
B=$1; WAD=$2
cd "$(cd "$(dirname "$0")/../../../.." && pwd)"
nice -n 19 make -s B="$B" PROGS="doom" >/dev/null || exit 1
D=$B/root/usr/share/games/doom
mkdir -p "$D"
cp -f "$WAD" "$D/"
echo "-timedemo demo1 -stats" > "$D/doom.args"
run() {
  tar --format=ustar --owner=0 --group=0 -C "$B/root" -cf "$B/initrd.tar" .
  nice -n 19 timeout 300 node tools/shot.mjs --build="$B" --init=/bin/doom --script=bin/doom/tests/timedemo.mjs --quiet 2>&1 | grep "\[test\]"
}
cp -f "$B/bin/doom" "$B/doom.clang"
cp -f "$B/doom.clang" "$B/root/bin/doom"
echo "clang: $(run)"
[ -f "$B/sysroot/usr/lib/libc.a" ] || nice -n 19 sh bin/cc/tests/build.sh "$B" >/dev/null 2>&1
nice -n 19 sh bin/cc/tests/buildall.sh "$B" doom >/dev/null || { echo "doom: cc build failed"; exit 1; }
cp -f "$B/ccobj/doom" "$B/root/bin/doom"
echo "cc:    $(run)"
