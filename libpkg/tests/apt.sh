# SPDX-License-Identifier: GPL-2.0-or-later
# apt tests. Sourced by run.sh, which provides pkg/file/script/build, check and
# the variables W, BIN, P. Builds a small repository with a Release file and
# Packages indexes, then runs apt against a file: source.
echo "apt"

# mkrepo DIR SUITE DEB...: a repository with pool/, Packages(.gz,.xz) and Release (+ a fake-signed InRelease)
mkrepo() {
  rd=$1 suite=$2; shift 2
  mkdir -p "$rd/pool" "$rd/dists/$suite/main/binary-amd64" "$rd/dists/$suite/main/binary-all"
  pk=$rd/dists/$suite/main/binary-amd64/Packages
  : > "$pk"
  for deb in "$@"; do
    cp "$deb" "$rd/pool/"
    b=$(basename "$deb")
    "$BIN/dpkg-deb" -f "$deb" >> "$pk"
    printf 'Filename: pool/%s\nSize: %s\nSHA256: %s\n\n' "$b" "$(wc -c < "$deb" | tr -d ' ')" "$(sha256sum "$deb" | cut -d' ' -f1)" >> "$pk"
  done
  : > "$rd/dists/$suite/main/binary-all/Packages"
  rm -f "$pk.gz" "$pk.xz"
  gzip -9nk "$pk"; xz -k "$pk"
  rel=$rd/dists/$suite/Release
  {
    printf 'Origin: Test\nLabel: Test\nSuite: %s\nCodename: %s\nDate: Sat, 21 Nov 2026 09:15:44 UTC\n' "$suite" "$suite"
    [ -n "${VALID_UNTIL:-}" ] && printf 'Valid-Until: %s\n' "$VALID_UNTIL"
    printf 'Architectures: amd64\nComponents: main\nSHA256:\n'
    for f in main/binary-amd64/Packages main/binary-amd64/Packages.gz main/binary-amd64/Packages.xz main/binary-all/Packages; do
      printf ' %s %s %s\n' "$(sha256sum "$rd/dists/$suite/$f" | cut -d' ' -f1)" "$(wc -c < "$rd/dists/$suite/$f" | tr -d ' ')" "$f"
    done
  } > "$rel"
  { printf -- '-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\n'; cat "$rel"; printf -- '-----BEGIN PGP SIGNATURE-----\n\nnot a real signature\n-----END PGP SIGNATURE-----\n'; } > "$rd/dists/$suite/InRelease"
}

pkg ra 1 all "Depends: rb (>= 1)" "Recommends: rrec" "Suggests: rsug"; file usr/share/ra/f 'ra 1'; build; RA1=$DEB
pkg ra 2 all "Depends: rb (>= 1)" "Recommends: rrec"; file usr/share/ra/f 'ra 2'; build; RA2=$DEB
pkg rb 1 all; file usr/share/rb/f 'rb'; build; RB=$DEB
pkg rrec 1 all; file usr/share/rrec/f 'rec'; build; RREC=$DEB
pkg rprov 1 all "Provides: rvirt"; file usr/share/rprov/f 'p'; build; RPROV=$DEB
pkg rvuser 1 all "Depends: rvirt"; file usr/share/rvuser/f 'u'; build; RVUSER=$DEB
pkg rx 1 all "Conflicts: ra"; file usr/share/rx/f 'x'; build; RX=$DEB
pkg rimp 1 all "Depends: nosuchpackage"; file usr/share/rimp/f 'i'; build; RIMP=$DEB
pkg rconf 1 all; file etc/rconf.conf 'a=1'; ctlfile conffiles '/etc/rconf.conf'; build; RCONF=$DEB
REPO=$W/repo
mkrepo "$REPO" test "$RA1" "$RB" "$RREC" "$RPROV" "$RVUSER" "$RX" "$RIMP" "$RCONF"

R=$W/root-apt
mkdir -p "$R/etc/apt"
echo "deb file:$REPO test main" > "$R/etc/apt/sources.list"
A() { "$BIN/apt" -o Dir="$R" -o DPkg::Options::=--force-script-chrootless "$@"; }
AG() { "$BIN/apt-get" -o Dir="$R" -o DPkg::Options::=--force-script-chrootless "$@"; }
AC() { "$BIN/apt-cache" -o Dir="$R" "$@"; }
LISTS=$R/var/lib/apt/lists
LNAME=$(echo "$REPO" | sed 's|/|_|g')

