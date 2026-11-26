# pgp: OpenPGP signature verification for apt (libsig), so packages stay authentic over untrusted mirrors, proxies and relays

- **Branch / worktree:** none yet (nothing committed). `.worktrees/pgp`, detached at `main`; staged = libarc + package manager (the base), unstaged and untracked = this work
- **Owner:** pgp agent of uc-os-dev
- **State:** pkgland layers 01-arc, 02-pkg, 03-sig stand on main 6f121c3, all suites green; saved in .worktrees/pkgland/build-layers/NN-name/
- **Updated:** 2026-11-26 20:50
- **Next step:** uc-os-dev reviews the three layers; open point: apt-get/apt-cache/dpkg-query as alias stubs (about 680 KB)
- **Waiting on:** owner decisions (keys); a machine with load < 3 for valid timings

## Done so far
- `libsig/` (new, verification only): OpenPGP packets, v4 keys and signatures, RSA PKCS#1 v1.5 with its own bignum (Montgomery), Ed25519 (RFC 8032), ASCII armor with CRC-24, strict cleartext parser, keyrings with self-signature, binding, back-signature, revocation and expiry checks. About 1,640 lines of C.
- libarc gains SHA-384 and SHA-1 (SHA-1 only for v4 fingerprints, never for checking a signature).
- apt: `apt update` verifies InRelease (or Release + Release.gpg) against the source's keyring before it reads a byte of it; refuses unsigned or badly signed repositories with apt's texts; `[trusted=yes]`, `--allow-insecure-repositories`, `--allow-unauthenticated`, `--allow-downgrade-to-insecure-repositories`, `Signed-By` (files, fingerprints, inline key block); install refuses packages from unauthenticated repositories without `--allow-unauthenticated`. New tool `apt-keyring` (list, verify).
- Verified with real data: Debian trixie, trixie-updates, trixie-security, bookworm; Ubuntu 26.04 resolute and resolute-updates, archive and ports (all InRelease, and Release + Release.gpg), against the keyrings from debian-archive-keyring 2025.1 and ubuntu-keyring 2026.08.18; same verdicts as the host's gpgv, tampered copies all refused for the right reason.
- Inside UC.OS: the real trixie InRelease + real Packages.xz (main amd64) + hello and its 3 dependencies: apt update with no warning, install works; changed InRelease, signature removed, no keys, clock in 2040 all refused (14 of 14).
- Suites (final runs 11:30 to 11:50): libsig 148 checks, 0 failed (vectors.py 5,018 to 7,560 checks depending on the differential time, pgpcases.py 161); libpkg native 321 of 321 (was 272); in OS 49 of 49 (was 37); os-signed 14 of 14; os-real.sh unchanged behaviour (with [trusted=yes]); `bash libarc/tests/run.sh` 843, 0 failed; `make B=build-pgp test`: ktest 109, libctest 290, sh-tests 238.
- No key material is in the tree.

