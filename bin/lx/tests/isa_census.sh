#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Instruction census for the architecture table in docs/LINUX-BINARIES.md.
# Needs roots made by fetch-debian.sh for each architecture:
#   LX_SCRATCH=$S/arch-amd64 LX_ARCH=amd64 sh fetch-debian.sh busybox-static bash coreutils libc6 python3.13-minimal
# then: sh isa_census.sh AMD64_ROOT ARM64_ROOT RISCV64_ROOT
# Heavy (disassembles about 9 MB per architecture): run it alone, niced.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
R1=$1; R2=$2; R3=$3
for spec in "amd64:$R1" "arm64:$R2" "riscv64:$R3"; do
  a=${spec%%:*}; R=${spec#*:}
  L=$(find "$R/usr/lib" -maxdepth 1 -name '*-linux-gnu' | head -1)
  set -- "$R/usr/bin/busybox" "$R/usr/bin/bash" "$L/libc.so.6"
  bytes=0
  for f in "$@"; do
    t=$(llvm-objdump -h "$f" | awk '$2==".text"{print $3}')
    bytes=$((bytes + 0x$t))
  done
  echo "$a: .text of busybox, bash, libc.so.6 = $bytes bytes"
  nice -n 19 python3 -I "$HERE/isa_count.py" "$a" "$@"
done
