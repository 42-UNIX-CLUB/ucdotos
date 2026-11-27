#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch the archive keyring packages of Debian and Ubuntu from their official
# mirrors, extract the keyring files and print every key's fingerprint, so a
# person can compare them with the fingerprints the projects publish:
#   Debian: https://ftp-master.debian.org/keys.html
#   Ubuntu: https://wiki.ubuntu.com/SecurityTeam/FAQ (and keyserver.ubuntu.com)
# Nothing here decides which keys UC.OS trusts. The files land in OUT (default
# build-keyrings/ at the top of the tree, git-ignored), never in the tree.
#   sh libpkg/tests/fetch-keyrings.sh [OUT]
# Needs curl, ar, tar, xz or zstd. Uses the host's gpg for a second listing if
# it is installed, and the native apt-keyring of build-native.sh if OUT/native
# exists or can be built.
# To ship a keyring in the image (an owner's decision), copy the chosen .gpg
# or .asc files into keyrings/ at the top of the tree (or build with KEYRINGS=DIR); the Makefile
# copies that directory, if it exists, to /debian/etc/apt/trusted.gpg.d/.
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-keyrings}
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
export TMPDIR="$out"
UA="pkg-keyring-check"
fetch() { curl -fsS -A "$UA" -o "$2" "$1"; }
newest() { # pool URL, package name: the highest version in the pool listing
  curl -fsS -A "$UA" "$1" | grep -o "href=\"$2_[^\"]*_all\.deb\"" | sed 's/^href="//; s/"$//' | sort -V | tail -1
}
unpack() { # deb dir
  mkdir -p "$2"
  (cd "$2" && ar x "$1" && for m in data.tar.*; do
     case $m in *.xz) xz -dc "$m" ;; *.zst) zstd -qdc "$m" ;; *.gz) gzip -dc "$m" ;; *) cat "$m" ;; esac | tar -xf -
   done)
}
for spec in "http://deb.debian.org/debian/pool/main/d/debian-archive-keyring/ debian-archive-keyring" \
            "http://archive.ubuntu.com/ubuntu/pool/main/u/ubuntu-keyring/ ubuntu-keyring"; do
  set -- $spec
  name=$(newest "$1" "$2")
  [ -n "$name" ] || { echo "no $2 package found at $1"; exit 1; }
  echo "== $1$name"
  [ -f "$out/$name" ] || fetch "$1$name" "$out/$name"
  echo "   sha256 $(sha256sum "$out/$name" | cut -d' ' -f1)  (compare with the SHA256 field in the suite's Packages index)"
  unpack "$out/$name" "$out/${name%.deb}"
done
echo
echo "Keyring files:"
find "$out" -path "$out/native" -prune -o \( -name '*.gpg' -o -name '*.asc' -o -name '*.pgp' \) -type f -print | sort | sed "s|^$out/|  |"
if command -v gpg >/dev/null 2>&1; then
  echo
  echo "Fingerprints according to the host's gpg:"
  G=$out/gnupg; mkdir -p "$G"; chmod 700 "$G"
  find "$out" -path "$out/native" -prune -o \( -name '*.gpg' -o -name '*.asc' -o -name '*.pgp' \) -type f -print | sort | while read -r f; do
    echo "--- ${f#"$out"/}"
    GNUPGHOME=$G gpg --batch --quiet --no-autostart --show-keys --with-subkey-fingerprints --keyid-format long "$f" 2>&1 | sed 's/^/  /'
  done
fi
if sh "$top/libpkg/tests/build-native.sh" "$out/native" >/dev/null 2>&1 && [ -x "$out/native/bin/apt-keyring" ]; then
  echo
  echo "Fingerprints according to this system's own OpenPGP code (apt-keyring list):"
  find "$out" -path "$out/native" -prune -o \( -name '*.gpg' -o -name '*.asc' -o -name '*.pgp' \) -type f -print | sort | while read -r f; do
    echo "--- ${f#"$out"/}"
    ASAN_OPTIONS=detect_leaks=0 "$out/native/bin/apt-keyring" list "$f" 2>&1 | sed 's/^/  /'
  done
fi
