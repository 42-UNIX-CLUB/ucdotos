#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Install REAL Debian and Ubuntu packages inside UC.OS from file: repositories.
#   sh libpkg/tests/os-real.sh BUILD DEBIAN_DEB... -- UBUNTU_DEB...
# BUILD is a build directory with the package tools in its image (os-test.sh
# makes one). The .deb files are not in the repository: download them first
# (for example Debian's hello, libc6, libgcc-s1 and gcc-14-base, and Ubuntu's
# hello). Nothing they contain is executed.
# Makes BUILD-real/ (same kernel, initrd plus /real); nothing in BUILD changes.
# Inside UC.OS: one repository per distribution, its Packages index compressed
# with xz (Debian) or zstd (Ubuntu); then apt update, apt install hello, dpkg -L,
# dpkg -V, remove and purge. Maintainer scripts cannot run yet (they need the
# binary engine), so --force-unrunnable-scripts records them as not run.
# The Release files are made here and not signed, so the sources are marked
# [trusted=yes] (apt prints its loud warning). libpkg/tests/os-signed.sh is the
# same kind of test with Debian's real, signed InRelease.
set -eu
B=${1:?usage: os-real.sh BUILD DEBIAN_DEB... -- UBUNTU_DEB...}; shift
top=$(cd "$(dirname "$0")/../.." && pwd)
cd "$top"
O=$top/$B-real
ST=$O/stage/real
mkdir -p "$ST/deb/pool" "$ST/deb/dists/trixie/main/binary-amd64" "$ST/ubu/pool" "$ST/ubu/dists/resolute/main/binary-amd64"
sh "$top/libpkg/tests/build-native.sh" "$O/native" >/dev/null   # dpkg-deb -f, to write the indexes
repo() { # DIR SUITE COMP DEB...: pool, Packages, Release
  d=$1 s=$2 c=$3; shift 3
  pk=$d/dists/$s/main/binary-amd64/Packages
  : > "$pk"
  for deb in "$@"; do
    command cp "$deb" "$d/pool/"
    "$O/native/bin/dpkg-deb" -f "$deb" >> "$pk"
    printf 'Filename: pool/%s\nSize: %s\nSHA256: %s\n\n' "$(basename "$deb")" "$(wc -c < "$deb" | tr -d ' ')" "$(sha256sum "$deb" | cut -d' ' -f1)" >> "$pk"
  done
  case $c in xz) xz -c "$pk" > "$pk.xz" ;; zst) zstd -q -c "$pk" > "$pk.zst" ;; esac
  { printf 'Origin: os-real\nSuite: %s\nCodename: %s\nArchitectures: amd64\nComponents: main\nSHA256:\n' "$s" "$s"
    for f in "$pk" "$pk.$c"; do printf ' %s %s main/binary-amd64/%s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(wc -c < "$f" | tr -d ' ')" "$(basename "$f")"; done
  } > "$d/dists/$s/Release"
  rm -f "$pk"   # only the compressed index is offered
}
debs=; ubus=; side=d
for f in "$@"; do
  if [ "$f" = -- ]; then side=u; continue; fi
  f=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
  if [ $side = d ]; then debs="$debs $f"; else ubus="$ubus $f"; fi
