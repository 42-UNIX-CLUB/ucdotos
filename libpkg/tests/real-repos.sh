#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# OPTIONAL test against real repositories. It DOWNLOADS, with the host's curl:
#   Debian 13 (trixie) main:      dists/trixie/{InRelease,Release} and
#                                 main/binary-{amd64,arm64,riscv64}/Packages.xz (about 9 MB each)
#   Ubuntu 26.04 (resolute) main and universe, same files, amd64 from
#   archive.ubuntu.com and arm64/riscv64 from ports.ubuntu.com (about 17 MB per architecture)
# into MIRROR (first argument; nothing is downloaded twice). Then, per distribution
# and architecture, it runs `apt update` from a file: source and resolves a list of
# packages from an empty system, printing package counts, download sizes and
# installed sizes. Nothing is installed.
#   sh libpkg/tests/real-repos.sh MIRROR [OUTDIR]
# APT_EXTRA adds apt options, e.g. APT_EXTRA="-o APT::Install-Recommends=false".
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
M=${1:?usage: real-repos.sh MIRROR [OUTDIR]}
out=${2:-$top/build-pkgnative}
mkdir -p "$M"
M=$(cd "$M" && pwd)
CFLAGS="-O2 -Wall -Wno-unused-parameter -Wno-format-truncation" sh "$top/libpkg/tests/build-native.sh" "$out-opt" >/dev/null
BIN=$out-opt/bin
get() { mkdir -p "$(dirname "$2")"; [ -s "$2" ] || curl -fsSL --retry 3 -o "$2" "$1"; }
D=https://deb.debian.org/debian
U=http://archive.ubuntu.com/ubuntu
P=http://ports.ubuntu.com/ubuntu-ports
for f in InRelease Release; do
  get $D/dists/trixie/$f "$M/debian/dists/trixie/$f"
  get $U/dists/resolute/$f "$M/ubuntu/dists/resolute/$f"
  get $P/dists/resolute/$f "$M/ubuntu-ports/dists/resolute/$f"
done
for a in amd64 arm64 riscv64; do get $D/dists/trixie/main/binary-$a/Packages.xz "$M/debian/dists/trixie/main/binary-$a/Packages.xz"; done
for c in main universe; do
  get $U/dists/resolute/$c/binary-amd64/Packages.xz "$M/ubuntu/dists/resolute/$c/binary-amd64/Packages.xz"
  for a in arm64 riscv64; do get $P/dists/resolute/$c/binary-$a/Packages.xz "$M/ubuntu-ports/dists/resolute/$c/binary-$a/Packages.xz"; done
done

W=$out/real-$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$W/conf"
export UCOS_PKG_CONFDIR=$W/conf
PKGS=${PKGS:-hello busybox-static bash coreutils python3-minimal gcc git nginx-light}
printf '%-8s %-8s %-16s %5s %10s %11s %5s %10s %11s %8s\n' distro arch package pkgs download installed "+base" download installed solve_ms
for distro in debian ubuntu; do
  for a in amd64 arm64 riscv64; do
    R=$W/$distro-$a
    mkdir -p "$R/etc/apt"
    if [ $distro = debian ]; then echo "deb [arch=$a] file:$M/debian trixie main" > "$R/etc/apt/sources.list"
    elif [ $a = amd64 ]; then echo "deb [arch=$a] file:$M/ubuntu resolute main universe" > "$R/etc/apt/sources.list"
    else echo "deb [arch=$a] file:$M/ubuntu-ports resolute main universe" > "$R/etc/apt/sources.list"; fi
    A="-o Dir=$R -o APT::Architecture=$a ${APT_EXTRA:-}"
    "$BIN/apt-get" $A -q update > "$R/update.log" 2>&1 || { echo "update failed for $distro $a (see $R/update.log)"; continue; }
    # the base system: every package of priority required (what debootstrap's minbase starts from)
    base=$(for f in "$R"/var/lib/apt/lists/*_Packages; do awk '$1=="Package:"{n=$2} $1=="Priority:"&&$2=="required"{print n}' "$f"; done | sort -u | tr '\n' ' ')
    "$BIN/apt-get" $A -s -o APT::Solver::Stats=true install $base > "$R/base.log" 2> "$R/base.stats" || true
    bn=$(sed -n 's/^plan: \([0-9]*\) packages, download [^(]*(\([0-9]*\) bytes), installed size [^(]*(\([0-9]*\) KiB)$/\1 \2 \3/p' "$R/base.stats")
    set -- $bn
    printf '%-8s %-8s %-16s %5s %10s %11s\n' $distro $a "(base: required)" "${1:-?}" "${2:-?}" "${3:-?}"
    for p in $PKGS; do
      "$BIN/apt-get" $A -s -o APT::Solver::Stats=true install $p > "$R/$p.log" 2> "$R/$p.stats" || { echo "$distro $a $p: FAILED (see $R/$p.log)"; continue; }
      s=$(sed -n 's/^plan: \([0-9]*\) packages, download [^(]*(\([0-9]*\) bytes), installed size [^(]*(\([0-9]*\) KiB)$/\1 \2 \3/p' "$R/$p.stats")
      ms=$(sed -n 's/^solver: .* \([0-9.]*\) ms, .*$/\1/p' "$R/$p.stats")
      "$BIN/apt-get" $A -s -o APT::Solver::Stats=true install $base $p > /dev/null 2> "$R/$p+base.stats" || true
      t=$(sed -n 's/^plan: \([0-9]*\) packages, download [^(]*(\([0-9]*\) bytes), installed size [^(]*(\([0-9]*\) KiB)$/\1 \2 \3/p' "$R/$p+base.stats")
      set -- $s $t
      printf '%-8s %-8s %-16s %5s %10s %11s %5s %10s %11s %8s\n' $distro $a $p "${1:-?}" "${2:-?}" "${3:-?}" "${4:-?}" "${5:-?}" "${6:-?}" "${ms:-?}"
    done
    grep '^index:' "$R/hello.stats" | sed "s/^/  $distro $a /" || true
  done
done
echo "logs: $W"
