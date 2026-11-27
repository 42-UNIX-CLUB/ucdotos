# SPDX-License-Identifier: GPL-2.0-or-later
# apt tests. Sourced by run.sh, which provides pkg/file/script/build, check and
# the variables W, BIN, P. Builds a small repository with a Release file and
# Packages indexes, then runs apt against a file: source.
echo "apt"

# The repositories are signed with a key made here by the host's gpg; each
# test root trusts it through etc/apt/trusted.gpg.d/test.gpg (see trust).
command -v gpg >/dev/null 2>&1 || { echo "FAIL: the apt tests need the host's gpg to sign test repositories"; fail=$((fail + 1)); }
GH=$W/gnupg; mkdir -p "$GH"; chmod 700 "$GH"
G() { GNUPGHOME=$GH gpg --batch --quiet --no-tty --pinentry-mode loopback --passphrase '' "$@"; }
G --quick-gen-key 'Test Archive <archive@example.invalid>' ed25519 sign never 2>/dev/null
G --quick-gen-key 'Other Archive <other@example.invalid>' ed25519 sign never 2>/dev/null
G --faked-system-time 20200101T000000 --quick-gen-key 'Old Archive <old@example.invalid>' ed25519 sign 1y 2>/dev/null
fpr() { G --with-colons --list-keys "$1" | awk -F: '$1=="fpr"{print $10; exit}'; }
TESTFPR=$(fpr archive@example.invalid); OTHERFPR=$(fpr other@example.invalid); OLDFPR=$(fpr old@example.invalid)
TESTKEY=$W/test-archive.gpg; G --export "$TESTFPR" > "$TESTKEY"
OTHERKEY=$W/other-archive.gpg; G --export "$OTHERFPR" > "$OTHERKEY"
G --export --armor "$TESTFPR" > "$W/test-archive.asc"
SIGNER=$TESTFPR
trust() { mkdir -p "$1/etc/apt/trusted.gpg.d"; cp "$TESTKEY" "$1/etc/apt/trusted.gpg.d/test.gpg"; }
# mkrepo DIR SUITE DEB...: a repository with pool/, Packages(.gz,.xz), Release, InRelease and Release.gpg (signed by $SIGNER)
mkrepo() {
  rd=$1 suite=$2; shift 2
  mkdir -p "$rd/pool" "$rd/dists/$suite/main/binary-riscv64" "$rd/dists/$suite/main/binary-all"
  pk=$rd/dists/$suite/main/binary-riscv64/Packages
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
    printf 'Architectures: riscv64\nComponents: main\nSHA256:\n'
    for f in main/binary-riscv64/Packages main/binary-riscv64/Packages.gz main/binary-riscv64/Packages.xz main/binary-all/Packages; do
      printf ' %s %s %s\n' "$(sha256sum "$rd/dists/$suite/$f" | cut -d' ' -f1)" "$(wc -c < "$rd/dists/$suite/$f" | tr -d ' ')" "$f"
    done
  } > "$rel"
  rm -f "$rd/dists/$suite/InRelease" "$rd/dists/$suite/Release.gpg"
  G ${GPGTIME:+--faked-system-time $GPGTIME} -u "$SIGNER!" --clearsign -o "$rd/dists/$suite/InRelease" "$rel" 2>/dev/null
  G ${GPGTIME:+--faked-system-time $GPGTIME} -u "$SIGNER!" --detach-sign -o "$rd/dists/$suite/Release.gpg" "$rel" 2>/dev/null
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
trust "$R"
echo "deb file:$REPO test main" > "$R/etc/apt/sources.list"
A() { "$BIN/apt" -o Dir="$R" -o DPkg::Options::=--force-script-chrootless "$@"; }
AG() { "$BIN/apt-get" -o Dir="$R" -o DPkg::Options::=--force-script-chrootless "$@"; }
AC() { "$BIN/apt-cache" -o Dir="$R" "$@"; }
LISTS=$R/var/lib/apt/lists
LNAME=$(echo "$REPO" | sed 's|/|_|g')

