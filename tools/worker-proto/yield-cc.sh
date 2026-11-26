#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Worker-process prototype, approach C (yield points), with clang only: a drop-in CC.
#   YIELD_MODE=loops  (default) compile C to WebAssembly assembly (clang -S), insert a counter
#                     check right after every `loop` instruction (every loop iteration passes
#                     there), assemble the result (clang -c file.s)
#   YIELD_MODE=sancov clang -fsanitize-coverage=trace-pc-guard: a hook call on every edge
#   YIELD_MODE=none   plain clang (the baseline for measuring)
# When linking, it adds $YIELD_RT (the counter global and the slow path, yield-rt.s and
# yield-slow.c, built by proto.mk). Used as: make CC="sh tools/worker-proto/yield-cc.sh".
CLANG="clang --target=wasm32-unknown-unknown"
MODE=${YIELD_MODE:-loops}
compile=0; src=; out=; prev=; rest=
for a in "$@"; do
  case $a in -c) compile=1 ;; esac
  case $a in *.c) src=$a ;; esac
  if [ "$prev" = "-o" ]; then out=$a; fi
  prev=$a
done
if [ $compile = 0 ]; then
  # The default Makefile links programs straight from .c files: compile each one through
  # this wrapper first, so it is instrumented too, then link the objects.
  flags=; files=; prev=; n=0
  for a in "$@"; do
    if [ "$prev" = "-o" ]; then prev=; continue; fi
    case $a in -o) ;; -Wl,*) ;; -*) flags="$flags $a" ;; esac
    prev=$a
  done
  prev=
  for a in "$@"; do
    case $a in
      *.c) n=$((n + 1)); o="$out.$n.o"; sh "$0" $flags -c -o "$o" "$a" || exit 1; files="$files $o" ;;
      *) files="$files $a" ;;
    esac
  done
  exec $CLANG $files $YIELD_RT
fi
if [ -z "$src" ] || [ "$MODE" = none ]; then exec $CLANG "$@"; fi
if [ "$MODE" = sancov ]; then exec $CLANG -fsanitize-coverage=trace-pc-guard "$@"; fi
# loops: same arguments, but -S into a temporary file
prev=
for a in "$@"; do
  if [ "$prev" = "-o" ]; then a="$out.raw.s"; fi
  if [ "$a" = "-c" ]; then a="-S"; fi
  rest="$rest $a"
  prev=$a
done
$CLANG $rest || exit 1
{
  printf '\t.functype\t__ucos_yield_slow () -> ()\n\t.globaltype\t__ucos_yield_ctr, i32\n'
  awk '{ print }
       /^[ \t]*loop([ \t]|$)/ {
         print "\tglobal.get\t__ucos_yield_ctr"
         print "\ti32.const\t1"
         print "\ti32.sub"
         print "\tglobal.set\t__ucos_yield_ctr"
         print "\tglobal.get\t__ucos_yield_ctr"
         print "\ti32.const\t0"
         print "\ti32.lt_s"
         print "\tif"
         print "\tcall\t__ucos_yield_slow"
         print "\tend_if"
       }' "$out.raw.s"
} > "$out.s" || exit 1
$CLANG -c -o "$out" "$out.s"
