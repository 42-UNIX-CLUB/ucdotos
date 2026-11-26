#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native test suite for the Linux binary engine. From a fresh clone:
#   sh bin/lx/tests/run.sh            # unit tests and own guest programs
#   LX_SCRATCH=/dir sh bin/lx/tests/run.sh   # also Debian binaries fetched by fetch-debian.sh
# Options (environment): LX_SAN=1 builds the engine with AddressSanitizer and UBSan.
# Build output goes to $LX_OUT (default: /tmp/lx-test-$USER), never into the repository.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(dirname "$HERE")
OUT=${LX_OUT:-/tmp/lx-test-$(id -un)}
mkdir -p "$OUT"
CC=${CC:-cc}
CFLAGS="-O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -fno-math-errno"
[ "${LX_SAN:-0}" = 1 ] && CFLAGS="$CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined"
RV="clang --target=riscv64-unknown-elf -mabi=lp64d -ffreestanding -nostdlib -static -fuse-ld=lld -ffp-contract=off -fno-math-errno"
pass=0; fail=0; skip=0
ok() { pass=$((pass+1)); echo "ok    $1"; }
bad() { fail=$((fail+1)); echo "FAIL  $1"; }
skipped() { skip=$((skip+1)); echo "skip  $1"; }

echo "building the engine natively ($OUT/lx)"
$CC $CFLAGS -Wno-format-truncation -o "$OUT/lx" "$SRC"/*.c || { echo "build failed"; exit 1; }
LX="$OUT/lx"

# 1. soft-float against the host FPU, and the fast paths against soft-float
$CC -O2 -frounding-math -ffp-contract=off -fno-builtin -mfma -o "$OUT/fp_test" "$HERE/fp_test.c" "$SRC/softfp.c" "$SRC/rv_fp.c" -lm \
  && "$OUT/fp_test" ${LX_FP_ITER:-300000} > "$OUT/fp_test.out" 2>&1 && ok "fp_test: $(tail -1 "$OUT/fp_test.out")" || bad "fp_test: $(tail -3 "$OUT/fp_test.out")"

# 2. decoder round trip through the LLVM assembler
if command -v llvm-mc > /dev/null; then
  $CC -O2 -o "$OUT/decode_test" "$HERE/decode_test.c" "$SRC/rv_decode.c" "$SRC/rv_disasm.c" \
    && python3 -I "$HERE/decode_test.py" "$OUT" 30000 > "$OUT/decode.out" 2>&1 && ok "decoder: $(tail -1 "$OUT/decode.out")" || bad "decoder: $(tail -5 "$OUT/decode.out")"
else skipped "decoder round trip (no llvm-mc)"; fi

# 3. path translation module
$CC -O2 -Wall -o "$OUT/path_test" "$HERE/path_test.c" "$SRC/path.c" && "$OUT/path_test" > "$OUT/path.out" 2>&1 \
  && ok "path: $(tail -1 "$OUT/path.out")" || bad "path: $(cat "$OUT/path.out")"

# 4. own guest programs
if command -v ld.lld > /dev/null; then
  clang --target=riscv64-unknown-elf -march=rv64gc -c -o "$OUT/hello.o" "$HERE/guest/hello.S" && ld.lld -o "$OUT/hello" "$OUT/hello.o"
  [ "$("$LX" "$OUT/hello")" = "Hello, world!" ] && ok "M1 hello (assembly)" || bad "M1 hello"
  $CC -O2 -mfma -ffp-contract=off -fno-math-errno -o "$OUT/ops-native" "$HERE/guest/ops.c" -lm && "$OUT/ops-native" > "$OUT/ops.expect"
  for fl in "-O0 -march=rv64gc" "-O1 -march=rv64gc" "-O2 -march=rv64gc" "-O2 -march=rv64g" "-Os -march=rv64imafdc"; do
    # shellcheck disable=SC2086
    $RV $fl -o "$OUT/ops-rv" "$HERE/guest/ops.c" && "$LX" "$OUT/ops-rv" > "$OUT/ops.got" 2>&1
    if cmp -s "$OUT/ops.got" "$OUT/ops.expect"; then ok "ops.c differential ($fl)"; else bad "ops.c differential ($fl)"; diff "$OUT/ops.expect" "$OUT/ops.got" | head -10; fi
  done
  for t in insn sig; do
    if [ -f "$HERE/guest/$t.c" ]; then
      $RV -O1 -march=rv64gc -o "$OUT/$t" "$HERE/guest/$t.c" && "$LX" "$OUT/$t" > "$OUT/$t.out" 2>&1
      r=$(tail -1 "$OUT/$t.out"); case "$r" in *" 0 failed"*) ok "$t: $r" ;; *) bad "$t: $(tail -5 "$OUT/$t.out")" ;; esac
    fi
  done
else skipped "guest programs (no ld.lld)"; fi

# 5. Debian binaries (need LX_SCRATCH with a root from fetch-debian.sh)
R=${LX_SCRATCH:-}/root
if [ -n "${LX_SCRATCH:-}" ] && [ -x "$R/usr/bin/busybox" ]; then
  L="$LX --root=$R"
  cp "$HERE/guest/m2.sh" "$R/tmp/lx-m2.sh"
  for mode in host emulate; do
    r=$(LX_FORK=$( [ $mode = emulate ] && echo emulate || echo host ) timeout 120 $L /bin/busybox sh /tmp/lx-m2.sh /bin/busybox /tmp 2>&1 | tail -1)
    case "$r" in *" 0 failed"*) ok "M2 busybox ($mode fork): $r" ;; *) bad "M2 busybox ($mode fork): $r" ;; esac
  done
  [ "$(timeout 30 $L /bin/dash -c 'echo $((40+2))')" = 42 ] && ok "M3/M4 dash (dynamic, glibc)" || bad "dash"
  mkdir -p "$R/lx" && cp "$HERE/guest/m34.sh" "$R/lx/m34.sh"
  r=$(timeout 60 $L /bin/dash /lx/m34.sh 2>&1 | tail -1)
  case "$r" in *" 0 failed"*) ok "M3/M4 script: $r" ;; *) bad "M3/M4 script: $r" ;; esac
  deb=$(ls "$LX_SCRATCH"/debs/bc_*.deb 2>/dev/null | head -1)
  if [ -n "$deb" ]; then
    mkdir -p "$R/tmp/debs" && cp "$deb" "$R/tmp/debs/"
    timeout 60 $L /usr/bin/dpkg-deb -c "/tmp/debs/$(basename "$deb")" 2>&1 | grep -q "usr/bin/bc$" && ok "M5 dpkg-deb -c (liblzma worker thread: green threads)" || bad "dpkg-deb -c"
  fi
  [ "$(timeout 30 $L /bin/bash -c 'a=(x y z); echo ${a[2]}')" = z ] && ok "M4 bash" || bad "bash"
  [ "$(timeout 30 $L /bin/ls -d /usr)" = /usr ] && ok "M4 coreutils ls" || bad "coreutils ls"
  timeout 30 $L /usr/bin/dpkg --version 2>&1 | grep -q "version 1\." && ok "M5 dpkg --version" || bad "dpkg --version"
  timeout 30 $L /usr/bin/apt-get --version 2>&1 | grep -q "^apt " && ok "M5 apt-get --version" || bad "apt-get --version"
  [ "$(LANG=C timeout 30 $L /usr/bin/perl -e 'print 6*7')" = 42 ] && ok "M5 perl -e" || bad "perl -e"
  if [ -d "$R/usr/lib/python3.13/encodings" ]; then
    [ "$(timeout 60 $L /usr/bin/python3.13 -c 'print(1)')" = 1 ] && ok "M5 python3 -c" || bad "python3 -c"
    [ "$(timeout 60 $L /usr/bin/python3.13 -c 'import threading as t; r=[]; ts=[t.Thread(target=lambda i=i: r.append(i*i)) for i in range(4)]; [x.start() for x in ts]; [x.join() for x in ts]; print(sum(r))')" = 14 ] && ok "python3 threading (green threads)" || bad "python3 threading"
  else skipped "python3 (libpython3.13-stdlib not unpacked)"; fi
else skipped "Debian binaries (set LX_SCRATCH, run fetch-debian.sh)"; fi

echo "lx tests: $((pass+fail)) run, $pass passed, $fail failed, $skip skipped"
[ $fail = 0 ]
