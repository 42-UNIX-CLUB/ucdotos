#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# apt update with REAL signatures inside UC.OS: Debian's current trixie
# InRelease, its real main/binary-riscv64/Packages.xz and real riscv64 packages
# (hello, base-files and their dependencies, busybox-static), in a file:
# repository, checked against the Debian archive keys shipped in keyrings/
# (the image has them in /debian/etc/apt/trusted.gpg.d; the test copies them).
#   sh libpkg/tests/os-signed.sh BUILD [DATA]
# BUILD is a build directory with the package tools in its image (os-test.sh
# makes one). DATA (default build-signed-repo/) receives the downloads the
# first time: InRelease, Packages.xz (about 10 MB) and seven .deb files (a few
# MB) from deb.debian.org. The keys are the ones in keyrings/. BUILD needs
# lx in its image (a full build). Nothing in the tree or in BUILD changes: the run uses
# BUILD-signed/ (same kernel, initrd plus /s).
# Inside UC.OS: apt update must succeed with no warning and install hello; a
# changed InRelease, one without its signature, one checked without the keys
# and one checked with the clock in 2040 must all be refused.
set -eu
B=${1:?usage: os-signed.sh BUILD [DATA]}
top=$(cd "$(dirname "$0")/../.." && pwd)
cd "$top"
D=${2:-$top/build-signed-repo}
M=http://deb.debian.org/debian
UA=pkg-signature-test
mkdir -p "$D/d/dists/trixie/main/binary-riscv64"
if [ ! -s "$D/d/dists/trixie/InRelease" ] || [ ! -s "$D/d/dists/trixie/main/binary-riscv64/Packages.xz" ]; then
  curl -fsS -A "$UA" -o "$D/d/dists/trixie/InRelease" "$M/dists/trixie/InRelease"
  curl -fsS -A "$UA" -o "$D/d/dists/trixie/main/binary-riscv64/Packages.xz" "$M/dists/trixie/main/binary-riscv64/Packages.xz"
fi
sed -n '/^SHA256:/,/^[A-Z]/p' "$D/d/dists/trixie/InRelease" | grep -q "$(sha256sum "$D/d/dists/trixie/main/binary-riscv64/Packages.xz" | cut -d' ' -f1) .* main/binary-riscv64/Packages.xz$" \
  || { echo "Packages.xz does not match the InRelease fetched with it (the mirror changed in between): delete nothing, use a new DATA directory"; exit 1; }
# gawk and its libraries too: with both awks in the index, the solver installs gawk for
# base-files' Pre-Depends: awk even when mawk is named (apt would take mawk); noted for the package manager
for p in hello libc6 libgcc-s1 gcc-14-base base-files mawk busybox-static gawk libgmp10 libmpfr6 libreadline8t64 readline-common libtinfo6 libsigsegv2; do
  f=$(xz -dc "$D/d/dists/trixie/main/binary-riscv64/Packages.xz" | awk -v p="$p" '$1=="Package:"{n=$2} $1=="Filename:"&&n==p{print $2; exit}')
  [ -n "$f" ] || { echo "no $p in the index"; exit 1; }
  mkdir -p "$D/d/$(dirname "$f")"
  [ -s "$D/d/$f" ] || curl -fsS -A "$UA" -o "$D/d/$f" "$M/$f"
