#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native build of cc and ld for development (with sanitizers), plus a sysroot that
# mirrors what the UC.OS image ships: usr/include, usr/lib/cc/include, usr/lib.
# usage: sh bin/cc/tests/build.sh [build-dir]
set -eu
B=${1:-build-cc}
T=$(dirname "$0")
ROOT=$(cd "$T/../../.." && pwd)
cd "$ROOT"
HOSTCC=${HOSTCC:-cc}
FLAGS=${FLAGS:--O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fsanitize=address,undefined}
mkdir -p "$B/native"
make -s B="$B" "$B/libc.a" "$B/obj/crt0_2.o" "$B/obj/crt0_3.o" "$B/libui.a" "$B/libgfx.a"
$HOSTCC $FLAGS -o "$B/native/cc" bin/cc/*.c
if [ -d bin/ld ]; then $HOSTCC $FLAGS -o "$B/native/ld" bin/ld/*.c; fi
S=$B/sysroot/usr
mkdir -p "$S/include/sys" "$S/include/ucos" "$S/lib/cc/include"
cp libc/include/*.h "$S/include/"
cp libc/include/sys/*.h "$S/include/sys/"
cp abi/abi.h "$S/include/ucos/abi.h"
sed 's|#include "../../../abi/abi.h"|#include <ucos/abi.h>|' libc/include/sys/syscall.h > "$S/include/sys/syscall.h"
cp bin/cc/include/*.h "$S/lib/cc/include/"
cp "$B/libc.a" "$B/libui.a" "$B/libgfx.a" "$S/lib/"
cp "$B/obj/crt0_2.o" "$B/obj/crt0_3.o" "$S/lib/"