## Needs from others
- sh: inside UC.OS, `apt ... > /dev/null 2> FILE; rc=$?` gives `rc=0` when the program exited 100, while `... 2>&1; echo $?` gives 100 and `false > /dev/null; echo $?` gives 1 (seen 2026-11-26 in `libpkg/tests/os-signed.sh`, which now uses `rc=$(...; echo $?)`). Not investigated further.
- kernel/vfs: the 63-byte name limit forced two-letter repository names in `os-test.sh` (`/pkgtest/ru`, `/pkgtest/rb`): apt's list names plus `.xz.partial` must fit.
- kernel: there is no system call that forces a yield. The longest stretch of verification without a system call is loading one keyring file (about 32 ms in UC.OS for Debian's 9-key `debian-archive-keyring.pgp`; see Measurements). If that matters, libsig could call a hook between keys; nothing to hook to today.

## Documentation notes
Facts for uc-os-docs (all run on 2026-11-26 unless said). Nothing here is in the docs yet.

Library
- New library `libsig/` (`sig.h`, `sig_int.h`, `pgp.c` packets/keys/signatures/policy, `clear.c` armor and cleartext, `rsa.c` bignum and RSA, `ed25519.c`). Linked by every `USE_PKG` program: the Makefile's `pkg_lib` is now `libpkg.a libsig.a`, then `libarc.a`. Builds for the host too. No third-party code; written from RFC 4880, RFC 9580, RFC 8017, RFC 8032, FIPS 180-4.
- `libarc/arc.h` additions at the end: `arc_sha384_init/final` (SHA-512 state) and `arc_sha1_*` (for fingerprints only). STATUS.md's libarc row should list SHA-384 and SHA-1.
- Accepted: v4 signatures; hashes SHA-256, SHA-384, SHA-512; public key algorithms RSA (1, 3; modulus 2048 to 8192 bits, odd exponent >= 3), EdDSA legacy on Ed25519 (22, OID 1.3.6.1.4.1.11591.15.1), Ed25519 (27, v4 key). Refused and reported as unsupported: MD5, SHA-1, RIPEMD-160, SHA-224, SHA-3; v3, v5, v6 signatures; v3, v5, v6 keys (listed with "not supported"); other algorithms and curves; RSA under 2048 bits; small-order Ed25519 keys.
- Signature types: cleartext must be 0x01, detached 0x00; anything else is "unsupported".
- Cleartext rules (clear.c): file must start with the BEGIN PGP SIGNED MESSAGE line (nothing before, not even a blank line); only `Hash:` headers; one empty line; dash-escaped text; the signature armor; nothing after the END line but empty lines. Refused: NUL anywhere, bare CR in a line, an unescaped line starting with "-", a line over 16,384 bytes, unknown hash names, a second block. Canonical text: trailing spaces and tabs removed per line (CR LF and LF line endings both accepted), lines joined by CR LF, no final line ending. Without a `Hash:` header only MD5 counts as declared (RFC 4880 reading, as gpgv does), so such a file cannot verify. CRC-24 optional, checked when present.
- The verifier returns the signed text (dash escapes removed, trailing white space removed, LF endings) and apt parses only that (`release_parse` now refuses anything that still starts with armor).
- Ed25519: S < L required; public key must decode canonically and not have small order (checked at keyring load); check is cofactorless `encode([S]B - [k]A) == R` byte for byte (so non-canonical R never matches). On the ed25519-speccheck cases 0 to 11 only 2 and 3 pass (by design; documented in `libsig/ed25519.c` and checked by `vectors.py`).
- RSA: EMSA-PKCS1-v1_5 checked by building the whole expected encoded message and comparing it (comment in `libsig/rsa.c` explains the forgery class). Not constant time; only public values are handled.
- Key policy (pgp.c header comment): self-signature on a user ID (or direct-key signature) must check with SHA-2; key flags must allow signing; a signing subkey needs a valid binding AND a valid embedded back-signature (0x19); any valid revocation by the primary key (0x20, 0x28) makes the key unusable, whatever its reason code (stricter than RFC 9580's soft revocations); a user ID revoked by 0x30 does not count; designated-revoker revocations are ignored (cannot be checked without the revoker's key); key expiry from the newest valid self-signature or binding; direct-key signatures override key flags and expiry only when they carry them (Debian's keys have five 0x1F signatures that only name revokers; they are not checked, which saves five RSA verifications per key).
- Policy per file (apt's): trusted when at least one signature is GOOD and none is BAD. NO_PUBKEY, EXPKEYSIG, REVKEYSIG, EXPSIG, unsupported and unusable signatures do not count either way. A signature with a future creation time counts as unusable ("is the clock right?"), not bad.
- The Ubuntu 2012 archive key in ubuntu-archive-keyring.gpg has only a SHA-1 self-signature, so it is refused ("its self-signatures use SHA-1 or MD5"). Ubuntu's current archives are signed by the 2018 key only, which works.

apt behaviour (repo.c, keyring.c, cli_apt.c, index.c)
- `apt update`: fetch InRelease; else Release, then Release.gpg. Verify against the source's keyring BEFORE parsing. Success: no message at all (the old "signature NOT checked" warning is gone). Failure: apt's lines, e.g. `W: GPG error: file:/x test InRelease: The following signatures couldn't be verified because the public key is not available: NO_PUBKEY 6ED0E7B82643E131`, `W: GPG error: ...: The following signatures were invalid: BADSIG <keyid> <uid> (<reason>)`, then `E: The repository 'file:/x test InRelease' is not signed.`, `N: Updating from such a repository can't be done securely, and is therefore disabled by default.`, `N: See apt-secure(8) manpage for repository creation and user configuration details.`; nothing is stored, exit 100.
- Keyrings: `Signed-By:` / `[signed-by=]` list of files and fingerprints (comma or space separated; `FPR!` exact key), or a key block written in a deb822 field (" ." lines). Default: `Dir::Etc::Trusted` (trusted.gpg) and `*.gpg`, `*.asc` in `Dir::Etc::TrustedParts` (trusted.gpg.d/); other names get apt's "N: Ignoring file ..." note, except apt's silently ignored suffixes. Signed-By file names are taken under apt's `Dir` (the package root, `/debian` in the image), like every other path of the package tools; real apt uses them as given. A keybox (.kbx) is refused with a message.
- `[trusted=yes]`: no check; the loud warning stays: `W: The signature of URI SUITE was NOT checked (the source is marked trusted=yes), ...`. Packages count as authenticated.
- `--allow-insecure-repositories` (`Acquire::AllowInsecureRepositories`): an unsigned or badly signed repository is used with `W: The repository '... Release' is not signed.` + `N: Data from such a repository can't be authenticated ...` + the loud warning; its text is stored as lists/..._Release only (no InRelease, no Release.gpg), which marks its packages unauthenticated. A repository that was signed before (InRelease or Release.gpg in lists/) and is not now is refused even then (`E: The repository '...' is no longer signed.`) unless `--allow-downgrade-to-insecure-repositories`.
- `--allow-unauthenticated` (`APT::Get::AllowUnauthenticated`): needed to install or download packages from an unauthenticated repository. Without it apt prints `WARNING: The following packages cannot be authenticated!` and the names, then with -y `E: There were unauthenticated packages and -y was used without --allow-unauthenticated` (exit 100), without -y asks "Install these packages without verification? [y/N]". It does NOT make `apt update` accept an unsigned repository (apt's semantics; the task text listed it for update too).
- Valid-Until and Date checks are unchanged in code and now read the verified text only.
- Release + Release.gpg: both are kept in lists/ when they verified.
- A malformed InRelease is refused even for `[trusted=yes]` sources (the old parser accepted anything between the markers); before, the test fixtures used a fake "not a real signature" block, which is now refused everywhere.
- Test clock: `-o Debug::Acquire::Signature-Time=EPOCH` (expiry checks only; Valid-Until still uses the real clock).
- New program `apt-keyring` (bin/apt-keyring, USE_PKG, logic in libpkg/keyring.c): `apt-keyring list` (default keyrings in apt-key's format, then which keys each source uses), `apt-keyring list FILE...`, `apt-keyring verify FILE [SIG] [--keyring FILE]...` (exit 0 trusted, 1 not, 2 not a signed file). Takes `-o Key=Value` like apt.
- Image: if a top-level `keyrings/` directory exists (or `make KEYRINGS=DIR`), its `*.gpg *.asc *.pgp` are copied to `/debian/etc/apt/trusted.gpg.d/`. The tree has no such directory and ships no key.
- apt manual text should mention: apt-secure behaviour above, the new options, Signed-By forms, `apt-keyring`, `Debug::Acquire::Signature-Time`.

Tests
- `sh libsig/tests/fetch-vectors.sh` (downloads RFC 8032, RFC 9580, ed25519-speccheck cases.json, Wycheproof RSA PKCS#1 2048/3072/4096 SHA-256/512 and Ed25519 into build-sigvec/).
- `sh libsig/tests/run.sh [OUT]`: 148 checks, 0 failed (includes the RFC 9580 A.3/A.6 version 6 samples: listed and reported as unsupported, text extracted correctly) (vectors.py: 7,358 checks; pgpcases.py: 161; GnuPG interop; real archives). Needs python3 + the `cryptography` package for pgpcases (it signs test data; test code only), the host's gpg/gpgv for interop, and the files from `libpkg/tests/fetch-signed.sh` and `libpkg/tests/fetch-keyrings.sh` for the real-archive part (each part says SKIP if missing).
- `libsig/tests/fuzz.c` modes keyring, armor, clear (with an oracle: a mutated message that still verifies must return the seed's text), detached, bignum.
- `libsig/tests/os-bench.sh BUILD KEYRING INRELEASE...`: timings inside UC.OS.
- `sh libpkg/tests/run.sh /ABS/OUT`: now 321 checks (adds 49 signature checks; NEEDS the host's gpg, which signs the test repositories).
- `sh libpkg/tests/os-test.sh BUILD`: now 49 checks (needs the host's gpg).
- New `sh libpkg/tests/os-signed.sh BUILD [DATA]`: downloads the current trixie InRelease, main/binary-amd64/Packages.xz and hello + 3 dependencies (13 MB, deb.debian.org) once, then 14 checks inside UC.OS.
- `sh libpkg/tests/fetch-keyrings.sh [OUT]` and `sh libpkg/tests/fetch-signed.sh [OUT]`: the downloads above (build-keyrings/, build-signed/).

## Assumptions and questions for the owners
1. **Which keys ship in the image.** (a) Debian's and Ubuntu's archive keys (as debian-archive-keyring and ubuntu-keyring install them): `apt update` from the official archives and from any mirror, proxy or relay verifies out of the box; the project then decides when to update the keys (Debian rotates per release; keys expire: trixie 2035-03-28, trixie stable release key 2033-03-22, bookworm 2031; Ubuntu's 2018 key has no expiry), and the image vouches for keys it did not make. (b) None: every source fails until the user adds a keyring (by `Signed-By` or trusted.gpg.d); safest default, but no repository works out of the box, and the user must get the key over some channel they trust. (c) The club's own key, for a club-run mirror that re-signs: the club controls the chain end to end, but then the club's key and its signing process become the root of trust for every user (key custody, rotation, revocation become club duties), and the mirror must re-sign every InRelease (it cannot reuse Debian's signatures). (a) and (c) can be combined. Mechanism ready for all: put files in `keyrings/` (or `make KEYRINGS=DIR`). Fingerprints to compare are printed by `libpkg/tests/fetch-keyrings.sh`.
2. **Default when no keyring is configured.** Assumed: refuse (apt's behaviour; most conservative). Alternative: warn and continue, which re-opens the hole the proxy plan depends on.
3. **Should `[trusted=yes]` exist at all here?** Kept (apt has it, tests and local repositories use it) with the loud warning. Options: keep; remove (only `--allow-insecure-repositories` per run); keep but only for file: URIs.
4. **Where the library lives and its name.** Assumed `libsig/` with `sig.h` and `sig_*` names, a separate library linked by USE_PKG, because it is not about packages (git or other tools could use it) and it stays testable alone. Options: `libsig/`, `libpgp/` or `libopenpgp/`, or files inside `libpkg/`. Renaming is mechanical.
5. **Name of the keyring tool.** Assumed `apt-keyring` (apt-key is deprecated in Debian and means something else). Options: `apt-keyring`, `apt-keys`, a subcommand such as `apt keyring list`, or none.
6. **Signed-By paths under Dir.** Assumed root-relative (the package tools' rule); real apt uses them as given. Under UC.OS the root is `/debian`.
7. **Revocations.** Assumed every valid revocation disables the key, whatever its reason (stricter than RFC 9580). Alternative: honour "superseded"/"retired" only for signatures made after the revocation.
8. **SHA-1 self-signatures.** Assumed refused everywhere (the Ubuntu 2012 key becomes unusable; current Ubuntu suites use the 2018 key). Alternative: accept SHA-1 for self-signatures made before a cut-off date, as Sequoia's policy does.
9. **Version 6.** No current Debian or Ubuntu key or signature uses v6 (checked: every signature in the 10 InRelease files and every key in both keyrings is v4). v6 is parsed far enough to say "not supported". Adding it is a later choice.

## Log
### 2026-11-26 10:20
Read the base. Fetched (generic user agent): debian-archive-keyring 2025.1, ubuntu-keyring 2026.08.18, InRelease/Release/Release.gpg for debian trixie, trixie-updates, trixie-security, bookworm, ubuntu resolute, resolute-updates (archive and ports). Decoded with `gpg --list-packets`: trixie InRelease has 3 v4 signatures over SHA-256 (RSA-4096 bookworm automatic subkey 6ED0E7B82643E131, RSA-4096 trixie automatic subkey 78DBA3BC47EF2265, EdDSA legacy trixie stable release key 762F67A0B2C39DE4); trixie-security 2 (bookworm-security and trixie-security subkeys, SHA-256); bookworm 3 (same automatic keys + bookworm stable Ed25519); Ubuntu archive and ports 1 (RSA-4096 2018 key 871920D1991BC93C, SHA-512). Release.gpg is still published by both (armored). Debian keys: v4, RSA-4096 primary with RSA-4096 signing subkey and back-signature, five direct-key signatures naming designated revokers; Ed25519 release keys are primary-only. Ubuntu 2012 key: SHA-1 self-signature only.

### 2026-11-26 10:45
libsig written; first run verified every real InRelease and Release.gpg. vectors.py: all Wycheproof RSA (valid accepted, invalid refused; 5 "acceptable" refused), RFC 8032 7.1 (5 vectors x 5 checks), RFC 9580 A.2, Wycheproof Ed25519 (117 valid, 64 invalid), speccheck as designed, e=3 Bleichenbacher forgery (checked to fool a naive parser) refused, RSA/bignum differential vs Python integers.

### 2026-11-26 11:00
pgpcases.py: 161 checks; gpgv accepts every file the test writer made to be valid, except: no Hash header (gpgv: "signature digest conflict") which made me adopt the RFC 4880 reading (no Hash header = MD5 declared = cannot verify), and two signatures with one unknown key (gpgv exits 2 on any missing key; apt reads gpgv's status lines instead).
Fuzzing (ASan+UBSan, 55 s each, load about 4.3): keyring 3,364 inputs (slow: RSA-4096 self-signatures), armor + cleartext split 4,035,008, clear 36,834 (4 still verified, all with the seed's own text), detached 196,800 (843 still verified: unsigned areas and armor headers), bignum 7,476 vs the slow implementation. No sanitizer report, no oracle failure.
ASan found one bug in my apt integration: the result's user ID pointed into a keyring already freed; `sig_result.uid` is now a copied array.

### 2026-11-26 11:15
apt integrated; native 321 of 321; in OS 49 of 49; os-signed 14 of 14 with the real trixie data.

### 2026-11-26 11:20 Measurements (ALL PENDING: load average was 3.5 to 5.2 during every run, above the limit of 3)
- Native, cc -O2: RSA-4096 e=65537 verification 1.82 ms (best batch of 10; first 1.82); Ed25519 0.70 ms. Full cleartext check, trixie-security InRelease with its key: 2.5 ms; trixie InRelease with the stable key: 1.7 ms; Ubuntu resolute: 2.8 ms (before the hash-once change).
- UC.OS under Node (sigtool built for wasm, libsig/tests/os-bench.sh): RSA-4096 1.48 ms best (first 1.68), Ed25519 0.49 ms best (first 0.58); trixie InRelease (3 signatures) 5.0 ms best (8.0 first); trixie-security 3.4 ms; loading the whole debian-archive-keyring.pgp (9 primary keys, 15 keys and subkeys, every self-signature, binding and back-signature checked) 32 ms, with no system call in between.
- Code size: apt for UC.OS 241,661 bytes before, 291,558 after (+49,897, +20.6%; libsig.a is 54 KiB as an archive); apt-keyring 86 KiB.
- Memory: not measured precisely. By construction a cleartext check allocates about 3 times the file size (canonical text, returned text, signature bytes); a key costs its packet plus about 200 bytes.

### 2026-11-26 11:50
Final runs: libsig 148/0; libpkg native 321/321 (no sanitizer report in the log); os-test 49/49; os-signed 14/14; os-real.sh with [trusted=yes] behaves as arcpkg.md describes (Debian install, Ubuntu dpkg -i, name-length failures with exit 100 and 1); libarc 843/0; make test: ktest 109, libctest 290, sh-tests 238. Optional scripts changed so they keep working under the new default: os-real.sh and os-bigindex.sh mark their self-made, unsigned repositories [trusted=yes]; real-repos.sh uses the real keyrings from fetch-keyrings.sh when present, else [trusted=yes] (real-repos.sh and os-bigindex.sh were NOT re-run).

### 2026-11-26 11:52 Measurements
- VALID (load 2.97 at the start): native cc -O2, sigtool: RSA-4096 e=65537 SHA-512 verification 1.31 ms (first and best batch of 10); Ed25519 0.50 ms; loading debian-archive-keyring.pgp 25 ms; whole trixie InRelease check (3 signatures, 138 KB) 4.0 ms best, 4.2 first; Ubuntu resolute InRelease 1.6 ms best, 2.0 first.
- PENDING (load 3.1 to 3.5): UC.OS under Node: RSA-4096 1.67 ms best (2.0 first); Ed25519 0.50 ms best (0.62 first); trixie InRelease 5.2 ms best (8.1 first); trixie-security 3.6 ms; Ubuntu resolute 2.7 ms best (4.4 first); keyring loads: debian-archive-keyring.pgp 33 to 34 ms, debian-archive-trixie-automatic.asc 9.6 ms, ubuntu-archive-keyring.gpg 2.6 ms. Earlier runs at load 4.5 to 5 gave the same values within 15%.
- Syscall-free stretches: one keyring file is parsed and checked without a system call (up to about 34 ms for the 9-key Debian file; apt's default trusted.gpg.d set is one key per file, about 10 ms each); a cleartext check is about 5 ms. No yield was added (see Needs from others).
- Memory (native, /usr/bin/time): max RSS 1.9 MB with the Debian keyring loaded, 2.1 to 2.3 MB after also checking the 138 KB trixie InRelease.
- Code size: apt for UC.OS 241,661 -> 291,558 bytes (+49,897, +20.6%); baseline linked from the staged sources with the same flags.

### 2026-11-26 12:16 (note from uc-os-dev)
My verification run of this stream (fresh build, all suites pass: see BOARD) left four idle daemons behind: gpg-agent and scdaemon started by the test scripts under `build-vfy-pkgnative/.../gnupg` and `build-vfy-os/gnupg`. The suites (`libpkg/tests/run.sh`, `os-test.sh`, `os-signed.sh`) should stop the agent they start when they finish (`gpgconf --homedir DIR --kill gpg-agent`) so that nothing outlives a test run. To fix at the next touch of this stream.

### 2026-11-26 14:41 pkgland job, paused on request
Owner delegated: Debian keys ship, riscv64 default, libsig/apt-keyring names kept, no keyring = refused, [trusted=yes] stays.
Done in .worktrees/pkgland (detached 064d4cb; no git command that changes anything):
- Baseline main 064d4cb built in build-mainsrc/build-m (pristine copy from git archive): 87 programs, initrd.tar 10,455,040 bytes, no warnings.
- P1 (archive library from arc-main2) applied: 38 added files copied; Makefile, abi/abi.h, libc headers, CLAUDE.md, docs merged three-way (git merge-file, base 3e5abe7). Conflicts: Makefile initrd rule (main's remote-files prerequisite kept, ALIASES added; ALIASES copy loop added after the program copy); abi.h (main's sound block, then arc's guarded links block); stat.h/unistd.h (main's tools_compat prototypes kept, arc's extra ones added, duplicates chmod/fchmod/chown/lchown/fchown dropped); docs: both sides kept (PERFORMANCE renumbered 11 libarc, 12 how to measure, 13 sound). libc: arc_compat.c no longer defines chmod, fchmod, chown, lchown, fchown (main's tools_compat.c has them, weak); each function once. No program name clashes: main has no ar, gzip (or its 9 aliases), sha256sum/sha512sum/md5sum, tar.
- P1 full build (build-p1): no warnings; 91 programs built (+4) and 101 names in /bin (+14: 10 are ALIASES copies); initrd.tar 11,622,400 bytes (+1,167,360, +11.2%; gzip 99,537 bytes copied under 9 more names is about 0.9 MB of it, the piece's decided design: no links in the kernel). make test on P1: ktest 124/0, libctest 298/0, sh-tests 238/238, sound 37/0. No kernel file changed.
- Not yet run on P1: libarc native suite and arc-tests.sh in the OS; layer not saved yet.
- P2 staged in build-layers/p2stage + build-layers/merge2 (merged Makefile, abi.h, headers, docs): riscv64 default (libpkg/conf.c, rootfs/etc/dpkg/dpkg.cfg), tests switched to binary-riscv64 and riscv64; in-OS repository moved to /pkgtest/r (binary-riscv64 makes apt's partial name 64 bytes at /pkgtest/repo, over the kernel's 63); pkg_compat.c keeps only fdatasync (fsync, umask are in main's tools_compat.c).
- P3 staged in build-layers/p3stage (p3.diff against the pkg base, added files, new libpkg/tests/keyrings-check.sh).
- Keys: debian-archive-keyring_2025.1_all.deb re-fetched (sha256 9ea7778e443144ca490668737a8ab22dd3e748bb99e805e22ec055abeb3c7fac), ftp-master.debian.org/keys.html fetched into pkgland/build-keys. Plan: ship the three trixie .asc files (archive, security, stable release key).

### 2026-11-26 20:50 pkgland layers on main 6f121c3 (resumed 19:4x)
Tree brought from 064d4cb to 6f121c3 file by file (git merge-file for the 7 files P1 also
touched; abi.h and STATUS.md had adjacent additions, both kept). Layers saved with
files.txt (cumulative against `git archive 6f121c3`), deleted.txt (empty), files.tar, NOTES.md.
- 01-arc: 49 paths. Alias names are now 2.8 KB stub programs (tools/alias/stub.c, TARGET set
  by the Makefile) instead of full copies. Image 11,161,600 vs main 10,803,200 (+3.3%; with
  copies +1.17 MB). make test (ktest 124, libctest 298, sh 238, sound 37, nettest 89,
  routertest 24) green; arc-tests 52/52 in the OS; libarc native 843/0.
- 02-pkg: 93 paths. riscv64 default; in-OS repository /pkgtest/r (64-byte list name at
  /pkgtest/repo). Image 12,400,640 (+1,239,040). make test green; native 272/272; in OS 37/37.
- 03-sig: 116 paths. Debian's three trixie keys in keyrings/ (fingerprints on
  ftp-master.debian.org/keys.html, checked by libpkg/tests/keyrings-check.sh, which also
  verifies today's InRelease with them); gpg-agent stopped by the test scripts. Image
  12,666,880 (+266,240). make test green; libsig 148/0; native 321/321; in OS 49/49;
  os-signed 16/16.
- End to end on main: install of real signed riscv64 packages works (with the shipped keys
  and with a test key); lx busybox (static) works; lx hello fails ("cannot load ... error 2",
  status 137) because /lib -> usr/lib from base-files cannot exist without symbolic links.
  Documented limitation (uc-os-dev's choice), no dpkg fallback.
Left on disk (git-ignored, pkgland): build-base3e, build-main064, build-main6f (+build-m),
build-mainsrc (+build-m), build-p1, build-a1, build-a2, build-a2os, build-a3, build-a3os,
build-a3-signed, build-a3-e2ek, build-arctest, build-pkgnative, build-pkgnative3, build-sig,
build-keys, build-keyrings-check, build-signed-repo, build-layers (also, by mistake, an
extracted copy of main 064d4cb's files directly inside build-layers/).

## Documentation notes (pkgland, 2026-11-26 20:50)
- The package manager's default architecture is riscv64 (libpkg/conf.c, dpkg.cfg); every
  doc line that says "amd64 (placeholder)" is now false (DECISIONS row, ARCHITECTURE
  section 17 "The architecture is one value", dpkg.cfg comment updated in the layer).
- Alias names in /bin (gunzip, zcat, xz, unxz, xzcat, zstd, unzstd, zstdcat, sha512sum,
  md5sum) are small stub programs (tools/alias/stub.c, about 2.8 KB) that spawn the real
  program with the same argv; DECISIONS' "installed under each name by copying" is now
  "by a stub that starts the program"; Makefile: ALIASED, $(B)/alias/%.
- Image keyrings: keyrings/ holds Debian's trixie archive, security and stable release keys
  (fingerprints above), copied to /debian/etc/apt/trusted.gpg.d/; so `apt update` from
  deb.debian.org-style trixie repositories verifies out of the box (file: only today).
- libpkg/tests/keyrings-check.sh (network): fingerprints vs ftp-master.debian.org/keys.html
  and today's InRelease vs the shipped keys.
- os-signed.sh now uses riscv64 data, the shipped keys and lx; it needs a full build (lx).
- On main: Debian packages install; statically linked programs run through lx; dynamically
  linked ones need /lib -> usr/lib and run only once the file system layer has landed.
- ARCHITECTURE section numbers: 15 sound, 16 networking, 17 package manager.
- Two SHA-256 implementations exist: libarc/hash.c (programs) and kernel/remote.c (kernel);
  intended, the kernel does not link libarc.
- apt over the network (not built): fetch.c runs Acquire::http::Command programs (curl, wget),
  none of which exists in UC.OS. It would take an HTTP/1.0 client in libpkg's fetch.c over
  sockets (getaddrinfo + connect to port 80; routerd forwards it, in the browser as https with
  fetch(), so the mirror must allow cross-origin reads: not checked for deb.debian.org; under
  Node real TCP), one request per connection, plus streaming large .debs. Signatures make the
  untrusted path safe; no TLS is needed for integrity.
- Solver note: with gawk and mawk in the index, base-files' Pre-Depends: awk is satisfied
  with gawk even when mawk is named (apt would use mawk).
