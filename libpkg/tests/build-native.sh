#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build libpkg, libarc, libsig and the package tools for the host, with
# AddressSanitizer and UBSan. Usage: sh libpkg/tests/build-native.sh [OUTDIR]
# OUTDIR defaults to build-pkgnative/ at the top of the tree (git-ignored).
set -e
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-pkgnative}
mkdir -p "$out/obj" "$out/bin"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer}
objs=
for f in "$top"/libpkg/*.c "$top"/libarc/*.c "$top"/libsig/*.c; do
  o="$out/obj/$(basename "$(dirname "$f")")-$(basename "$f" .c).o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "$top/libpkg/pkg.h" -nt "$o" ] || [ "$top/libarc/arc.h" -nt "$o" ] || [ "$top/libarc/arc_int.h" -nt "$o" ] || [ "$top/libsig/sig.h" -nt "$o" ] || [ "$top/libsig/sig_int.h" -nt "$o" ]; then
    $CC $CFLAGS -c -o "$o" "$f"
  fi
  objs="$objs $o"
done
for p in dpkg dpkg-deb dpkg-query apt apt-get apt-cache apt-keyring; do
  $CC $CFLAGS -o "$out/bin/$p" "$top/bin/$p/main.c" $objs
done
if [ -f "$top/libpkg/tests/unit.c" ]; then
  $CC $CFLAGS -o "$out/bin/unit" "$top/libpkg/tests/unit.c" $objs
fi
echo "built in $out/bin"
