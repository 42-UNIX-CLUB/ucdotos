#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Random expression campaign: fuzz.py programs built with clang and with cc must print
# the same thing. usage: sh bin/cc/tests/fuzz.sh [build-dir] [first-seed] [count]
set -u
B=${1:-build-cc}; S=${2:-1}; N=${3:-20}
T=$(dirname "$0")
cd "$(cd "$T/../../.." && pwd)"
O=$B/ccfuzz
mkdir -p "$O"
export ASAN_OPTIONS=detect_leaks=0
CLANG="clang --target=wasm32-unknown-unknown -O2 -ffreestanding -nostdlib -fvisibility=hidden -w -Ilibc/include"
LDF="--entry=_start -z stack-size=1048576 --stack-first"
bad=0
i=$S
while [ $i -lt $((S + N)) ]; do
  python3 "$T/fuzz.py" $i 150 > "$O/f.c"
  $CLANG -c -o "$O/f.ref.o" "$O/f.c" && wasm-ld $LDF -o "$O/f.ref" "$B/obj/crt0_2.o" "$O/f.ref.o" "$B/libc.a" || { echo "seed $i: clang failed"; i=$((i+1)); continue; }
  node "$T/run.mjs" "$O/f.ref" > "$O/f.exp"
  if ! $B/native/cc --sysroot="$B/sysroot" -c -o "$O/f.o" "$O/f.c" 2> "$O/f.err"; then echo "seed $i: cc failed"; head -3 "$O/f.err"; bad=$((bad+1)); cp "$O/f.c" "$O/fail-$i.c"; i=$((i+1)); continue; fi
  $B/native/ld -o "$O/f" --crt0="$B/sysroot/usr/lib/crt0_2.o" "$O/f.o" -L "$B/sysroot/usr/lib" -lc
  node "$T/run.mjs" "$O/f" > "$O/f.got" 2>&1
  if ! cmp -s "$O/f.exp" "$O/f.got"; then echo "seed $i: output differs"; diff "$O/f.exp" "$O/f.got" | head -4; bad=$((bad+1)); cp "$O/f.c" "$O/fail-$i.c"; fi
  i=$((i+1))
done
echo "fuzz: $N programs, $bad differ"
[ $bad -eq 0 ]