check "apt update" sh -c "'$BIN/apt' -o Dir='$R' update > '$W/upd.out' 2> '$W/upd.err'"
check "  the Packages index is stored under apt's name" exists "$LISTS/${LNAME}_dists_test_main_binary-riscv64_Packages"
check "  InRelease stored" exists "$LISTS/${LNAME}_dists_test_InRelease"
check "  no signature warning: the InRelease signature checked" hasnot "$W/upd.err" "NOT checked"
check "  and nothing else on stderr" sh -c "[ ! -s '$W/upd.err' ]"
check "  the index came from Packages.xz" grep -q "Packages.xz" "$W/upd.out"
check "  stored index equals the original" cmp "$LISTS/${LNAME}_dists_test_main_binary-riscv64_Packages" "$REPO/dists/test/main/binary-riscv64/Packages"
check "apt-cache policy" sh -c "AC() { '$BIN/apt-cache' -o Dir='$R' \"\$@\"; }; AC policy ra | grep -q 'Candidate: 1'"
check "apt show" sh -c "'$BIN/apt' -o Dir='$R' show ra | grep -q '^Depends: rb (>= 1)'"
check "apt-cache show prints the Filename" sh -c "'$BIN/apt-cache' -o Dir='$R' show ra | grep -q '^Filename: pool/ra_1_all.deb'"
check "apt search (description)" sh -c "'$BIN/apt' -o Dir='$R' search 'test package rprov' | grep -q '^rprov/'"
check "apt-cache search" sh -c "'$BIN/apt-cache' -o Dir='$R' search rvuser | grep -q '^rvuser - test package rvuser'"
check "apt list" sh -c "'$BIN/apt' -o Dir='$R' list 'r*' | grep -q '^ra/test/main riscv64 1 all'"
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
check "apt list --installed" sh -c "'$BIN/apt' -o Dir='$R' list --installed | grep -q '^ra/test/main riscv64 1 all \\[installed\\]'"
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
R2=$W/root-apt2; mkdir -p "$R2/etc/apt"; trust "$R2"
REPO2=$W/repo2
mkrepo "$REPO2" test "$RB"
printf 'garbage' >> "$REPO2/pool/rb_1_all.deb"
echo "deb file:$REPO2 test main" > "$R2/etc/apt/sources.list"
"$BIN/apt" -o Dir="$R2" update >/dev/null 2>&1
checknot "a .deb with the wrong checksum is refused" "$BIN/apt" -o Dir="$R2" install -y rb
check "  nothing installed" sh -c "! grep -q '^Package: rb' '$R2/var/lib/dpkg/status'"
# a damaged index
R3=$W/root-apt3; mkdir -p "$R3/etc/apt"; trust "$R3"
REPO3=$W/repo3
mkrepo "$REPO3" test "$RB"
printf 'Package: evil\nVersion: 1\n\n' >> "$REPO3/dists/test/main/binary-riscv64/Packages"
xz -dc "$REPO3/dists/test/main/binary-riscv64/Packages.xz" > "$W/p3"; printf 'Package: evil\nVersion: 1\n\n' >> "$W/p3"; xz -c "$W/p3" > "$REPO3/dists/test/main/binary-riscv64/Packages.xz"
gzip -9nc "$W/p3" > "$REPO3/dists/test/main/binary-riscv64/Packages.gz"
echo "deb file:$REPO3 test main" > "$R3/etc/apt/sources.list"
checknot "an index that does not match Release is refused" "$BIN/apt" -o Dir="$R3" update
check "  and not stored" sh -c "! ls '$R3/var/lib/apt/lists/' | grep -q binary-riscv64_Packages"
# an expired Release
R4=$W/root-apt4; mkdir -p "$R4/etc/apt"; trust "$R4"
REPO4=$W/repo4
VALID_UNTIL="Sat, 01 Jan 2000 00:00:00 UTC" mkrepo "$REPO4" test "$RB"
echo "deb file:$REPO4 test main" > "$R4/etc/apt/sources.list"
checknot "an expired Release file is refused" "$BIN/apt" -o Dir="$R4" update
check "  unless Check-Valid-Until is off" "$BIN/apt" -o Dir="$R4" -o Acquire::Check-Valid-Until=false update
# deb822 sources
R5=$W/root-apt5; mkdir -p "$R5/etc/apt/sources.list.d"; trust "$R5"
printf 'Types: deb\nURIs: file:%s\nSuites: test\nComponents: main\n\nTypes: deb\nURIs: file:/nonexistent\nSuites: test\nComponents: main\nEnabled: no\n' "$REPO" > "$R5/etc/apt/sources.list.d/test.sources"
check "deb822 .sources files" sh -c "'$BIN/apt' -o Dir='$R5' update 2>/dev/null && '$BIN/apt-cache' -o Dir='$R5' policy ra | grep -q 'Candidate: 2'"
# http through the configured command: a fake downloader that copies from the repository
mkdir -p "$W/fakebin"
printf '#!/bin/sh\n# fake curl: -fsSL -o OUT URL\nout=$3; url=$4; path=${url#http://example.invalid}\n[ -f "%s$path" ] || exit 22\ncp "%s$path" "$out"\n' "$REPO" "$REPO" > "$W/fakebin/curl"
chmod 755 "$W/fakebin/curl"
R6=$W/root-apt6; mkdir -p "$R6/etc/apt"; trust "$R6"
echo "deb http://example.invalid/ test main" > "$R6/etc/apt/sources.list"
check "http: goes through Acquire::http::Command" sh -c "PATH='$W/fakebin':\$PATH '$BIN/apt' -o Dir='$R6' update 2>/dev/null && PATH='$W/fakebin':\$PATH '$BIN/apt' -o Dir='$R6' -o DPkg::Options::=--force-script-chrootless install -y rb && grep -q '^Package: rb' '$R6/var/lib/dpkg/status'"
check "a missing download program is reported" sh -c "'$BIN/apt' -o Dir='$R6' -o 'Acquire::http::Command::=nosuchprogram %o %u' update 2>&1 | grep -q 'no download program is installed'"

