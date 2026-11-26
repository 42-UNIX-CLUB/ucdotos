#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run the package tools inside UC.OS. Builds an image with the tools, stages
# small .deb fixtures (members compressed with gzip, xz and zstd) and a file:
# repository (Packages.xz and Packages.gz) into it under /pkgtest (/pkgtest/r:
# apt's list names for binary-riscv64 must stay within the kernel's 63 bytes), boots it
# under Node and runs libpkg/tests/os/pkg-os.sh there.
#   sh libpkg/tests/os-test.sh [BUILD_DIR]      (default build-pkgos)
# Needs what `make` needs, plus Node, gzip, xz and zstd on the host.
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
B=${1:-build-pkgos}
cd "$top"
PROGS="sh cat grep ls mkdir rm cp mv head wc echo true false touch env dpkg dpkg-deb apt"   # apt-get, apt-cache and dpkg-query come with apt and dpkg (ALIASES)
nice -n 15 make B="$B" PROGS="$PROGS" >/dev/null
sh "$top/libpkg/tests/build-native.sh" "$top/$B/native" >/dev/null   # for dpkg-deb -f when writing the index
ST=$top/$B/root/pkgtest
mkdir -p "$ST/src" "$ST/r/pool" "$ST/r/dists/test/main/binary-riscv64"
mk() { # name version depends control-compression data-compression files...
  n=$1 v=$2 dep=$3 cc=$4 dc=$5; shift 5
  d=$top/$B/fixtures/${n}_$v
  mkdir -p "$d/DEBIAN"
  { printf 'Package: %s\nVersion: %s\nArchitecture: all\nMaintainer: Test <test@example.invalid>\nInstalled-Size: 4\n' "$n" "$v"
    [ -n "$dep" ] && printf 'Depends: %s\n' "$dep"
    printf 'Description: os test package %s\n a fixture.\n' "$n"; } > "$d/DEBIAN/control"
  for f in "$@"; do mkdir -p "$(dirname "$d/$f")"; echo "$n $v" > "$d/$f"; done
  printf '#!/bin/sh\necho "%s postinst $*" >> "$DPKG_ROOT/scripts.log"\n' "$n" > "$d/DEBIAN/postinst"
  chmod 755 "$d/DEBIAN/postinst"
  mkdir -p "$d.b"
  printf '2.0\n' > "$d.b/debian-binary"
  rm -f "$d.b"/control.tar* "$d.b"/data.tar*
  tar --format=ustar --owner=0 --group=0 -cf "$d.b/control.tar" -C "$d/DEBIAN" .
  tar --format=ustar --owner=0 --group=0 --exclude=./DEBIAN -cf "$d.b/data.tar" -C "$d" .
  z "$cc" "$d.b/control.tar"; z "$dc" "$d.b/data.tar"
  rm -f "$ST/src/${n}_${v}_all.deb"
  (cd "$d.b" && ar rcD "$ST/src/${n}_${v}_all.deb" debian-binary control.tar.$cc data.tar.$dc)
}
z() { case $1 in gz) gzip -9n "$2" ;; xz) xz "$2" ;; zst) zstd -q --rm "$2" ;; esac; }
mk osa 1 "osb" xz xz usr/share/osa/file
mk osb 1 "" gz zst usr/share/osb/file usr/share/common/b
mk osc 1 "" zst gz usr/share/common/c
mk osa 2 "osb" xz zst usr/share/osa/file usr/share/osa/new
pk=$ST/r/dists/test/main/binary-riscv64/Packages
: > "$pk"
for deb in "$ST/src/osa_1_all.deb" "$ST/src/osb_1_all.deb" "$ST/src/osc_1_all.deb"; do
  cp "$deb" "$ST/r/pool/"
  "$top/$B/native/bin/dpkg-deb" -f "$deb" >> "$pk"
  printf 'Filename: pool/%s\nSize: %s\nSHA256: %s\n\n' "$(basename "$deb")" "$(wc -c < "$deb" | tr -d ' ')" "$(sha256sum "$deb" | cut -d' ' -f1)" >> "$pk"
done
rm -f "$pk.xz" "$pk.gz"; xz -k "$pk"; gzip -9nk "$pk"
{ printf 'Origin: OS test\nSuite: test\nCodename: test\nArchitectures: riscv64\nComponents: main\nSHA256:\n'
  for f in "$pk" "$pk.xz" "$pk.gz"; do
    printf ' %s %s main/binary-riscv64/%s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(wc -c < "$f" | tr -d ' ')" "$(basename "$f")"
  done; } > "$ST/r/dists/test/Release"
rm -f "$pk"   # only the compressed indexes are offered, so apt must decompress inside UC.OS
cp "$top/libpkg/tests/os/pkg-os.sh" "$ST/pkg-os.sh"
tar --format=ustar --owner=0 --group=0 -C "$top/$B/root" -cf "$top/$B/initrd.tar" .
node tools/run.mjs --build="$B" --init=/bin/sh --input='sh /pkgtest/pkg-os.sh\nexit\n' 2>&1 | tee "$top/$B/pkg-os.log" | tail -40
grep -q 'pkg-os-tests: .* 0 failed' "$top/$B/pkg-os.log"