done
repo "$ST/deb" trixie xz $debs
repo "$ST/ubu" resolute zst $ubus
# The kernel's limit on name length (63 bytes today): a copy of the Ubuntu
# repository at a path that gives apt list names as long as real Ubuntu ones
# (67 bytes and more), and a package holding a file with a 60-byte name
# (69 bytes with dpkg's .dpkg-new suffix).
L=$ST/archive.ubuntu.com/ubuntu
mkdir -p "$L"
command cp -R "$ST/ubu/." "$L/"
lp=$O/longname
mkdir -p "$lp/DEBIAN" "$lp/usr/share/longname"
printf 'Package: longname\nVersion: 1\nArchitecture: all\nMaintainer: Test <test@example.invalid>\nDescription: a file with a long name\n x\n' > "$lp/DEBIAN/control"
echo x > "$lp/usr/share/longname/$(printf '%060d' 0 | tr 0 n)"
mkdir -p "$O/longname.b"; printf '2.0\n' > "$O/longname.b/debian-binary"
rm -f "$O/longname.b"/*.tar*
tar --format=gnu --owner=0 --group=0 -cf "$O/longname.b/control.tar" -C "$lp/DEBIAN" . && xz "$O/longname.b/control.tar"
tar --format=gnu --owner=0 --group=0 --exclude=./DEBIAN -cf "$O/longname.b/data.tar" -C "$lp" . && xz "$O/longname.b/data.tar"
rm -f "$ST/longname_1_all.deb"
(cd "$O/longname.b" && ar rcD "$ST/longname_1_all.deb" debian-binary control.tar.xz data.tar.xz)
cat > "$ST/run.sh" <<'EOF'
F=--force-unrunnable-scripts
echo "=== Debian: apt update (Packages.xz)"
mkdir -p /real/d/etc/apt
echo "deb [trusted=yes] file:/real/deb trixie main" > /real/d/etc/apt/sources.list
A="apt -o Dir=/real/d -o DPkg::Options::=$F"
$A update; echo "exit $?"
echo "=== apt install -y hello"
$A install -y hello; echo "exit $?"
echo "=== dpkg -l"
dpkg --root=/real/d -l
echo "=== dpkg -L hello"
dpkg --root=/real/d -L hello
echo "=== dpkg -V hello"
dpkg --root=/real/d -V hello; echo "exit $?"
echo "=== dpkg -V hello after changing one installed file"
echo changed >> /real/d/usr/share/doc/hello/copyright
dpkg --root=/real/d -V hello; echo "exit $?"
echo "=== dpkg -V libc6 (count of lines)"
dpkg --root=/real/d -V libc6 > /real/v.txt 2>&1; echo "exit $?"; wc -l < /real/v.txt
head -5 /real/v.txt
echo "=== the installed hello binary"
ls -l /real/d/usr/bin/hello
echo "=== apt remove -y hello"
$A remove -y hello; echo "exit $?"
echo "=== apt purge -y hello"
$A purge -y hello; echo "exit $?"
dpkg-query --root=/real/d -W -f '${db:Status-Status}\n' hello
echo "=== apt autoremove -y"
$A autoremove -y; echo "exit $?"
dpkg --root=/real/d -l
echo "=== Ubuntu: apt update (Packages.zst)"
mkdir -p /real/u/etc/apt
echo "deb [trusted=yes] file:/real/ubu resolute main" > /real/u/etc/apt/sources.list
U="apt -o Dir=/real/u -o DPkg::Options::=$F"
$U update; echo "exit $?"
apt-cache -o Dir=/real/u policy hello
echo "=== dpkg -i --force-depends (Ubuntu hello; its libc6 is not in the repository)"
for d in /real/ubu/pool/*.deb; do dpkg --root=/real/u $F --force-depends -i $d; echo "exit $?"; done
dpkg --root=/real/u -L hello
dpkg --root=/real/u -V hello; echo "exit $?"
dpkg --root=/real/u $F -P hello; echo "exit $?"
dpkg-query --root=/real/u -W -f '${db:Status-Status}\n' hello
echo "=== limits: apt update with list names over 63 bytes"
mkdir -p /real/l/etc/apt
echo "deb [trusted=yes] file:/real/archive.ubuntu.com/ubuntu resolute main" > /real/l/etc/apt/sources.list
apt -o Dir=/real/l update; echo "exit $?"
echo "=== limits: dpkg -i of a package with a 60-byte file name"
dpkg --root=/real/l -i /real/longname_1_all.deb; echo "exit $?"
dpkg-query --root=/real/l -W -f '${db:Status-Status}\n' longname
echo "=== done"
EOF
mkdir -p "$O"
command cp "$top/$B/kernel.wasm" "$O/"
command cp "$top/$B/initrd.tar" "$O/initrd.tar"
tar --format=ustar --owner=0 --group=0 -C "$O/stage" -rf "$O/initrd.tar" ./real
node tools/run.mjs --build="$B-real" --init=/bin/sh --input='sh /real/run.sh\nexit\n' 2>&1 | tee "$O/os-real.log"