check "apt update" sh -c "'$BIN/apt' -o Dir='$R' update > '$W/upd.out' 2> '$W/upd.err'"
check "  the Packages index is stored under apt's name" exists "$LISTS/${LNAME}_dists_test_main_binary-amd64_Packages"
check "  InRelease stored" exists "$LISTS/${LNAME}_dists_test_InRelease"
check "  the missing signature check is reported" grep -q "signature of .* was NOT checked" "$W/upd.err"
check "  the index came from Packages.xz" grep -q "Packages.xz" "$W/upd.out"
check "  stored index equals the original" cmp "$LISTS/${LNAME}_dists_test_main_binary-amd64_Packages" "$REPO/dists/test/main/binary-amd64/Packages"
check "a quiet update still warns about signatures" sh -c "'$BIN/apt' -o Dir='$R' -qq update 2>&1 | grep -q 'NOT checked'"
check "apt-cache policy" sh -c "AC() { '$BIN/apt-cache' -o Dir='$R' \"\$@\"; }; AC policy ra | grep -q 'Candidate: 1'"
check "apt show" sh -c "'$BIN/apt' -o Dir='$R' show ra | grep -q '^Depends: rb (>= 1)'"
check "apt-cache show prints the Filename" sh -c "'$BIN/apt-cache' -o Dir='$R' show ra | grep -q '^Filename: pool/ra_1_all.deb'"
check "apt search (description)" sh -c "'$BIN/apt' -o Dir='$R' search 'test package rprov' | grep -q '^rprov/'"
check "apt-cache search" sh -c "'$BIN/apt-cache' -o Dir='$R' search rvuser | grep -q '^rvuser - test package rvuser'"
check "apt list" sh -c "'$BIN/apt' -o Dir='$R' list 'r*' | grep -q '^ra/test/main amd64 1 all'"
check "apt depends" sh -c "'$BIN/apt' -o Dir='$R' depends ra | grep -q 'Depends: rb (>= 1)'"
check "apt rdepends" sh -c "'$BIN/apt' -o Dir='$R' rdepends rb | grep -q '^  ra$'"
check "apt-cache stats" sh -c "'$BIN/apt-cache' -o Dir='$R' stats | grep -q 'Total distinct versions: 8'"
check "simulate an install" sh -c "'$BIN/apt-get' -o Dir='$R' -s install ra > '$W/sim.out' && grep -q '^Inst rb ' '$W/sim.out' && grep -q '^Inst rrec ' '$W/sim.out' && grep -q '^Conf ra ' '$W/sim.out'"
check "  rb is configured before ra" sh -c "[ \$(grep -n '^Conf rb' '$W/sim.out' | cut -d: -f1) -lt \$(grep -n '^Inst ra' '$W/sim.out' | cut -d: -f1) ]"
check "  simulation changed nothing" missing "$R/var/lib/dpkg/info/ra.list"
check "--no-install-recommends leaves rrec out" sh -c "'$BIN/apt-get' -o Dir='$R' -s --no-install-recommends install ra | grep -v '^Recommended' | grep -q '^Inst rrec' && exit 1 || exit 0"
check "a virtual package is resolved through its provider" sh -c "'$BIN/apt-get' -o Dir='$R' -s install rvuser | grep -q '^Inst rprov '"
checknot "an unsatisfiable request fails" AG -s install rimp
check "  and says why" sh -c "'$BIN/apt-get' -o Dir='$R' -s install rimp 2>&1 | grep -q 'rimp 1 cannot be installed'"
checknot "an unknown package fails" AG -s install nosuch
check "apt install -y ra" A install -y ra
check "  ra installed" st "$R" ra "install ok installed"
check "  rb installed" st "$R" rb "install ok installed"
check "  recommended rrec installed" st "$R" rrec "install ok installed"
check "  the .deb was cached" exists "$R/var/cache/apt/archives/ra_1_all.deb"
check "  rb marked automatic" sh -c "grep -A2 '^Package: rb' '$R/var/lib/apt/extended_states' | grep -q 'Auto-Installed: 1'"
checknot "  ra not marked automatic" sh -c "grep -A2 '^Package: ra$' '$R/var/lib/apt/extended_states' | grep -q 'Auto-Installed: 1'"
check "installing again says it is the newest version" sh -c "'$BIN/apt' -o Dir='$R' install -y ra | grep -q 'already the newest version'"
check "apt list --installed" sh -c "'$BIN/apt' -o Dir='$R' list --installed | grep -q '^ra/test/main amd64 1 all \\[installed\\]'"
# a new version arrives
mkrepo "$REPO" test "$RA2" "$RB" "$RREC" "$RPROV" "$RVUSER" "$RX" "$RIMP" "$RCONF"
check "update sees the upgrade" sh -c "'$BIN/apt' -o Dir='$R' update 2>/dev/null | grep -q '1 package can be upgraded'"
check "apt list --upgradable" sh -c "'$BIN/apt' -o Dir='$R' list --upgradable | grep -q 'upgradable to: 2'"
check "apt upgrade -y" A upgrade -y
check "  ra is version 2" sh -c "'$BIN/dpkg' --root='$R' -s ra | grep -q '^Version: 2$'"
check "  new content" same "$R/usr/share/ra/f" "ra 2"
check "a conflicting package removes ra (with -y)" A install -y rx
check "  ra removed" st "$R" ra "deinstall ok not-installed"
check "  rx installed" st "$R" rx "install ok installed"
check "autoremove offers rb and rrec" sh -c "'$BIN/apt-get' -o Dir='$R' -s autoremove | grep -q '^Remv rb'"
check "apt autoremove -y" A autoremove -y
check "  rb gone" st "$R" rb "deinstall ok not-installed"
check "apt install rconf, then purge" sh -c "'$BIN/apt' -o Dir='$R' -o DPkg::Options::=--force-script-chrootless install -y rconf && '$BIN/apt' -o Dir='$R' purge -y rconf"
check "  conffile gone after purge" missing "$R/etc/rconf.conf"
check "apt download" sh -c "cd '$W' && '$BIN/apt' -o Dir='$R' download rb && [ -s '$W/rb_1_all.deb' ]"
# a broken half-done install: dpkg -i without the dependency, then apt -f install
check "remove rx and rb first" sh -c "'$BIN/dpkg' --root='$R' -r rx rb"
check "dpkg -i ra without rb leaves it unpacked" sh -c "'$BIN/dpkg' --root='$R' --force-script-chrootless -i '$RA2'; true"
check "  unpacked" st "$R" ra "install ok unpacked"
check "apt -f install repairs it" A -f install -y
check "  rb installed" st "$R" rb "install ok installed"
check "  ra configured" st "$R" ra "install ok installed"
# a damaged download
R2=$W/root-apt2; mkdir -p "$R2/etc/apt"
REPO2=$W/repo2
mkrepo "$REPO2" test "$RB"
printf 'garbage' >> "$REPO2/pool/rb_1_all.deb"
echo "deb file:$REPO2 test main" > "$R2/etc/apt/sources.list"
"$BIN/apt" -o Dir="$R2" update >/dev/null 2>&1
checknot "a .deb with the wrong checksum is refused" "$BIN/apt" -o Dir="$R2" install -y rb
check "  nothing installed" sh -c "! grep -q '^Package: rb' '$R2/var/lib/dpkg/status'"
# a damaged index
R3=$W/root-apt3; mkdir -p "$R3/etc/apt"
REPO3=$W/repo3
mkrepo "$REPO3" test "$RB"
printf 'Package: evil\nVersion: 1\n\n' >> "$REPO3/dists/test/main/binary-amd64/Packages"
xz -dc "$REPO3/dists/test/main/binary-amd64/Packages.xz" > "$W/p3"; printf 'Package: evil\nVersion: 1\n\n' >> "$W/p3"; xz -c "$W/p3" > "$REPO3/dists/test/main/binary-amd64/Packages.xz"
gzip -9nc "$W/p3" > "$REPO3/dists/test/main/binary-amd64/Packages.gz"
echo "deb file:$REPO3 test main" > "$R3/etc/apt/sources.list"
checknot "an index that does not match Release is refused" "$BIN/apt" -o Dir="$R3" update
check "  and not stored" sh -c "! ls '$R3/var/lib/apt/lists/' | grep -q binary-amd64_Packages"
# an expired Release
R4=$W/root-apt4; mkdir -p "$R4/etc/apt"
REPO4=$W/repo4
VALID_UNTIL="Sat, 01 Jan 2000 00:00:00 UTC" mkrepo "$REPO4" test "$RB"
echo "deb file:$REPO4 test main" > "$R4/etc/apt/sources.list"
checknot "an expired Release file is refused" "$BIN/apt" -o Dir="$R4" update
check "  unless Check-Valid-Until is off" "$BIN/apt" -o Dir="$R4" -o Acquire::Check-Valid-Until=false update
# deb822 sources
R5=$W/root-apt5; mkdir -p "$R5/etc/apt/sources.list.d"
printf 'Types: deb\nURIs: file:%s\nSuites: test\nComponents: main\n\nTypes: deb\nURIs: file:/nonexistent\nSuites: test\nComponents: main\nEnabled: no\n' "$REPO" > "$R5/etc/apt/sources.list.d/test.sources"
check "deb822 .sources files" sh -c "'$BIN/apt' -o Dir='$R5' update 2>/dev/null && '$BIN/apt-cache' -o Dir='$R5' policy ra | grep -q 'Candidate: 2'"
# http through the configured command: a fake downloader that copies from the repository
mkdir -p "$W/fakebin"
printf '#!/bin/sh\n# fake curl: -fsSL -o OUT URL\nout=$3; url=$4; path=${url#http://example.invalid}\n[ -f "%s$path" ] || exit 22\ncp "%s$path" "$out"\n' "$REPO" "$REPO" > "$W/fakebin/curl"
chmod 755 "$W/fakebin/curl"
R6=$W/root-apt6; mkdir -p "$R6/etc/apt"
echo "deb http://example.invalid/ test main" > "$R6/etc/apt/sources.list"
check "http: goes through Acquire::http::Command" sh -c "PATH='$W/fakebin':\$PATH '$BIN/apt' -o Dir='$R6' update 2>/dev/null && PATH='$W/fakebin':\$PATH '$BIN/apt' -o Dir='$R6' -o DPkg::Options::=--force-script-chrootless install -y rb && grep -q '^Package: rb' '$R6/var/lib/dpkg/status'"
check "a missing download program is reported" sh -c "'$BIN/apt' -o Dir='$R6' -o 'Acquire::http::Command::=nosuchprogram %o %u' update 2>&1 | grep -q 'no download program is installed'"
