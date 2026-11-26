#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Test suite of the C compiler. Every program in c/ is built twice, with clang (the
# reference) and with our cc, against the same UC.OS libc; both run under run.mjs and
# their output and exit status must match. Programs in err/ must fail to compile with
# the message given in their "// ERROR:" line.
# usage: sh bin/cc/tests/run.sh [build-dir] [linker]     (linker: ld (ours, default) or wasm-ld)
set -u
B=${1:-build-cc}
LINKER=${2:-ld}
T=$(dirname "$0")
ROOT=$(cd "$T/../../.." && pwd)
cd "$ROOT"
OUT=$B/cctest
mkdir -p "$OUT"
CC=$B/native/cc
SYSROOT=$B/sysroot
[ -x "$CC" ] && [ -f "$SYSROOT/usr/lib/libc.a" ] || { echo "run: build first: sh bin/cc/tests/build.sh $B"; exit 2; }
export ASAN_OPTIONS=detect_leaks=0
CLANG="clang --target=wasm32-unknown-unknown -O2 -ffreestanding -nostdlib -fvisibility=hidden -w -Ilibc/include"
LDF="--entry=_start -z stack-size=1048576 --stack-first --fatal-warnings"
pass=0; fail=0
link() { # out objs...
  o=$1; shift
  if [ "$LINKER" = wasm-ld ]; then
    wasm-ld $LDF -o "$o" "$B/obj/crt0_2.o" "$@" "$B/libc.a" 2>"$OUT/link.err" ||
      wasm-ld $LDF -o "$o" "$B/obj/crt0_3.o" "$@" "$B/libc.a" 2>"$OUT/link.err"
  else
    $B/native/ld -o "$o" --crt0="$SYSROOT/usr/lib/crt0_2.o,$SYSROOT/usr/lib/crt0_3.o" "$@" -L "$SYSROOT/usr/lib" -lc 2>"$OUT/link.err"
  fi
}
for f in $T/c/*.c; do
  n=$(basename "$f" .c)
  $CLANG -c -o "$OUT/$n.ref.o" "$f" 2>/dev/null &&
    wasm-ld $LDF -o "$OUT/$n.ref" "$B/obj/crt0_2.o" "$OUT/$n.ref.o" "$B/libc.a" 2>/dev/null ||
    wasm-ld $LDF -o "$OUT/$n.ref" "$B/obj/crt0_3.o" "$OUT/$n.ref.o" "$B/libc.a" || { echo "FAIL $n (clang reference did not build)"; fail=$((fail+1)); continue; }
  node "$T/run.mjs" "$OUT/$n.ref" > "$OUT/$n.exp" 2>&1; echo "exit $?" >> "$OUT/$n.exp"
  if ! $CC --sysroot="$SYSROOT" -c -o "$OUT/$n.o" "$f" 2>"$OUT/$n.err"; then
    echo "FAIL $n (cc)"; head -5 "$OUT/$n.err"; fail=$((fail+1)); continue
  fi
  if ! link "$OUT/$n" "$OUT/$n.o"; then echo "FAIL $n (link)"; head -5 "$OUT/link.err"; fail=$((fail+1)); continue; fi
  node "$T/run.mjs" "$OUT/$n" > "$OUT/$n.got" 2>&1; echo "exit $?" >> "$OUT/$n.got"
  if cmp -s "$OUT/$n.exp" "$OUT/$n.got"; then pass=$((pass+1)); else echo "FAIL $n (output)"; diff "$OUT/$n.exp" "$OUT/$n.got" | head -10; fail=$((fail+1)); fi
done
# mixed: main.c and lib.c built by different compilers must link and agree
M=$T/mix
if [ -f "$M/main.c" ]; then
  $CLANG -c -o "$OUT/mix.main.ref.o" "$M/main.c" && $CLANG -c -o "$OUT/mix.lib.ref.o" "$M/lib.c" &&
    wasm-ld $LDF -o "$OUT/mix.ref" "$B/obj/crt0_2.o" "$OUT/mix.main.ref.o" "$OUT/mix.lib.ref.o" "$B/libc.a" &&
    node "$T/run.mjs" "$OUT/mix.ref" > "$OUT/mix.exp" 2>&1
  $CC --sysroot="$SYSROOT" -c -o "$OUT/mix.main.o" "$M/main.c" && $CC --sysroot="$SYSROOT" -c -o "$OUT/mix.lib.o" "$M/lib.c"
  for combo in "main.o lib.ref.o" "main.ref.o lib.o" "main.o lib.o"; do
    set -- $combo
    if link "$OUT/mix.out" "$OUT/mix.$1" "$OUT/mix.$2" && node "$T/run.mjs" "$OUT/mix.out" > "$OUT/mix.got" 2>&1 && cmp -s "$OUT/mix.exp" "$OUT/mix.got"; then
      pass=$((pass+1))
    else
      echo "FAIL mix ($combo)"; cat "$OUT/link.err"; diff "$OUT/mix.exp" "$OUT/mix.got" | head; fail=$((fail+1))
    fi
  done
fi
for f in $T/err/*.c; do
  [ -f "$f" ] || continue
  n=$(basename "$f" .c)
  want=$(sed -n 's|.*// ERROR: ||p' "$f" | head -1)
  if $CC --sysroot="$SYSROOT" -c -o "$OUT/$n.o" "$f" 2>"$OUT/$n.err"; then
    echo "FAIL err/$n (compiled, expected: $want)"; fail=$((fail+1))
  elif grep -qF -- "$want" "$OUT/$n.err" && grep -q "$n.c:[0-9]*:[0-9]*: error" "$OUT/$n.err"; then pass=$((pass+1))
  else echo "FAIL err/$n (wanted '$want')"; head -3 "$OUT/$n.err"; fail=$((fail+1)); fi
done
echo "cc tests: $((pass+fail)) programs, $pass passed, $fail failed"
[ $fail -eq 0 ]
