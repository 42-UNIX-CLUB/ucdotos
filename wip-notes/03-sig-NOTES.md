# Layer 03-sig: OpenPGP signature checks for apt (libsig, apt-keyring) and Debian's archive keys, on top of 02-pkg (main 6f121c3)

Source: .worktrees/pgp (verified by uc-os-dev), carried as a patch (p3stage/p3.diff) plus
its added files. files.txt is cumulative against 6f121c3: 116 paths, none deleted. No kernel
file changes. Three hunks needed hand work: Makefile (the image rule now also carries the
alias stubs), apt.sh (riscv64 index names), os/pkg-os.sh (/pkgtest/r, riscv64).

## Added in this round
- keyrings/: Debian's three trixie archive keys, from debian-archive-keyring 2025.1
  (deb.debian.org, sha256 9ea7778e443144ca490668737a8ab22dd3e748bb99e805e22ec055abeb3c7fac,
  equal to the SHA256 of debian-archive-keyring 2025.1 in trixie's main/binary-all
  Packages.xz, whose own hash is in today's signed trixie InRelease):
  | file | bytes | sha256 | primary key fingerprint |
  |---|---|---|---|
  | debian-archive-trixie-automatic.asc | 11,861 | 6f1d2774...b0da7 | 04B5 4C3C DCA7 9751 B16B C6B5 2256 29DF 75B1 88BD |
  | debian-archive-trixie-security-automatic.asc | 11,873 | 844c07d2...808b7 | 5E04 A1E3 223A 19A2 0706 E20F 9904 613D 4CCE 68C6 |
  | debian-archive-trixie-stable.asc | 1,384 | 4d097bb9...d198b | 4158 7F7D B8C7 74BC CF13 1416 762F 67A0 B2C3 9DE4 |
  Compared programmatically with https://ftp-master.debian.org/keys.html (fetched 2026-11-26
  20:2x, generic client): all three are listed there, as "Debian 13/trixie archive signing
  key", "security archive signing key" and "Debian 13/trixie release key". The bookworm and
  bullseye keys in the package are not shipped (trixie verifies without them).
  The Makefile copies keyrings/* to /debian/etc/apt/trusted.gpg.d/ (seen in the image).
- libpkg/tests/keyrings-check.sh: compares every shipped fingerprint with Debian's page and
  verifies today's trixie InRelease with the shipped keys only (network). Result: 3 of 3
  listed, InRelease trusted (2 good: trixie automatic subkey and trixie release key; the
  bookworm key's signature is NO_PUBKEY, which does not count against it).
- gpg-agent: libpkg/tests/apt.sh, os-test.sh and libsig/tests/run.sh stop the agent they
  start (gpgconf --homedir DIR --kill gpg-agent); no agent was left after the runs.
- os-signed.sh: riscv64 (binary-riscv64), the shipped keys, hello + base-files + mawk +
  busybox-static, and the engine.
- SHA-256 exists twice: libarc/hash.c (programs) and kernel/remote.c (kernel). Left as is:
  the kernel does not link libarc.

## Size (full builds)
| | 02-pkg | 03-sig |
|---|---|---|
| names in /bin | 115 | 116 (+apt-keyring) |
| initrd.tar | 12,400,640 | 12,666,880 (+266,240) |
Reason: libsig in apt, apt-get, apt-cache (about +50 KB each, three copies), apt-keyring
87,781 bytes, the keys 25,118 bytes.

## Suites
- full build: no warnings; make test: ktest 124, libctest 298, sh 238, sound 37, nettest 89, routertest 24, all 0 failed
- native: libsig/tests/run.sh 148 passed, 0 failed (vectors.py 5,832 checks, pgpcases 161, gpg interop, real archives); libpkg/tests/run.sh 321 of 321, no sanitizer report
- in OS: os-test.sh 49 of 49; os-signed.sh 16 of 16; keyrings-check.sh ok

## End to end on main (UC.OS under Node, build-a3)
1. Real signed archive (os-signed.sh, log os-signed.log): today's trixie InRelease + real
   main/binary-riscv64/Packages.xz in a file: repository, checked with the SHIPPED keys:
   apt update with no warning; apt install -y --no-install-recommends hello base-files mawk
   busybox-static installs 14 packages (also gawk and its 6 libraries: see below);
   `lx --root=/s/r /usr/bin/busybox echo signed and installed` prints it (statically linked:
   WORKS); `lx --root=/s/r /usr/bin/hello` fails: "lx: cannot load /usr/bin/hello (error 2)",
   the shell prints "Killed", status 137. /s/r/lib does not exist; /s/r/usr/lib/ld-linux-
   riscv64-lp64d.so.1 does (dpkg copied the file the libc6 link points to). Refusals: changed
   InRelease (BADSIG), signature removed, no keys (NO_PUBKEY), clock in 2040 (EXPKEYSIG): all
   exit 100, nothing stored.
2. Test key (scripts/e2e-testkey.sh, log e2e-testkey.log): the integration engineer's
   repository (hello, libc6, libgcc-s1, gcc-14-base, base-files, mawk) signed with a
   throw-away ed25519 key: apt update clean, apt install -y hello base-files: 6 packages ii,
   `lx /usr/bin/hello` fails the same way.
Why: base-files ships /lib -> usr/lib as a symbolic link; main's kernel has no symbolic
links, so dpkg skips it (a link to a directory cannot become a copy). hello's ELF
interpreter is /lib/ld-linux-riscv64-lp64d.so.1. Decision (uc-os-dev, owner's delegation):
documented limitation, no fallback in dpkg. Debian programs install on main; those that need
/lib -> usr/lib (every dynamically linked one) run once the file system layer has landed;
statically linked ones (busybox-static) run now.
Solver finding: with gawk and mawk both in the index, the package manager installs gawk for
base-files' Pre-Depends: awk even when mawk is named on the command line (apt would let the
named mawk satisfy it). Not changed here.
