# Layer 02-pkg: the package manager, on top of 01-arc (main 6f121c3)

Source: .worktrees/pkg-main2 (working tree on 3e5abe7) minus what 01-arc carries.
files.txt is cumulative against 6f121c3: 93 paths, none deleted. No kernel file changes.

## Changes made while carrying it
- Default architecture riscv64 (owner's delegation: guest architecture riscv64, Debian 13):
  libpkg/conf.c PKG_NATIVE_ARCH "riscv64"; rootfs/etc/dpkg/dpkg.cfg comment and example.
  Tests that assumed amd64: libpkg/tests/run.sh (print-architecture riscv64, the override
  test now sets arm64, nativepkg is riscv64), apt.sh, os-test.sh, os/pkg-os.sh
  (binary-riscv64, Architectures: riscv64).
- In-OS repository moved from /pkgtest/repo to /pkgtest/r: apt's temporary name
  _pkgtest_repo_dists_test_main_binary-riscv64_Packages.xz.partial is 64 bytes, one over
  the kernel's 63 per name.
- libc: pkg_compat.c keeps only fdatasync (fsync and umask are in main's tools_compat.c);
  the duplicate prototypes (fsync in unistd.h, umask in sys/stat.h) are not added.
- abi/abi.h: the layer's block (SYS_fsync 82, SYS_fdatasync 83, SYS_umask 166, ENOTSUP)
  after main's blocks; guarded, no duplicate.
- Docs: the package manager becomes ARCHITECTURE.md section 17 (main has 15 sound,
  16 networking); other doc edits carried as they are (they still say "amd64 placeholder":
  see the stream file's documentation notes).
- Optional scripts with real amd64 data (os-real.sh, os-bigindex.sh, real-repos.sh,
  survey-scripts.py) were not changed for riscv64 and not run.

## Size (full builds)
| | 01-arc | 02-pkg |
|---|---|---|
| names in /bin | 109 | 115 (+dpkg, dpkg-deb, dpkg-query, apt, apt-get, apt-cache) |
| initrd.tar | 11,161,600 | 12,400,640 (+1,239,040) |
Reason: new programs. apt, apt-get and apt-cache are three full copies of the same code
(about 242 KB each) and dpkg and dpkg-query two (198 KB): made alias stubs they would save
about 680 KB (not done: it needs the three directories removed, which is the owner's or
uc-os-dev's call; proposed in the report).

## Suites
- full build: no warnings; make test: ktest 124, libctest 298, sh 238, sound 37, nettest 89, routertest 24, all 0 failed
- native: sh libpkg/tests/run.sh: 272 of 272
- in OS: sh libpkg/tests/os-test.sh: 37 of 37