done
O=$top/$B-signed
ST=$O/stage/s
mkdir -p "$ST/keys" "$ST/b/dists/trixie" "$ST/n/dists/trixie"
command cp "$top"/keyrings/*.asc "$ST/keys/"
command cp -R "$D/d" "$ST/"
# a changed copy (one byte of the signed text) and one without its signature block
sed '0,/^Date:/s/^Date: \(.\)/Date: X\1/' "$D/d/dists/trixie/InRelease" > "$ST/b/dists/trixie/InRelease"
awk '/^-----BEGIN PGP SIGNATURE-----/{exit} {print}' "$D/d/dists/trixie/InRelease" > "$ST/n/dists/trixie/InRelease"
cat > "$ST/run.sh" <<'EOF'
echo "=== the shell: \$? after a command with redirections"
false > /dev/null; echo "false > /dev/null; then \$? is $?"
false; echo "false; then \$? is $?"
apt -o Dir=/nonexistent-root nosuchcommand > /dev/null 2> /s/probe; rc=$?; echo "a program exiting 100 with both outputs redirected, then rc=\$?: rc is $rc"
apt -o Dir=/nonexistent-root nosuchcommand > /dev/null 2>&1; echo "the same with 2>&1, then echo \$?: $?"
pass=0; fail=0
check() { if [ "$2" = "$3" ]; then pass=$((pass + 1)); echo "ok    $1"; else fail=$((fail + 1)); echo "FAIL  $1"; echo "      expected: [$2]"; echo "      got:      [$3]"; fi; }
yes_if() { if "$@" >/dev/null 2>&1; then echo yes; else echo no; fi; }
root() { mkdir -p /s/$1/etc/apt/trusted.gpg.d; [ "$2" = keys ] && cp /s/keys/*.asc /s/$1/etc/apt/trusted.gpg.d/; echo "deb file:/s/$3 trixie main" > /s/$1/etc/apt/sources.list; }
F=--force-unrunnable-scripts
root r keys d
echo "=== apt update (real InRelease, real Packages.xz, Debian's keys)"
rc=$(apt -o Dir=/s/r update > /s/u.out 2> /s/u.err; echo $?)
cat /s/u.out; cat /s/u.err
check "apt update" 0 $rc
check "  no warning or error" 0 "$(grep -c '^[WEN]:\|NOT checked' /s/u.err)"
check "  InRelease kept" yes "$(yes_if cat /s/r/var/lib/apt/lists/_s_d_dists_trixie_InRelease)"
echo "=== apt-keyring verify"
apt-keyring -o Dir=/s/r verify /s/d/dists/trixie/InRelease
echo "=== apt install -y --no-install-recommends hello base-files mawk busybox-static (mawk named: base-files pre-depends on awk, which gawk also provides)"
rc=$(apt -o Dir=/s/r -o DPkg::Options::=$F install -y --no-install-recommends hello base-files mawk busybox-static > /s/i.out 2>&1; echo $?)
cat /s/i.out
check "apt install -y hello" 0 $rc
check "  installed" installed "$(dpkg-query --root=/s/r -W -f '${db:Status-Status}' hello)"
check "  busybox-static installed" installed "$(dpkg-query --root=/s/r -W -f '${db:Status-Status}' busybox-static)"
echo "=== the engine: a statically linked program (busybox) from the package"
check "lx busybox echo" "signed and installed" "$(lx --root=/s/r /usr/bin/busybox echo signed and installed 2>&1)"
echo "=== the engine: hello (dynamically linked; its interpreter is /lib/ld-linux-riscv64-lp64d.so.1)"
lx --root=/s/r /usr/bin/hello > /s/h.out 2>&1; echo "exit status of lx /usr/bin/hello: $(lx --root=/s/r /usr/bin/hello >/dev/null 2>&1; echo $?)"
cat /s/h.out
echo "is /s/r/lib there? $(yes_if ls -d /s/r/lib)"; echo "is /s/r/usr/lib/ld-linux-riscv64-lp64d.so.1 there? $(yes_if ls /s/r/usr/lib/ld-linux-riscv64-lp64d.so.1)"
echo "=== refusals"
root rb keys b
rc=$(apt -o Dir=/s/rb update > /dev/null 2> /s/e1; echo $?)
cat /s/e1
check "changed InRelease refused" 100 $rc
check "  BADSIG" yes "$(yes_if grep -q 'signatures were invalid: BADSIG' /s/e1)"
root rn keys n
rc=$(apt -o Dir=/s/rn update > /dev/null 2> /s/e2; echo $?)
cat /s/e2
check "InRelease without its signature refused" 100 $rc
check "  as not signed" yes "$(yes_if grep -q 'is not signed' /s/e2)"
root rk nokeys d
rc=$(apt -o Dir=/s/rk update > /dev/null 2> /s/e3; echo $?)
cat /s/e3
check "no keys: refused" 100 $rc
check "  NO_PUBKEY" yes "$(yes_if grep -q 'NO_PUBKEY' /s/e3)"
root rx keys d
rc=$(apt -o Dir=/s/rx -o Debug::Acquire::Signature-Time=2208988800 update > /dev/null 2> /s/e4; echo $?)
cat /s/e4
check "clock in 2040 (keys expired): refused" 100 $rc
check "  EXPKEYSIG" yes "$(yes_if grep -q 'EXPKEYSIG' /s/e4)"
check "  no index stored" no "$(yes_if cat /s/rx/var/lib/apt/lists/_s_d_dists_trixie_main_binary-riscv64_Packages)"
echo "os-signed: $((pass + fail)) checks, $pass passed, $fail failed"
EOF
mkdir -p "$O"
command cp "$top/$B/kernel.wasm" "$O/"
command cp "$top/$B/initrd.tar" "$O/initrd.tar"
tar --format=ustar --owner=0 --group=0 -C "$O/stage" -rf "$O/initrd.tar" ./s
timeout 60 node tools/run.mjs --build="$B-signed" --init=/bin/sh --input='sh /s/run.sh\nexit\n' 2>&1 | tee "$O/os-signed.log"
grep -q 'os-signed: .* 0 failed' "$O/os-signed.log"
