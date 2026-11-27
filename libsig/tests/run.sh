#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Native tests for libsig (OpenPGP signature verification).
#   sh libsig/tests/run.sh [OUTDIR]          (default build-sig/, git-ignored)
# Builds sigtool with AddressSanitizer and UBSan, then runs, each when what it
# needs is there (otherwise it says SKIP):
#   1. vectors.py: published vectors (needs libsig/tests/fetch-vectors.sh first)
#   2. pgpcases.py: crafted keys, signatures and cleartext attacks (needs
#      Python's 'cryptography'; with the host's gpgv, files meant to be valid
#      are also checked by gpgv)
#   3. GnuPG interop: keys and signatures made by the host's gpg
#   4. real archives: the InRelease, Release and Release.gpg files fetched by
#      libpkg/tests/fetch-signed.sh, against the keyrings fetched by
#      libpkg/tests/fetch-keyrings.sh, checked by sigtool and by gpgv, plus
#      tampered copies.
# Exits 1 if anything fails.
set -u
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-sig}
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
export TMPDIR="$out"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer}
$CC $CFLAGS -o "$out/sigtool" "$top/libsig/tests/sigtool.c" "$top"/libsig/*.c "$top/libarc/hash.c" || { echo "build failed"; exit 1; }
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
T=$out/sigtool
fail=0 pass=0
ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }
# expect DESCRIPTION EXITCODE PATTERN command...: the command's exit code and output
expect() {
  e_d=$1 e_code=$2 e_pat=$3; shift 3
  e_o=$("$@" 2>&1); e_rc=$?
  if [ "$e_rc" = "$e_code" ] && printf '%s\n' "$e_o" | grep -q -e "$e_pat" && ! printf '%s\n' "$e_o" | grep -q 'AddressSanitizer\|runtime error'; then ok
  else bad "$e_d: want exit $e_code and /$e_pat/, got exit $e_rc: $(printf '%s' "$e_o" | head -5)"; fi
}

# ---- 1 and 2
VEC=${SIG_VECTORS:-$top/build-sigvec}
if command -v python3 >/dev/null 2>&1 && [ -d "$VEC" ]; then
  python3 -I "$top/libsig/tests/vectors.py" "$T" "$VEC" "${SIG_DIFF_SECONDS:-20}" > "$out/vectors.log" 2>&1
  r=$?; tail -1 "$out/vectors.log"
  [ $r = 0 ] && ok || { bad "vectors.py (see $out/vectors.log)"; grep FAIL "$out/vectors.log" | head; }
else echo "SKIP vectors: run sh libsig/tests/fetch-vectors.sh first (or set SIG_VECTORS)"; fi
if command -v python3 >/dev/null 2>&1 && python3 -c 'import cryptography' 2>/dev/null; then
  G=$out/gnupg-cases; mkdir -p "$G"; chmod 700 "$G"
  GV=$(command -v gpgv || true)
  GNUPGHOME=$G python3 -I "$top/libsig/tests/pgpcases.py" "$T" "$out" $GV > "$out/pgpcases.log" 2>&1
  r=$?; tail -1 "$out/pgpcases.log"
  [ $r = 0 ] && ok || { bad "pgpcases.py (see $out/pgpcases.log)"; grep FAIL "$out/pgpcases.log" | head; }
else echo "SKIP pgpcases: needs python3 with the 'cryptography' package"; fi

# ---- RFC 9580 appendix A: a version 6 certificate and cleartext message are refused cleanly
if [ -f "$VEC/rfc9580.txt" ]; then
  awk '/^A.3.  Sample Version 6 Certificate/{a=1} a&&/-----BEGIN PGP PUBLIC KEY BLOCK-----/{p=1} p{sub(/^   /,""); print} p&&/-----END PGP PUBLIC KEY BLOCK-----/{exit}' "$VEC/rfc9580.txt" > "$out/rfc9580-v6.asc"
  awk '/^A.6.  Sample Cleartext Signed Message/{a=1} a&&/-----BEGIN PGP SIGNED MESSAGE-----/{p=1} p{sub(/^   /,""); print} p&&/-----END PGP SIGNATURE-----/{exit}' "$VEC/rfc9580.txt" > "$out/rfc9580-v6-clear.asc"
  expect "RFC 9580 A.3 v6 certificate is listed as unsupported" 0 'version 6 keys are not supported' "$T" keys "$out/rfc9580-v6.asc"
  expect "RFC 9580 A.6 v6 cleartext message: unsupported, not trusted" 1 'UNSUPPORTED.*version 6' "$T" clear "$out/rfc9580-v6-clear.asc" "$out/rfc9580-v6.asc"
  o=$("$T" text "$out/rfc9580-v6-clear.asc")
  [ "$o" = "$(printf 'What we need from the grocery store:\n\n- tofu\n- vegetables\n- noodles\n')" ] && ok || bad "RFC 9580 A.6 text (dash escapes): $o"
fi

# ---- 3: GnuPG interop
if command -v gpg >/dev/null 2>&1; then
  G=$out/gnupg-$(date +%Y%m%d%H%M%S)-$$
  mkdir -p "$G"; chmod 700 "$G"
  I=$G/files; mkdir -p "$I"
  g() { GNUPGHOME=$G gpg --batch --quiet --no-tty --pinentry-mode loopback --passphrase '' "$@"; }
  fpr_of() { g --with-colons --list-keys "$1" | awk -F: '$1=="fpr"{print $10; exit}'; }
  subfpr_of() { g --with-colons --list-keys "$1" | awk -F: '$1=="fpr"{n++} $1=="fpr"&&n==2{print $10; exit}'; }
  printf 'Origin: Interop\nSuite: test\n- dash line\n  trailing  \nSHA256:\n abc 1 main/Packages\n' > "$I/text"
  g --quick-gen-key 'Interop RSA <rsa@example.invalid>' rsa3072 sign never
  g --quick-gen-key 'Interop Ed <ed@example.invalid>' ed25519 sign never
  RSA=$(fpr_of rsa@example.invalid); ED=$(fpr_of ed@example.invalid)
  g --quick-add-key "$ED" rsa3072 sign never
  EDSUB=$(subfpr_of ed@example.invalid)
  g --export "$RSA" "$ED" > "$I/kr.gpg"
  g --export --armor "$RSA" "$ED" > "$I/kr.asc"
  for who in "$RSA" "$ED" "$EDSUB!"; do
    for h in SHA256 SHA384 SHA512; do
      f=$I/clear-$(echo "$who" | cut -c1-8)-$h.asc
      g -u "$who" --digest-algo $h --clearsign -o "$f" "$I/text"
      expect "gpg clearsign by ${who} $h (binary keyring)" 0 GOODSIG "$T" clear "$f" "$I/kr.gpg"
      expect "gpg clearsign by ${who} $h (armored keyring)" 0 GOODSIG "$T" clear "$f" "$I/kr.asc"
    done
    f=$I/det-$(echo "$who" | cut -c1-8)
    g -u "$who" --detach-sign -o "$f.sig" "$I/text"
    g -u "$who" --armor --detach-sign -o "$f.asc" "$I/text"
    expect "gpg detached binary by $who" 0 GOODSIG "$T" detached "$I/text" "$f.sig" "$I/kr.gpg"
    expect "gpg detached armored by $who" 0 GOODSIG "$T" detached "$I/text" "$f.asc" "$I/kr.gpg"
  done
  o=$("$T" text "$I/clear-$(echo "$RSA" | cut -c1-8)-SHA256.asc")
  [ "$o" = "$(sed 's/[ \t]*$//' "$I/text")" ] && ok || bad "the signed text of a gpg clearsigned file: $o"
  g -u "$RSA" --clearsign -o "$I/two.asc" --digest-algo SHA256 -u "$ED" "$I/text"
  expect "gpg clearsign with two keys" 0 '(2 good, 0 bad)' "$T" clear "$I/two.asc" "$I/kr.gpg"
  g --export "$RSA" > "$I/rsa-only.gpg"
  expect "two signatures, one key in the keyring" 0 'NO_PUBKEY' "$T" clear "$I/two.asc" "$I/rsa-only.gpg"
  # an expired key: made and used in 2020, valid for one day
  g --faked-system-time 20200101T000000 --quick-gen-key 'Interop Old <old@example.invalid>' ed25519 sign 1d
  OLD=$(fpr_of old@example.invalid)
  g --faked-system-time 20200101T010000 -u "$OLD" --clearsign -o "$I/old.asc" "$I/text"
  g --export "$OLD" > "$I/old.gpg"
  expect "gpg key that expired" 1 EXPKEYSIG "$T" clear "$I/old.asc" "$I/old.gpg"
  expect "gpg key before it expired (test clock)" 0 GOODSIG "$T" clear -t 1577840400 "$I/old.asc" "$I/old.gpg"
  # a revoked key: import the revocation certificate gpg wrote when it made the key
  g --quick-gen-key 'Interop Revoked <rev@example.invalid>' ed25519 sign never
  REV=$(fpr_of rev@example.invalid)
  g -u "$REV" --clearsign -o "$I/rev.asc" "$I/text"
  g --export "$REV" > "$I/rev-before.gpg"
  sed 's/^:-----BEGIN/-----BEGIN/' "$G/openpgp-revocs.d/$REV.rev" > "$I/rev.cert"
  g --import "$I/rev.cert" 2>/dev/null
  g --export "$REV" > "$I/rev.gpg"
  expect "gpg key before its revocation" 0 GOODSIG "$T" clear "$I/rev.asc" "$I/rev-before.gpg"
  expect "gpg revoked key" 1 REVKEYSIG "$T" clear "$I/rev.asc" "$I/rev.gpg"
  # a revoked signing subkey
  g --quick-gen-key 'Interop Subrev <subrev@example.invalid>' ed25519 cert never
  SR=$(fpr_of subrev@example.invalid)
  g --quick-add-key "$SR" ed25519 sign never
  SRSUB=$(subfpr_of subrev@example.invalid)
  g -u "$SRSUB!" --clearsign -o "$I/subrev.asc" "$I/text"
  g --export "$SR" > "$I/subrev-before.gpg"
  printf 'key 1\nrevkey\ny\n0\n\ny\nsave\n' | GNUPGHOME=$G gpg --batch --no-tty --pinentry-mode loopback --passphrase '' --command-fd 0 --edit-key "$SR" >/dev/null 2>&1
  g --export "$SR" > "$I/subrev.gpg"
  expect "gpg signing subkey before its revocation" 0 GOODSIG "$T" clear "$I/subrev.asc" "$I/subrev-before.gpg"
  # (gpg refuses to sign data with a certify-only primary key; pgpcases.py covers that case)
  expect "gpg revoked signing subkey" 1 REVKEYSIG "$T" clear "$I/subrev.asc" "$I/subrev.gpg"
  # keys with a subkey that does not sign
  g --quick-gen-key 'Interop Enc <enc@example.invalid>' ed25519 sign never
  ENC=$(fpr_of enc@example.invalid)
  g --quick-add-key "$ENC" cv25519 encr never
  g --export "$ENC" > "$I/enc.gpg"
  expect "a key with an encryption subkey lists both" 0 'sub .*can_sign=0' "$T" keys "$I/enc.gpg"
  command -v gpgconf >/dev/null 2>&1 && gpgconf --homedir "$G" --kill gpg-agent 2>/dev/null   # stop the agent gpg started
else echo "SKIP GnuPG interop: no gpg on this host"; fi

# ---- 4: real archives
SIGNED=${SIG_SIGNED:-$top/build-signed}
KEYS=${SIG_KEYRINGS:-$top/build-keyrings}
DK=$(find "$KEYS" -path '*/usr/share/keyrings/debian-archive-keyring.pgp' 2>/dev/null | head -1)
UK=$(find "$KEYS" -path '*/usr/share/keyrings/ubuntu-archive-keyring.gpg' 2>/dev/null | head -1)
if [ -d "$SIGNED" ] && [ -n "$DK" ] && [ -n "$UK" ]; then
  R=$out/real; mkdir -p "$R"
  GV=$(command -v gpgv || true)
  for d in "$SIGNED"/*/; do
    n=$(basename "$d")
    case $n in debian*) K=$DK; OTHERK=$UK ;; ubuntu*) K=$UK; OTHERK=$DK ;; *) continue ;; esac
    [ -f "$d/InRelease" ] || continue
    expect "$n InRelease" 0 TRUSTED "$T" clear "$d/InRelease" "$K"
    "$T" clear "$d/InRelease" "$K" | grep -v TRUSTED | sed "s|^|  $n: |"
    if [ -n "$GV" ]; then
      "$GV" --keyring "$K" "$d/InRelease" >/dev/null 2>&1 && ok || bad "$n: gpgv refuses InRelease"
    fi
    if [ -f "$d/Release.gpg" ]; then
      # A mirror can change between the downloads of InRelease, Release and
      # Release.gpg. When gpgv refuses the pair too, the two verdicts must agree.
      if [ -z "$GV" ] || "$GV" --keyring "$K" "$d/Release.gpg" "$d/Release" >/dev/null 2>&1; then
        expect "$n Release + Release.gpg" 0 TRUSTED "$T" detached "$d/Release" "$d/Release.gpg" "$K"
        o=$("$T" text "$d/InRelease" | cmp - "$d/Release" 2>&1) && ok || echo "  note: $n: InRelease and Release differ (the mirror changed between the downloads)"
      else
        echo "  note: $n: gpgv refuses Release.gpg too (the mirror changed between the downloads)"
        expect "$n Release + Release.gpg refused like gpgv" 1 BADSIG "$T" detached "$d/Release" "$d/Release.gpg" "$K"
      fi
    fi
    # tampered copies
    sed '0,/^Date:/s/^Date: \(.\)/Date: X\1/' "$d/InRelease" > "$R/$n.body"
    expect "$n: one byte changed in the body" 1 BADSIG "$T" clear "$R/$n.body" "$K"
    awk 'BEGIN{s=0} /BEGIN PGP SIGNATURE/{s=1} s&&length($0)>=60&&/^[A-Za-z0-9+\/]+$/&&!done{c=substr($0,30,1); r=(c=="A")?"B":"A"; $0=substr($0,1,29) r substr($0,31); done=1} {print}' "$d/InRelease" > "$R/$n.sig"
    o=$("$T" clear "$R/$n.sig" "$K" 2>&1); rc=$?
    { [ $rc = 1 ] && printf '%s' "$o" | grep -q 'BADSIG\|NOT TRUSTED'; } || { [ $rc = 2 ] && printf '%s' "$o" | grep -q MALFORMED; } && ok || bad "$n: one character changed in the signature: exit $rc $o"
    awk '/BEGIN PGP SIGNATURE/{exit} {print}' "$d/InRelease" > "$R/$n.nosig"
    expect "$n: signature removed" 2 MALFORMED "$T" clear "$R/$n.nosig" "$K"
    expect "$n: key not in the keyring" 1 NO_PUBKEY "$T" clear "$d/InRelease" "$OTHERK"
    case $n in debian*) expect "$n: every key expired (clock in 2040)" 1 EXPKEYSIG "$T" clear -t 2208988800 "$d/InRelease" "$K" ;; esac  # Ubuntu's 2018 key has no expiry date
    head -c 1000 "$d/InRelease" > "$R/$n.trunc"
    expect "$n: truncated" 2 MALFORMED "$T" clear "$R/$n.trunc" "$K"
    if [ -n "$GV" ]; then
      for x in body nosig trunc; do "$GV" --keyring "$K" "$R/$n.$x" >/dev/null 2>&1 && bad "$n: gpgv accepts the $x copy" || ok; done
      "$GV" --keyring "$OTHERK" "$d/InRelease" >/dev/null 2>&1 && bad "$n: gpgv accepts with the other keyring" || ok
    fi
  done
  # the Ubuntu 2012 key (SHA-1 self-signature) is listed and refused
  expect "Ubuntu 2012 key refused for its SHA-1 self-signature" 0 '3B4FE6ACC0B21F32.*SHA-1\|790BC7277767219C42C86F933B4FE6ACC0B21F32.*SHA-1' "$T" keys "$UK"
else echo "SKIP real archives: run libpkg/tests/fetch-signed.sh and libpkg/tests/fetch-keyrings.sh first"; fi

echo "libsig: $pass passed, $fail failed"
[ $fail = 0 ]
