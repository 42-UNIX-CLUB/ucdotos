#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch the published test vectors the libsig tests read. Nothing is copied
# into the tree: they go to OUT (default build-sigvec/, git-ignored).
#   RFC 8032 (Ed25519 test vectors in section 7.1)
#   RFC 9580 (OpenPGP samples in appendix A: version 4 Ed25519Legacy key and
#     signature, version 6 certificate and cleartext signed message)
#   ed25519-speccheck cases.json ("Taming the many EdDSAs", Chalkias, Garillot
#     and Nikolaenko, 2020: small-order points, non-canonical encodings)
#   Project Wycheproof (C2SP): RSA PKCS#1 v1.5 signature tests for 2048, 3072
#     and 4096-bit keys with SHA-256 and SHA-512 (they include padding
#     forgeries), and the Ed25519 tests.
#   sh libsig/tests/fetch-vectors.sh [OUT]
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$top/build-sigvec}
mkdir -p "$out"
UA="sig-test-vectors"
get() { [ -s "$out/$2" ] || curl -fsS -A "$UA" -o "$out/$2" "$1"; echo "$2: $(wc -c < "$out/$2") bytes"; }
get https://www.rfc-editor.org/rfc/rfc8032.txt rfc8032.txt
get https://www.rfc-editor.org/rfc/rfc9580.txt rfc9580.txt
get https://raw.githubusercontent.com/novifinancial/ed25519-speccheck/main/cases.json speccheck-cases.json
W=https://raw.githubusercontent.com/C2SP/wycheproof/main/testvectors_v1
for f in rsa_signature_2048_sha256_test rsa_signature_3072_sha256_test rsa_signature_3072_sha512_test rsa_signature_4096_sha512_test rsa_signature_2048_sha512_test ed25519_test; do
  get "$W/$f.json" "$f.json"
done
