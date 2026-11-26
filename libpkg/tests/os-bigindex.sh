#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# OPTIONAL: measure apt inside UC.OS with a full, real Packages index.
#   sh libpkg/tests/os-bigindex.sh PACKAGES_FILE [BUILD_DIR] [PKG...]
# PACKAGES_FILE is a real Packages index, compressed (Packages.xz, as on a
# Debian mirror) or not (for example the one real-repos.sh leaves under
# OUTDIR/real-*/debian-amd64/var/lib/apt/lists/).
# A Packages.xz is put in the image as a file: repository with a Release file,
# and `apt update` decompresses and checks it inside UC.OS; ps samples apt's
# memory and CPU time once a second while it runs. A plain file is placed as an
# already-updated list. Then apt-get resolves each PKG (simulation only) and
# prints the solver and memory statistics.
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
PK=${1:?usage: os-bigindex.sh PACKAGES_FILE [BUILD_DIR] [PKG...]}
B=${2:-build-pkgos}
shift; [ $# -gt 0 ] && shift
PKGS=${*:-hello git}
cd "$top"
nice -n 15 make B="$B" PROGS="sh cat grep ps sleep date dpkg apt apt-get apt-cache" >/dev/null
R=$top/$B/root/big
mkdir -p "$R/etc/apt" "$R/var/lib/apt/lists" "$R/var/lib/dpkg"
echo "deb file:/mirror trixie main" > "$R/etc/apt/sources.list"
: > "$R/var/lib/dpkg/status"
case $PK in
*.xz)
  M=$top/$B/root/mirror/dists/trixie
  mkdir -p "$M/main/binary-amd64"
  cp "$PK" "$M/main/binary-amd64/Packages.xz"
  { printf 'Origin: os-bigindex\nSuite: trixie\nCodename: trixie\nArchitectures: amd64\nComponents: main\nSHA256:\n'
    printf ' %s %s main/binary-amd64/Packages\n' "$(xz -dc "$PK" | sha256sum | cut -d' ' -f1)" "$(xz -dc "$PK" | wc -c | tr -d ' ')"
    printf ' %s %s main/binary-amd64/Packages.xz\n' "$(sha256sum "$PK" | cut -d' ' -f1)" "$(wc -c < "$PK" | tr -d ' ')"; } > "$M/Release"
  {
    echo 'date'
    echo 'apt -o Dir=/big update > /big/update.log 2>&1 &'
    echo 'while ps | grep -q " R .* apt$\| S .* apt$"; do ps | grep " apt$"; sleep 1; done'
    echo 'ps | grep " apt$"'
    echo 'wait'
    echo 'date'
    echo 'cat /big/update.log'
  } > "$R/run.sh" ;;
*)
  cp "$PK" "$R/var/lib/apt/lists/_mirror_dists_trixie_main_binary-amd64_Packages"
  : > "$R/run.sh" ;;
esac
{
  echo 'apt-cache -o Dir=/big -o APT::Solver::Stats=true stats'
  for p in $PKGS; do echo "apt-get -o Dir=/big -o APT::Solver::Stats=true -s install $p"; done
} >> "$R/run.sh"
tar --format=ustar --owner=0 --group=0 -C "$top/$B/root" -cf "$top/$B/initrd.tar" .
node tools/run.mjs --build="$B" --init=/bin/sh --input='sh /big/run.sh\nexit\n' 2>&1 | grep -E '^(index|solver|plan|Total|Memory|[0-9]+ upgraded|Hit|Get|Fetched|E:|W:| *[0-9]+ +[0-9]+ +[RSZ] )|UTC|[0-9]:[0-9][0-9]:[0-9][0-9]' || true