# ---------- signatures (apt-secure(8)) ----------
echo "apt signatures"
# sigroot NAME [KEYFILE]: a fresh root, trusting KEYFILE (default: the test key; "none": nothing)
sigroot() {
  SR=$W/root-sig-$1; mkdir -p "$SR/etc/apt"
  case ${2:-} in none) ;; "") trust "$SR" ;; *) mkdir -p "$SR/etc/apt/trusted.gpg.d"; cp "$2" "$SR/etc/apt/trusted.gpg.d/k.gpg" ;; esac
}
SREPO=$W/repo-sig; mkrepo "$SREPO" test "$RB"
SL="file:$SREPO test main"
upd() { "$BIN/apt" -o Dir="$SR" "$@" update > "$W/sig.out" 2> "$W/sig.err"; }
sigroot good; echo "deb $SL" > "$SR/etc/apt/sources.list"
check "a signed repository updates" upd
check "  without any warning" sh -c "[ ! -s '$W/sig.err' ]"
check "  apt-keyring verify agrees" sh -c "'$BIN/apt-keyring' -o Dir='$SR' verify '$SREPO/dists/test/InRelease' | grep -q ': trusted'"
check "  apt-keyring list shows the key" sh -c "'$BIN/apt-keyring' -o Dir='$SR' list | grep -q 'Test Archive <archive@example.invalid>'"
check "  install works without --allow-unauthenticated" sh -c "'$BIN/apt' -o Dir='$SR' -o DPkg::Options::=--force-script-chrootless install -y rb > /dev/null 2>&1"
sigroot nokey none; echo "deb $SL" > "$SR/etc/apt/sources.list"
checknot "no trusted key: refused" upd
check "  apt's error text" grep -q "^E: The repository 'file:$SREPO test InRelease' is not signed.$" "$W/sig.err"
check "  NO_PUBKEY with the key ID" grep -q "public key is not available: NO_PUBKEY $(echo "$TESTFPR" | cut -c25-40)" "$W/sig.err"
check "  apt-secure(8) is named" grep -q "See apt-secure(8)" "$W/sig.err"
check "  no index stored" sh -c "! ls '$SR/var/lib/apt/lists/' 2>/dev/null | grep -q Packages"
check "  no InRelease stored" sh -c "! ls '$SR/var/lib/apt/lists/' 2>/dev/null | grep -q InRelease"
sigroot otherkey "$OTHERKEY"; echo "deb $SL" > "$SR/etc/apt/sources.list"
checknot "only another key trusted: refused" upd
# tampered InRelease: one byte of the signed text
TREPO=$W/repo-tamper; mkrepo "$TREPO" test "$RB"
sed -i 's/^Suite: test$/Suite: tesT/' "$TREPO/dists/test/InRelease"
sigroot tamper; echo "deb file:$TREPO test main" > "$SR/etc/apt/sources.list"
checknot "a changed InRelease is refused" upd
check "  BADSIG reported" grep -q "The following signatures were invalid: BADSIG" "$W/sig.err"
# text added after the signature
TREPO2=$W/repo-tamper2; mkrepo "$TREPO2" test "$RB"
printf 'Valid-Until: Sat, 01 Jan 2000 00:00:00 UTC\n' >> "$TREPO2/dists/test/InRelease"
sigroot after; echo "deb file:$TREPO2 test main" > "$SR/etc/apt/sources.list"
checknot "text after the signature: refused" upd
check "  said so" grep -q "text after the signature block" "$W/sig.err"
# text before the header
TREPO3=$W/repo-tamper3; mkrepo "$TREPO3" test "$RB"
{ printf 'Origin: Evil\n'; cat "$TREPO3/dists/test/InRelease"; } > "$W/inr" && cp "$W/inr" "$TREPO3/dists/test/InRelease"
sigroot before; echo "deb file:$TREPO3 test main" > "$SR/etc/apt/sources.list"
checknot "text before the signed message: refused" upd
# the signature block removed: an InRelease that only looks signed is not enough
TREPO4=$W/repo-tamper4; mkrepo "$TREPO4" test "$RB"
{ printf -- '-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\n'; cat "$TREPO4/dists/test/Release"; printf -- '-----BEGIN PGP SIGNATURE-----\n\nnot a real signature\n-----END PGP SIGNATURE-----\n'; } > "$TREPO4/dists/test/InRelease"
sigroot looks; echo "deb file:$TREPO4 test main" > "$SR/etc/apt/sources.list"
checknot "an InRelease with a fake signature block: refused" upd
check "  as not signed" grep -q "is not signed" "$W/sig.err"
# Release + Release.gpg when there is no InRelease
DREPO=$W/repo-detached; mkrepo "$DREPO" test "$RB"; rm -f "$DREPO/dists/test/InRelease"
sigroot detached; echo "deb file:$DREPO test main" > "$SR/etc/apt/sources.list"
check "Release + Release.gpg: accepted" upd
check "  without warnings" sh -c "[ ! -s '$W/sig.err' ]"
DN=$(echo "$DREPO" | sed 's|/|_|g')
check "  Release.gpg kept in lists/" exists "$SR/var/lib/apt/lists/${DN}_dists_test_Release.gpg"
printf 'X-Extra: 1\n' >> "$DREPO/dists/test/Release"
checknot "  a Release that no longer matches Release.gpg: refused" upd
# unsigned: Release only
UREPO=$W/repo-unsigned; mkrepo "$UREPO" test "$RB"; rm -f "$UREPO/dists/test/InRelease" "$UREPO/dists/test/Release.gpg"
sigroot unsigned; echo "deb file:$UREPO test main" > "$SR/etc/apt/sources.list"
checknot "an unsigned repository is refused" upd
check "  apt's error text names Release" grep -q "^E: The repository 'file:$UREPO test Release' is not signed.$" "$W/sig.err"
check "  --allow-insecure-repositories accepts it" upd --allow-insecure-repositories
check "  with apt's warning" grep -q "^W: The repository 'file:$UREPO test Release' is not signed.$" "$W/sig.err"
check "  and the loud one" grep -q "was NOT checked (--allow-insecure-repositories)" "$W/sig.err"
checknot "  install -y of an unauthenticated package fails" sh -c "'$BIN/apt' -o Dir='$SR' -o DPkg::Options::=--force-script-chrootless install -y rb > '$W/sig.out' 2> '$W/sig.err'"
check "  with apt's message" grep -q "There were unauthenticated packages and -y was used without --allow-unauthenticated" "$W/sig.err"
check "  and the warning list" grep -q "WARNING: The following packages cannot be authenticated!" "$W/sig.out"
check "  --allow-unauthenticated installs it" sh -c "'$BIN/apt' -o Dir='$SR' -o DPkg::Options::=--force-script-chrootless install -y --allow-unauthenticated rb > /dev/null 2>&1 && grep -q '^Package: rb' '$SR/var/lib/dpkg/status'"
checknot "  apt download refuses it too" sh -c "cd '$W' && '$BIN/apt' -o Dir='$SR' download rb 2>/dev/null"
sigroot trustedyes none; echo "deb [trusted=yes] file:$UREPO test main" > "$SR/etc/apt/sources.list"
check "[trusted=yes] accepts an unsigned repository" upd
check "  with the loud warning" grep -q "was NOT checked (the source is marked trusted=yes)" "$W/sig.err"
check "  and installs without --allow-unauthenticated" sh -c "'$BIN/apt' -o Dir='$SR' -o DPkg::Options::=--force-script-chrootless install -y rb > /dev/null 2>&1"
# downgrade: signed before, unsigned now
sigroot downgrade; echo "deb file:$UREPO test main" > "$SR/etc/apt/sources.list"
DGN=$(echo "$UREPO" | sed 's|/|_|g')
mkdir -p "$SR/var/lib/apt/lists"; cp "$SREPO/dists/test/InRelease" "$SR/var/lib/apt/lists/${DGN}_dists_test_InRelease"
checknot "a repository that was signed and is not any more: refused even with --allow-insecure-repositories" upd --allow-insecure-repositories
check "  as no longer signed" grep -q "is no longer signed" "$W/sig.err"
check "  --allow-downgrade-to-insecure-repositories accepts it" upd --allow-insecure-repositories --allow-downgrade-to-insecure-repositories
# Signed-By
sigroot signedby none; mkdir -p "$SR/keys"; cp "$TESTKEY" "$SR/keys/archive.gpg"
echo "deb [signed-by=/keys/archive.gpg] $SL" > "$SR/etc/apt/sources.list"
check "[signed-by=FILE] (under Dir) accepts" upd
echo "deb [signed-by=/keys/missing.gpg] $SL" > "$SR/etc/apt/sources.list"
checknot "[signed-by=] naming a missing file: refused" upd
check "  saying which file" grep -q "cannot read the keyring .*/keys/missing.gpg" "$W/sig.err"
sigroot signedby2; cp "$OTHERKEY" "$SR/etc/apt/trusted.gpg.d/other.gpg"
echo "deb [signed-by=$OTHERFPR] $SL" > "$SR/etc/apt/sources.list"
checknot "[signed-by=FINGERPRINT] of another trusted key: refused" upd
echo "deb [signed-by=$TESTFPR] $SL" > "$SR/etc/apt/sources.list"
check "[signed-by=FINGERPRINT] of the signing key: accepted" upd
sigroot deb822 none; mkdir -p "$SR/etc/apt/sources.list.d"
{ printf 'Types: deb\nURIs: file:%s\nSuites: test\nComponents: main\nSigned-By:\n' "$SREPO"; sed 's/^$/./; s/^/ /' "$W/test-archive.asc"; } > "$SR/etc/apt/sources.list.d/t.sources"
check "deb822 Signed-By with the key block written in the field" upd
printf 'Types: deb\nURIs: file:%s\nSuites: test\nComponents: main\nSigned-By: %s\n' "$SREPO" "$OTHERFPR" > "$SR/etc/apt/sources.list.d/t.sources"
checknot "deb822 Signed-By with a fingerprint that is not there: refused" upd
# expired key, through the test clock
OREPO=$W/repo-old; SIGNER=$OLDFPR; GPGTIME=20200601T000000; mkrepo "$OREPO" test "$RB"; SIGNER=$TESTFPR; GPGTIME=
sigroot old none; mkdir -p "$SR/etc/apt/trusted.gpg.d"; G --export "$OLDFPR" > "$SR/etc/apt/trusted.gpg.d/old.gpg"
echo "deb file:$OREPO test main" > "$SR/etc/apt/sources.list"
checknot "a repository signed by an expired key: refused" upd
check "  EXPKEYSIG reported" grep -q "EXPKEYSIG" "$W/sig.err"
check "  accepted with the clock set inside the key's lifetime" upd -o Debug::Acquire::Signature-Time=1593600000
# stop the gpg-agent the signing above started
command -v gpgconf >/dev/null 2>&1 && gpgconf --homedir "$GH" --kill gpg-agent 2>/dev/null
