# Layer 01-arc: the archive library (libarc) and its tools, on main 6f121c3

Source: .worktrees/arc-main2 (working tree on 3e5abe7, with the documentation session's edits).
Applied to a copy of main 6f121c3 in .worktrees/pkgland (HEAD there is still 064d4cb: the
tree was brought to 6f121c3 file by file, no git command changed anything; files.txt is
against `git archive 6f121c3`). 49 paths added or changed (files.txt), none deleted.
files.tar holds them all. No kernel file changes.

## Clashes and how they were resolved
- Program names: main has none of ar, tar, gzip, gunzip, zcat, xz, unxz, xzcat, zstd,
  unzstd, zstdcat, sha256sum, sha512sum, md5sum. Main's newer tools (od, install, mktemp,
  basename, ..., nc, ping, ifconfig, netstat, tcpdump, routerd) do not overlap. Nothing removed.
- libc: main's libc/src/tools_compat.c already defines (weak) chmod, fchmod, chown, lchown,
  fchown, fsync, umask. arc_compat.c no longer defines chmod, fchmod, chown, lchown, fchown;
  it keeps symlink(at), link(at), readlink(at), fchmodat, fchownat, utimensat, futimens,
  mknod(at), mkfifo. Each function is defined once. The network stack's libc (net.c,
  sys/socket.h, netdb.h, arpa/inet.h) has no function in common with this layer.
- Headers: sys/stat.h and unistd.h keep main's prototypes; only the missing ones are added.
- abi/abi.h: main's sound and sockets blocks, then the layer's guarded block of link and
  permission numbers (SYS_mknodat 33 ... EOPNOTSUPP 95); no number is defined twice.
- Makefile: three-way merge. The image rule keeps main's $(B)/root/etc/remote-files and
  CC_SYSROOT and adds the ALIASES prerequisite and copy loop.
- Docs (CLAUDE.md, ARCHITECTURE, DECISIONS, DEVELOPMENT, PERFORMANCE, STATUS): the layer's
  edits carried as they are; where main had added text at the same place both are kept
  (PERFORMANCE.md sections renumbered: 11 decompression, 12 how to measure, 13 sound).

## Alias names: small programs instead of copies (new, uc-os-dev's point 4)
The piece installed gzip (99,537 bytes) under 9 more names and sha256sum under 2 by
copying: about 0.9 MB. Now each alias is tools/alias/stub.c built with TARGET=/bin/NAME
(2,841 bytes for gzip's, 2,851 for sha256sum's): it spawns the target with the same argv
(argv[0] included, so the tool picks its mode by name) and fds 0 to 2, waits, and exits with
the target's status (128 + signal when killed). Makefile: ALIASED list, $(B)/alias/% rule,
the copy loop copies the stub. Cost: one extra process per call of an alias name.
Reversible: when the kernel has links, make the alias names links and drop the stub.
The in-OS arc-tests.sh (which calls zcat, xzcat, unxz, sha512sum, md5sum ... by name)
passes 52 of 52 with the stubs.

## Size against main 6f121c3 (full builds)
| | main | 01-arc |
|---|---|---|
| programs built | 95 | 99 (+ar, gzip, sha256sum, tar) |
| names in /bin | 95 | 109 (+4 programs, +10 alias stubs) |
| initrd.tar | 10,803,200 | 11,161,600 (+358,400, +3.3%) |
With copies instead of stubs it was +1,167,360 (on 064d4cb).

## Suites (2026-11-26)
- full build: no warnings
- make test: ktest 124/0, libctest 298/0, sh-tests 238/238, sound 37/0, nettest 89/0, routertest 24/0
- in OS: arc-tests 52 of 52
- native: bash libarc/tests/run.sh 843 checks, 0 failed
