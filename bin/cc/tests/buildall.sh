#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile (and link) programs of the tree with our cc and ld, natively.
# usage: sh bin/cc/tests/buildall.sh BUILD-DIR prog...    Results in BUILD-DIR/ccobj/PROG.
B=$1; shift
cd "$(cd "$(dirname "$0")/../../.." && pwd)"
export ASAN_OPTIONS=detect_leaks=0
S=$B/sysroot
mkdir -p "$B/ccobj"
fails=0
for p in "$@"; do
  if [ -d bin/$p ]; then srcs=$(echo bin/$p/*.c); extra=$(sed "s|\$(B)|$B|g" bin/$p/CFLAGS 2>/dev/null); libs=""; [ -f bin/$p/USE_GFX ] && libs="-lui -lgfx"
  else srcs=bin/$p.c; extra=""; libs=""; fi
  objs=""; ok=1
  for s in $srcs; do
    o=$B/ccobj/$(echo "$s" | tr / _).o
    cflags=""
    for w in $extra; do case $w in -Wl,*|*.a) ;; *) cflags="$cflags $w";; esac; done
    if ! $B/native/cc --sysroot="$S" -Ilibgfx -Ilibui $cflags -c "$s" -o "$o" 2>"$B/ccobj/err.txt"; then
      ok=0; echo "$p: cc failed on $s"; head -4 "$B/ccobj/err.txt"; break
    fi
    objs="$objs $o"
  done
  if [ $ok = 1 ]; then
    ldx=""
    for w in $extra; do case $w in -Wl,*) ldx="$ldx $(echo "${w#-Wl,}" | tr , ' ')";; *.a) ldx="$ldx $w";; esac; done
    if $B/native/ld -o "$B/ccobj/$p" --crt0="$S/usr/lib/crt0_2.o,$S/usr/lib/crt0_3.o" $objs -L "$S/usr/lib" $ldx $libs -lc 2>"$B/ccobj/err.txt"; then
      echo "$p: ok ($(wc -c < "$B/ccobj/$p") bytes)"
    else ok=0; echo "$p: ld failed"; head -4 "$B/ccobj/err.txt"; fi
  fi
  [ $ok = 1 ] || fails=$((fails+1))
done
[ $fails -eq 0 ]
