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
PROGS="sh cat grep ls mkdir rm cp mv head wc echo true false touch env dpkg dpkg-deb dpkg-query apt apt-get apt-cache apt-keyring"
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
# Sign the repository with a key made here by the host's gpg; the image gets
# the public key in /pkgtest/keys. Two more repositories: ru, unsigned, and rb,
# with an InRelease changed after signing (copies; files are overwritten,
# nothing is deleted; short names keep apt's list names under 63 bytes).
command -v gpg >/dev/null 2>&1 || { echo "os-test.sh needs the host's gpg to sign the test repository"; exit 1; }
GH=$top/$B/gnupg; mkdir -p "$GH"; chmod 700 "$GH"
G() { GNUPGHOME=$GH gpg --batch --quiet --no-tty --pinentry-mode loopback --passphrase '' "$@"; }
G --list-keys os-test@example.invalid >/dev/null 2>&1 || G --quick-gen-key 'OS Test Archive <os-test@example.invalid>' ed25519 sign never
mkdir -p "$ST/keys"
G --export os-test@example.invalid > "$ST/keys/test.gpg"
rm -f "$ST/r/dists/test/InRelease"
G -u os-test@example.invalid --clearsign -o "$ST/r/dists/test/InRelease" "$ST/r/dists/test/Release"
for r in ru rb; do mkdir -p "$ST/$r"; cp -R "$ST/r/." "$ST/$r/"; done
rm -f "$ST/ru/dists/test/InRelease"
sed 's/^Codename: test$/Codename: tesT/' "$ST/r/dists/test/InRelease" > "$ST/rb/dists/test/InRelease"
command -v gpgconf >/dev/null 2>&1 && gpgconf --homedir "$GH" --kill gpg-agent 2>/dev/null   # stop the agent gpg started
cp "$top/libpkg/tests/os/pkg-os.sh" "$ST/pkg-os.sh"
tar --format=ustar --owner=0 --group=0 -C "$top/$B/root" -cf "$top/$B/initrd.tar" .
node tools/run.mjs --build="$B" --init=/bin/sh --input='sh /pkgtest/pkg-os.sh\nexit\n' 2>&1 | tee "$top/$B/pkg-os.log" | tail -40
grep -q 'pkg-os-tests: .* 0 failed' "$top/$B/pkg-os.log"
