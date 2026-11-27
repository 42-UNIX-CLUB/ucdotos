#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Decompression throughput inside UC.OS under Node, with libarc/tests/arcbench.c.
#   sh libarc/tests/os-bench.sh BUILD FILE...
# BUILD is a build directory made with `make B=BUILD` (it needs libc.a,
# libarc.a, crt0_2.o, kernel.wasm and initrd.tar; libarc.a is built when any
# program with USE_ARC is in PROGS). FILE... are compressed files to time.
# The script makes BUILD-bench/ holding the same kernel and an initrd with
# /bin/arcbench and the files under /bench added; nothing in BUILD changes.
set -e
B=$1; shift
top=$(cd "$(dirname "$0")/../.." && pwd)
cd "$top"
O=$B-bench
mkdir -p $O/stage/bin $O/stage/bench
clang --target=wasm32-unknown-unknown -O2 -ffreestanding -nostdlib -fvisibility=hidden -Ilibc/include -Ilibarc \
  -Wl,--entry=_start -Wl,-z,stack-size=1048576 -Wl,--stack-first -Wl,--fatal-warnings \
  -o $O/stage/bin/arcbench $B/obj/crt0_2.o libarc/tests/arcbench.c $B/libarc.a $B/libc.a
names=""
for f in "$@"; do cp "$f" $O/stage/bench/; names="$names $(basename "$f")"; done
cp $B/kernel.wasm $O/
cp $B/initrd.tar $O/initrd.tar
tar --format=ustar --owner=0 --group=0 -C $O/stage -rf $O/initrd.tar ./bin/arcbench ./bench
script=""
for n in $names; do script="$script arcbench /bench/$n 5;"; done
node tools/run.mjs --build=$O --init=/bin/sh --input="$script exit\n"
