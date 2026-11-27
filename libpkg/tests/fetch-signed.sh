#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch the current signed Release files of the suites the signature tests use:
# InRelease, and Release with its detached Release.gpg where the mirror still
# offers them. About 2 MB in all, a few requests per host.
#   sh libpkg/tests/fetch-signed.sh [OUT]      (default build-signed/, git-ignored)
# Files go to OUT/NAME/{InRelease,Release,Release.gpg}; NAME is listed below.
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-signed}
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
UA="pkg-signature-test"
while read -r name base; do
  [ -n "$name" ] || continue
  mkdir -p "$out/$name"
  for f in InRelease Release Release.gpg; do
    if curl -fsS -A "$UA" -o "$out/$name/$f.part" "$base/$f" 2>/dev/null; then mv "$out/$name/$f.part" "$out/$name/$f"
    else rm -f "$out/$name/$f.part"; echo "$name: no $f"; fi
  done
  echo "$name: $(command ls "$out/$name" | tr '\n' ' ')"
done <<EOF
debian-trixie http://deb.debian.org/debian/dists/trixie
debian-trixie-updates http://deb.debian.org/debian/dists/trixie-updates
debian-trixie-security http://security.debian.org/debian-security/dists/trixie-security
debian-bookworm http://deb.debian.org/debian/dists/bookworm
ubuntu-resolute http://archive.ubuntu.com/ubuntu/dists/resolute
ubuntu-resolute-ports http://ports.ubuntu.com/ubuntu-ports/dists/resolute
ubuntu-resolute-updates http://archive.ubuntu.com/ubuntu/dists/resolute-updates
ubuntu-resolute-updates-ports http://ports.ubuntu.com/ubuntu-ports/dists/resolute-updates
EOF
