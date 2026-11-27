#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check the archive keys shipped in keyrings/ (copied into the image at
# /debian/etc/apt/trusted.gpg.d/). It DOWNLOADS, with the host's curl and a
# generic client name: Debian's page of archive keys
# (https://ftp-master.debian.org/keys.html) and today's trixie InRelease.
#   sh libpkg/tests/keyrings-check.sh [OUT]       (default build-keyrings-check/)
# 1. Every primary key in keyrings/*.asc|*.gpg|*.pgp must have its fingerprint
#    on Debian's page (compared without spaces, upper case).
# 2. Today's trixie InRelease must verify with those keys alone
#    (apt-keyring verify, built natively by build-native.sh).
# Exits 1 if either fails.
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-keyrings-check}
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out/root"   # apt-keyring on the host needs a root (Dir); nothing is read from it
UA=keyring-check
sh "$top/libpkg/tests/build-native.sh" "$out/native" >/dev/null
export ASAN_OPTIONS=detect_leaks=0
curl -fsS -A "$UA" -o "$out/keys.html" https://ftp-master.debian.org/keys.html
curl -fsS -A "$UA" -o "$out/InRelease" http://deb.debian.org/debian/dists/trixie/InRelease
published=$(sed 's/<[^>]*>/ /g' "$out/keys.html" | tr -d ' \n' | tr a-f A-F)
fail=0 n=0 args=
for f in "$top"/keyrings/*.asc "$top"/keyrings/*.gpg "$top"/keyrings/*.pgp; do
  [ -f "$f" ] || continue
  args="$args --keyring $f"
  for fp in $("$out/native/bin/apt-keyring" -o Dir="$out/root" list "$f" | awk '$1=="pub"{getline; gsub(/ /,""); print}'); do
    n=$((n + 1))
    if printf '%s' "$published" | grep -q "$fp"; then echo "ok    $fp ($(basename "$f")) is on ftp-master.debian.org/keys.html"
    else echo "FAIL  $fp ($(basename "$f")) is NOT on ftp-master.debian.org/keys.html"; fail=1; fi
  done
done
[ $n -gt 0 ] || { echo "FAIL  no keys in $top/keyrings/"; exit 1; }
if "$out/native/bin/apt-keyring" -o Dir="$out/root" verify $args "$out/InRelease"; then echo "ok    today's trixie InRelease verifies with the shipped keys"
else echo "FAIL  today's trixie InRelease does not verify with the shipped keys"; fail=1; fi
exit $fail
