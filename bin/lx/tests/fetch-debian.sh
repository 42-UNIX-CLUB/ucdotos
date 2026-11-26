#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Download Debian riscv64 packages for the engine's tests and unpack them into a guest
# root. Nothing here goes into the repository: everything lands in $LX_SCRATCH.
#
#   LX_SCRATCH=/some/dir sh bin/lx/tests/fetch-debian.sh [package ...]
#
# Default packages: what the milestones need. Uses the host's curl, xz, ar and tar.
# The Debian release is "stable" (trixie, Debian 13, when this was written).
set -eu
: "${LX_SCRATCH:?set LX_SCRATCH to a scratch directory outside the repository}"
ARCH=${LX_ARCH:-riscv64}
MIRROR=${LX_MIRROR:-https://deb.debian.org/debian}
SUITE=${LX_SUITE:-stable}
mkdir -p "$LX_SCRATCH/debs" "$LX_SCRATCH/root"
IDX="$LX_SCRATCH/Packages-$ARCH"
if [ ! -s "$IDX" ]; then
  echo "fetching the $SUITE/main/$ARCH package index"
  curl -sfL "$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.xz" | xz -dc > "$IDX"
fi
PKGS=${*:-"busybox-static libc6 libgcc-s1 dash bash libtinfo6 coreutils libacl1 libattr1 libselinux1 libpcre2-8-0 libgmp10 libssl3t64 libcap2 dpkg zlib1g liblzma5 libbz2-1.0 libzstd1 libmd0 tar sed grep gawk libmpfr6 libsigsegv2 libreadline8t64 findutils diffutils perl-base apt libapt-pkg7.0 libstdc++6 libgnutls30t64 libsystemd0 libudev1 libxxhash0 libseccomp2 liblz4-1 libgcrypt20 libgpg-error0 libp11-kit0 libtasn1-6 libnettle8t64 libhogweed6t64 libidn2-0 libunistring5 libffi8 libcrypt1 debianutils base-files base-passwd mawk python3.13-minimal libpython3.13-minimal libexpat1 libdb5.3t64 gzip bc libc-bin libdebconfclient0 libpython3.13-stdlib"}
for p in $PKGS; do
  f=$(awk -v p="$p" '$1=="Package:"{cur=$2} $1=="Filename:" && cur==p {print $2; exit}' "$IDX")
  if [ -z "$f" ]; then echo "not in the index: $p" >&2; continue; fi
  deb="$LX_SCRATCH/debs/$(basename "$f")"
  if [ ! -s "$deb" ]; then
    echo "fetching $p"
    curl -sfL -o "$deb" "$MIRROR/$f"
  fi
  # unpack data.tar.* into the root (ar member names vary: data.tar.xz / .zst / .gz)
  tmp="$LX_SCRATCH/unpack"
  mkdir -p "$tmp"
  rm -f "$tmp"/*
  (cd "$tmp" && ar x "$deb")
  for d in "$tmp"/data.tar.*; do
    case "$d" in
      *.xz) xz -dc "$d" | tar -x -C "$LX_SCRATCH/root" ;;
      *.zst) zstd -dc "$d" | tar -x -C "$LX_SCRATCH/root" ;;
      *.gz) gzip -dc "$d" | tar -x -C "$LX_SCRATCH/root" ;;
      *) tar -xf "$d" -C "$LX_SCRATCH/root" ;;
    esac
  done
  rm -f "$tmp"/*
done
echo "root: $LX_SCRATCH/root"
