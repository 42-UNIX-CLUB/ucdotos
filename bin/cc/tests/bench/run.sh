#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build bench.c with clang -O0, clang -O2 and cc, run each under Node, print the tables
# and the code sizes. usage: sh bin/cc/tests/bench/run.sh [build-dir]
B=${1:-build-cc}
D=$(dirname "$0")
cd "$(cd "$D/../../../.." && pwd)"
O=$B/ccbench
mkdir -p "$O"
export ASAN_OPTIONS=detect_leaks=0
CL="clang --target=wasm32-unknown-unknown -ffreestanding -nostdlib -fvisibility=hidden -w -Ilibc/include"
LDF="--entry=_start -z stack-size=1048576 --stack-first"
$CL -O0 -c -o "$O/b0.o" "$D/bench.c" && wasm-ld $LDF -o "$O/clang-O0" "$B/obj/crt0_2.o" "$O/b0.o" "$B/libc.a" 2>/dev/null ||
  wasm-ld $LDF -o "$O/clang-O0" "$B/obj/crt0_2.o" "$O/b0.o" "$B/libc.a"
$CL -O2 -c -o "$O/b2.o" "$D/bench.c" && wasm-ld $LDF -o "$O/clang-O2" "$B/obj/crt0_2.o" "$O/b2.o" "$B/libc.a"
$B/native/cc --sysroot="$B/sysroot" ${CCFLAGS:-} -c -o "$O/bc.o" "$D/bench.c" &&
  $B/native/ld -o "$O/cc" --crt0="$B/sysroot/usr/lib/crt0_2.o" "$O/bc.o" -L "$B/sysroot/usr/lib" -lc
for v in clang-O0 clang-O2 cc; do
  echo "== $v (object $(wc -c < "$O/$( [ $v = cc ] && echo bc || ([ $v = clang-O0 ] && echo b0 || echo b2) ).o") bytes, program $(wc -c < "$O/$v") bytes)"
  node "$D/../run.mjs" "$O/$v"
done
