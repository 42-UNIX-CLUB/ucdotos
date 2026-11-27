#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Signature verification time inside UC.OS under Node, with libsig/tests/sigtool.c.
#   sh libsig/tests/os-bench.sh BUILD KEYRING INRELEASE...
# BUILD is a build directory made with `make B=BUILD` with a USE_PKG program in
# PROGS (so libsig.a, libarc.a, libc.a, crt0_2.o, kernel.wasm and initrd.tar
# exist). The script makes BUILD-sigbench/ with the same kernel and an initrd
# with /bin/sigtool and the files under /bench added; nothing in BUILD changes.
# Inside: "sigtool prim 20" (one RSA-4096 and one Ed25519 verification, batches
# of 10, first and best batch), then "sigtool bench KEYRING FILE 10" for each
# file (a whole cleartext check: parse, hash, every signature).
set -e
B=$1; K=$2; shift 2
top=$(cd "$(dirname "$0")/../.." && pwd)
cd "$top"
O=$B-sigbench
mkdir -p $O/stage/bin $O/stage/bench
TMPDIR=$top/$O clang --target=wasm32-unknown-unknown -O2 -ffreestanding -nostdlib -fvisibility=hidden -Ilibc/include -Ilibarc \
  -Wl,--entry=_start -Wl,-z,stack-size=1048576 -Wl,--stack-first -Wl,--fatal-warnings \
  -o $O/stage/bin/sigtool $B/obj/crt0_2.o libsig/tests/sigtool.c $B/libsig.a $B/libarc.a $B/libc.a
command cp "$K" $O/stage/bench/keyring
script="sigtool prim 20;"
for f in "$@"; do n=$(basename "$(dirname "$f")")-$(basename "$f"); command cp "$f" "$O/stage/bench/$n"; script="$script sigtool bench /bench/keyring /bench/$n 10;"; done
command cp $B/kernel.wasm $O/
command cp $B/initrd.tar $O/initrd.tar
tar --format=ustar --owner=0 --group=0 -C $O/stage -rf $O/initrd.tar ./bin/sigtool ./bench
timeout 60 node tools/run.mjs --build=$O --init=/bin/sh --input="$script exit\n"
